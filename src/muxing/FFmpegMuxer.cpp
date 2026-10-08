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
#include <QMap>

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

    // Bound how far one stream may lag behind the others in the
    // interleave queue (default is 10s). A stalled audio stream would
    // otherwise pin many megabytes of video in RAM — invisible on disk
    // until the trailer flush — which makes live size readouts lag the
    // true size badly and loses more footage on crash. 1s is plenty
    // for local file output.
    m_formatCtx->max_interleave_delta = 1000000;

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
        // Clear codec tag to avoid container-specific fourcc confusion
        codecpar->codec_tag = 0;

        // Prefer an explicit pixel format when known. If the encoder did not
        // provide one, fall back to a common container-friendly format
        // (YUV420P) for compressed codecs so demuxers can determine image
        // layout correctly.
        if (info.pixelFormat != 0) {
            codecpar->format = info.pixelFormat;
        } else if (info.codecId == AV_CODEC_ID_H264 ||
               info.codecId == AV_CODEC_ID_HEVC ||
               info.codecId == AV_CODEC_ID_AV1) {
            codecpar->format = AV_PIX_FMT_YUV420P;
        } else {
            codecpar->format = AV_PIX_FMT_NONE;
        }

        // Also provide default color/imaging metadata which demuxers
        // sometimes require to determine pixel format correctly.
    #ifdef AVCOL_PRI_BT709
        codecpar->color_primaries = AVCOL_PRI_BT709;
    #endif
    #ifdef AVCOL_TRC_BT709
        codecpar->color_trc = AVCOL_TRC_BT709;
    #endif
    #ifdef AVCOL_SPC_BT709
        codecpar->colorspace = AVCOL_SPC_BT709;
    #endif
    #ifdef AVCHROMA_LOC_CENTER
        codecpar->chroma_location = AV_CHROMA_LOC_CENTER;
    #endif

        codecpar->bit_rate = static_cast<int64_t>(info.bitrate) * 1000;
        stream->time_base = {1, 1000};
        stream->avg_frame_rate = {info.fps, 1};
        stream->r_frame_rate = {info.fps, 1};
    } else {
        codecpar->codec_type = AVMEDIA_TYPE_AUDIO;
        codecpar->codec_id = static_cast<AVCodecID>(info.codecId);
        codecpar->sample_rate = info.sampleRate;
        av_channel_layout_default(&codecpar->ch_layout, info.channels);
        codecpar->bit_rate = static_cast<int64_t>(info.bitrate) * 1000;
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

bool FFmpegMuxer::setMetadata(const QMap<QString, QString> &metadata) {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (!m_formatCtx) return false;

    for (auto it = metadata.constBegin(); it != metadata.constEnd(); ++it) {
        const QByteArray key = it.key().toUtf8();
        const QByteArray value = it.value().toUtf8();
        if (key.isEmpty() || value.isEmpty())
            continue;
        av_dict_set(&m_formatCtx->metadata, key.constData(), value.constData(), 0);
    }
    return true;
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
    } else if (QString(m_formatCtx->oformat->name).contains("matroska")) {
        // Flush clusters every second instead of the 5s default: bounds
        // how much encoded data sits in muxer RAM (invisible to live
        // size readouts and lost on crash) to ~1s of content.
        av_dict_set(&opts, "cluster_time_limit", "1000", 0);
    }

    int ret = avformat_write_header(m_formatCtx, &opts);
    av_dict_free(&opts);

    if (ret < 0) return false;

    m_headerWritten = true;
    return true;
}

bool FFmpegMuxer::updateStreamExtraData(int streamIndex, const std::vector<uint8_t> &data) {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (!m_formatCtx) return false;
    if (streamIndex < 0 || streamIndex >= static_cast<int>(m_streams.size())) return false;
    AVStream *stream = m_streams[streamIndex];
    AVCodecParameters *codecpar = stream->codecpar;

    // Free existing extradata if present
    if (codecpar->extradata) {
        av_free(codecpar->extradata);
        codecpar->extradata = nullptr;
        codecpar->extradata_size = 0;
    }

    if (!data.empty()) {
        codecpar->extradata_size = static_cast<int>(data.size());
        codecpar->extradata = static_cast<uint8_t *>(av_mallocz(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        memcpy(codecpar->extradata, data.data(), data.size());
    }

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