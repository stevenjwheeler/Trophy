/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "FFmpegMuxer.h"
#include "encoding/IEncoder.h"

#include <QDebug>
#include <QFileInfo>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace GameRecorder {

FFmpegMuxer::FFmpegMuxer(QObject *parent) : IMuxer(parent) {}

FFmpegMuxer::~FFmpegMuxer() { cleanup(); }

bool FFmpegMuxer::initialize(const QString &outputPath) {
  m_outputPath = outputPath;

  // Determine format from extension
  QString ext = QFileInfo(outputPath).suffix().toLower();
  const char *formatName = nullptr;

  if (ext == "mkv") {
    formatName = "matroska";
  } else if (ext == "mp4") {
    formatName = "mp4";
  } else if (ext == "webm") {
    formatName = "webm";
  } else {
    formatName = "matroska"; // Default to MKV
  }

  int ret = avformat_alloc_output_context2(&m_formatCtx, nullptr, formatName,
                                           outputPath.toUtf8().constData());
  if (ret < 0 || !m_formatCtx) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    qCritical() << "Failed to create output context:" << err;
    return false;
  }

  qInfo() << "Muxer initialized for" << formatName << "output";
  return true;
}

int FFmpegMuxer::addStream(const StreamInfo &info) {
  if (!m_formatCtx) {
    emit muxingError("Muxer not initialized");
    return -1;
  }

  AVStream *stream = avformat_new_stream(m_formatCtx, nullptr);
  if (!stream) {
    emit muxingError("Failed to create stream");
    return -1;
  }

  stream->id = m_formatCtx->nb_streams - 1;

  AVCodecParameters *codecpar = stream->codecpar;

  if (info.type == StreamInfo::Type::Video) {
    codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
    codecpar->codec_id = static_cast<AVCodecID>(info.codecId);
    codecpar->width = info.width;
    codecpar->height = info.height;
    stream->time_base = {1, 1000000};

    if (!info.extraData.empty()) {
      codecpar->extradata_size = static_cast<int>(info.extraData.size());
      codecpar->extradata = static_cast<uint8_t *>(
          av_mallocz(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
      memcpy(codecpar->extradata, info.extraData.data(),
             codecpar->extradata_size);
    }
  } else if (info.type == StreamInfo::Type::Audio) {
    codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
    codecpar->codec_id = static_cast<AVCodecID>(info.codecId);
    codecpar->sample_rate = info.sampleRate;
    av_channel_layout_default(&codecpar->ch_layout, info.channels);
    codecpar->bit_rate = info.bitrate * 1000;
    stream->time_base = {1, info.sampleRate};

    if (!info.extraData.empty()) {
      codecpar->extradata_size = static_cast<int>(info.extraData.size());
      codecpar->extradata = static_cast<uint8_t *>(
          av_mallocz(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
      memcpy(codecpar->extradata, info.extraData.data(),
             codecpar->extradata_size);
    }
  }

  m_streams.push_back(stream);

  qInfo() << "Added stream" << stream->id
          << (info.type == StreamInfo::Type::Video ? "video" : "audio");

  return stream->id;
}

bool FFmpegMuxer::writeHeader() {
  if (!m_formatCtx) {
    emit muxingError("Muxer not initialized");
    return false;
  }

  // Open output file
  if (!(m_formatCtx->oformat->flags & AVFMT_NOFILE)) {
    int ret = avio_open(&m_formatCtx->pb, m_outputPath.toUtf8().constData(),
                        AVIO_FLAG_WRITE);
    if (ret < 0) {
      char err[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, err, sizeof(err));
      emit muxingError(QString("Failed to open output file: %1").arg(err));
      return false;
    }
  }

  // Set options for the format
  AVDictionary *opts = nullptr;

  // For MP4, enable faststart for streaming
  if (QString(m_formatCtx->oformat->name) == "mp4") {
    av_dict_set(&opts, "movflags", "faststart", 0);
  }

  int ret = avformat_write_header(m_formatCtx, &opts);
  av_dict_free(&opts);

  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    qCritical() << "avformat_write_header failed for"
                << m_formatCtx->oformat->name << ":" << err;

    // Log stream info for debugging
    for (unsigned int i = 0; i < m_formatCtx->nb_streams; i++) {
      AVStream *s = m_formatCtx->streams[i];
      qCritical() << "Stream" << i << "type:" << s->codecpar->codec_type
                  << "codec:" << s->codecpar->codec_id
                  << "res:" << s->codecpar->width << "x" << s->codecpar->height
                  << "timebase:" << s->time_base.num << "/" << s->time_base.den;
    }

    emit muxingError(QString("Failed to write muxer header: %1").arg(err));
    return false;
  }

  m_headerWritten = true;
  qInfo() << "Container header written";
  return true;
}

bool FFmpegMuxer::writePacket(const EncodedPacket &packet) {
  if (!m_formatCtx || !m_headerWritten) {
    return false;
  }

  if (packet.streamIndex < 0 ||
      static_cast<size_t>(packet.streamIndex) >= m_streams.size()) {
    qWarning() << "Invalid stream index:" << packet.streamIndex;
    return false;
  }

  AVPacket *avPacket = av_packet_alloc();
  if (!avPacket) {
    return false;
  }

  if (av_new_packet(avPacket, static_cast<int>(packet.data.size())) < 0) {
    av_packet_free(&avPacket);
    return false;
  }
  memcpy(avPacket->data, packet.data.data(), packet.data.size());

  avPacket->pts = packet.pts;
  avPacket->dts = packet.dts;
  avPacket->stream_index = packet.streamIndex;

  if (packet.isKeyframe) {
    avPacket->flags |= AV_PKT_FLAG_KEY;
  }

  // Rescale timestamps
  AVStream *stream = m_streams[packet.streamIndex];
  av_packet_rescale_ts(avPacket, {1, 1000000}, stream->time_base);

  std::lock_guard<std::mutex> lock(m_writeMutex);

  int ret = av_interleaved_write_frame(m_formatCtx, avPacket);

  m_bytesWritten = avio_tell(m_formatCtx->pb);

  av_packet_free(&avPacket);

  if (ret < 0) {
    char err[AV_ERROR_MAX_STRING_SIZE];
    av_strerror(ret, err, sizeof(err));
    emit muxingError(QString("Failed to write packet: %1").arg(err));
    return false;
  }

  return true;
}

bool FFmpegMuxer::finalize() {
  if (!m_formatCtx) {
    return false;
  }

  if (m_headerWritten) {
    std::lock_guard<std::mutex> lock(m_writeMutex);

    int ret = av_write_trailer(m_formatCtx);
    if (ret < 0) {
      char err[AV_ERROR_MAX_STRING_SIZE];
      av_strerror(ret, err, sizeof(err));
      qWarning() << "Error writing trailer:" << err;
    }

    m_bytesWritten = avio_tell(m_formatCtx->pb);
  }

  cleanup();

  qInfo() << "Muxer finalized. Total bytes:" << m_bytesWritten;
  return true;
}

QString FFmpegMuxer::formatName() const {
  if (m_formatCtx && m_formatCtx->oformat) {
    return QString(m_formatCtx->oformat->name);
  }
  return QString();
}

void FFmpegMuxer::cleanup() {
  if (m_formatCtx) {
    if (m_formatCtx->pb && !(m_formatCtx->oformat->flags & AVFMT_NOFILE)) {
      avio_closep(&m_formatCtx->pb);
    }
    avformat_free_context(m_formatCtx);
    m_formatCtx = nullptr;
  }
  m_streams.clear();
  m_headerWritten = false;
}

} // namespace GameRecorder
