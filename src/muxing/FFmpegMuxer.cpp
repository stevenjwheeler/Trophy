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
    : IMuxer(parent), m_globalOffset(-1) {}

FFmpegMuxer::~FFmpegMuxer() { 
    cleanup(); 
}

bool FFmpegMuxer::initialize(const QString &outputPath) {
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
        // Use millisecond timebase
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
        memcpy(codecpar->extradata, info.extraData.data(), codecpar->extradata_size);
    }

    m_streams.push_back(stream);
    return stream->id;
}

bool FFmpegMuxer::writeHeader() {
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
    if (packet.streamIndex >= static_cast<int>(m_streams.size())) return false;

    AVPacket *avPacket = av_packet_alloc();
    if (!avPacket) return false;

    avPacket->data = (uint8_t *)av_malloc(packet.data.size());
    memcpy(avPacket->data, packet.data.data(), packet.data.size());
    avPacket->size = packet.data.size();
    
    // Normalize timestamps to start at 0 based on the first arriving packet.
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
    
    // Rescale from 1ms internal timebase to stream timebase
    av_packet_rescale_ts(avPacket, {1, 1000}, stream->time_base);
    
    int ret = av_interleaved_write_frame(m_formatCtx, avPacket);
    av_packet_free(&avPacket);

    return ret >= 0;
}

bool FFmpegMuxer::finalize() {
    if (!m_formatCtx) return false;
    
    bool success = true;
    if (m_headerWritten) {
        success = (av_write_trailer(m_formatCtx) >= 0);
    }
    
    cleanup();
    return success;
}

QString FFmpegMuxer::formatName() const {
    if (m_formatCtx && m_formatCtx->oformat) {
        return QString::fromUtf8(m_formatCtx->oformat->name);
    }
    return QStringLiteral("unknown");
}

void FFmpegMuxer::cleanup() {
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

} // namespace TrophyBackend