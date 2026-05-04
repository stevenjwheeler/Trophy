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
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace TrophyBackend {

FFmpegMuxer::FFmpegMuxer(QObject *parent) 
    : IMuxer(parent), 
      m_formatCtx(nullptr), 
      m_headerWritten(false),
      m_globalOffset(-1) {}

bool FFmpegMuxer::initialize(const QString &outputPath) {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    m_outputPath = outputPath;

    QString ext = QFileInfo(outputPath).suffix().toLower();
    const char *formatNameStr = (ext == "mp4") ? "mp4" : "matroska";

    int ret = avformat_alloc_output_context2(&m_formatCtx, nullptr, formatNameStr,
                                           outputPath.toUtf8().constData());
    if (ret < 0 || !m_formatCtx) {
        qCritical() << "Failed to create output context for" << formatNameStr;
        return false;
    }

    return true;
}

int FFmpegMuxer::addStream(const StreamInfo &info) {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (!m_formatCtx) return -1;

    AVStream *stream = avformat_new_stream(m_formatCtx, nullptr);
    if (!stream) return -1;

    stream->id = m_formatCtx->nb_streams - 1;
    AVCodecParameters *codecpar = stream->codecpar;

    if (info.type == StreamInfo::Type::Video) {
        codecpar->codec_type = AVMEDIA_TYPE_VIDEO;
        codecpar->codec_id = static_cast<AVCodecID>(info.codecId);
        codecpar->width = info.width;
        codecpar->height = info.height;
        stream->time_base = {1, 1000};
        stream->avg_frame_rate = {info.fps, 1};
        stream->r_frame_rate = {info.fps, 1};
    } else {
        codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        codecpar->codec_id = static_cast<AVCodecID>(info.codecId);
        codecpar->sample_rate = info.sampleRate;
        av_channel_layout_default(&codecpar->ch_layout, info.channels);
        stream->time_base = {1, 1000};
    }

    if (!info.extraData.empty()) {
        codecpar->extradata_size = static_cast<int>(info.extraData.size());
        codecpar->extradata = static_cast<uint8_t *>(
            av_mallocz(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(codecpar->extradata, info.extraData.data(), info.extraData.size());
    }

    m_streams.push_back(stream);
    return stream->id;
}

bool FFmpegMuxer::writeHeader() {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (!m_formatCtx) return false;

    if (!(m_formatCtx->oformat->flags & AVFMT_NOFILE)) {
        if (avio_open(&m_formatCtx->pb, m_outputPath.toUtf8().constData(), AVIO_FLAG_WRITE) < 0)
            return false;
    }

    AVDictionary *opts = nullptr;
    if (QString(m_formatCtx->oformat->name) == "mp4") {
        av_dict_set(&opts, "movflags", "faststart", 0);
    }

    int ret = avformat_write_header(m_formatCtx, &opts);
    av_dict_free(&opts);

    if (ret < 0) return false;

    m_headerWritten = true;
    return true;
}

bool FFmpegMuxer::writePacket(const EncodedPacket &packet) {
    if (!m_formatCtx || !m_headerWritten) return false;
    
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    
    if (packet.streamIndex >= static_cast<int>(m_streams.size())) return false;

    AVPacket *avPacket = av_packet_alloc();
    if (!avPacket) return false;

    if (av_new_packet(avPacket, packet.data.size()) < 0) {
        av_packet_free(&avPacket);
        return false;
    }
    memcpy(avPacket->data, packet.data.data(), packet.data.size());
    
    if (m_globalOffset == -1) {
        m_globalOffset = packet.pts;
    }

    avPacket->pts = packet.pts - m_globalOffset;
    avPacket->dts = (packet.dts == AV_NOPTS_VALUE) ? avPacket->pts : (packet.dts - m_globalOffset);
    avPacket->duration = packet.duration;
    avPacket->stream_index = packet.streamIndex;

    if (packet.isKeyframe) {
        avPacket->flags |= AV_PKT_FLAG_KEY;
    }

    AVStream *stream = m_streams[packet.streamIndex];
    av_packet_rescale_ts(avPacket, {1, 1000}, stream->time_base);
    
    int ret = av_interleaved_write_frame(m_formatCtx, avPacket);
    av_packet_free(&avPacket);

    return ret >= 0;
}

bool FFmpegMuxer::finalize() {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (!m_formatCtx) return false;
    
    bool success = true;
    if (m_headerWritten) {
        // Write the trailer (this updates the file size on disk)
        success = (av_write_trailer(m_formatCtx) >= 0);
        
        // Force a flush to the IO context so avio_tell is accurate
        if (m_formatCtx->pb) {
            avio_flush(m_formatCtx->pb);
        }
    }
    
    return success;
}

// Ensure the destructor still cleans up
FFmpegMuxer::~FFmpegMuxer() { 
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    cleanup_internal(); 
}

int64_t FFmpegMuxer::bytesWritten() const {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (m_formatCtx && m_formatCtx->pb) {
        return avio_tell(m_formatCtx->pb);
    }
    return 0;
}

QString FFmpegMuxer::formatName() const {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (m_formatCtx && m_formatCtx->oformat) {
        return QString::fromUtf8(m_formatCtx->oformat->name);
    }
    return QStringLiteral("unknown");
}

void FFmpegMuxer::cleanup_internal() {
    if (m_formatCtx) {
        if (!(m_formatCtx->oformat->flags & AVFMT_NOFILE) && m_formatCtx->pb) {
            avio_closep(&m_formatCtx->pb);
        }
        avformat_free_context(m_formatCtx);
        m_formatCtx = nullptr;
    }
    m_streams.clear();
    m_headerWritten = false;
    m_globalOffset = -1;
}

void FFmpegMuxer::cleanup() {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    cleanup_internal();
}

} // namespace TrophyBackend