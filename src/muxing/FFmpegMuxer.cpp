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
#include <algorithm>
#include <limits>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libavutil/mathematics.h>
}

namespace TrophyBackend {

namespace {
QString ffmpegErrorText(int err) {
    char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
    av_strerror(err, buffer, sizeof(buffer));
    return QString::fromUtf8(buffer);
}
} // namespace

FFmpegMuxer::FFmpegMuxer(QObject *parent) 
    : IMuxer(parent), 
      m_formatCtx(nullptr), 
      m_headerWritten(false),
      m_globalOffset(-1), m_closeOutput(&avio_closep) {}

bool FFmpegMuxer::initialize(const QString &outputPath) {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    // Re-init must not leak the previous context or carry a stale
    // global offset: reset first (lock-free variant — the mutex is
    // already held, so cleanup() would deadlock).
    cleanup_internal();
    m_outputPath = outputPath;
    m_lastError.clear();

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
    if (!m_formatCtx || m_headerWritten) return -1;

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
        if (info.extraData.size() >
            static_cast<size_t>(std::numeric_limits<int>::max() -
                                AV_INPUT_BUFFER_PADDING_SIZE))
            return -1;
        codecpar->extradata_size = static_cast<int>(info.extraData.size());
        codecpar->extradata = static_cast<uint8_t *>(
            av_mallocz(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codecpar->extradata) {
            codecpar->extradata_size = 0;
            return -1;
        }
        memcpy(codecpar->extradata, info.extraData.data(), info.extraData.size());
    }

    m_streams.push_back(stream);
    m_lastDts.push_back(AV_NOPTS_VALUE);
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
    if (!m_formatCtx || m_headerWritten || m_finalized || m_formatCtx->pb) {
        m_lastError = QStringLiteral("The recording header cannot be written now "
                                     "(uninitialized, already written or finalized)");
        return false;
    }
    m_lastError.clear();

    if (!(m_formatCtx->oformat->flags & AVFMT_NOFILE)) {
        const int openRet = avio_open(&m_formatCtx->pb, m_outputPath.toUtf8().constData(), AVIO_FLAG_WRITE);
        if (openRet < 0) {
            m_lastError = QStringLiteral("The recording file could not be opened: %1")
                              .arg(ffmpegErrorText(openRet));
            return false;
        }
    }

    AVDictionary *opts = nullptr;
    if (QString(m_formatCtx->oformat->name) == "mp4") {
        // faststart moves the moov atom to the front for web playback.
        // hybrid_fragmented (FFmpeg >= 8.1) also writes the file in fragmented
        // mode, so a crashed/killed recording stays playable instead of
        // needing the trailer to be flushed on clean stop.
        QString movflags = QStringLiteral("faststart");
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 6, 101)
        movflags += QStringLiteral("+hybrid_fragmented");
#endif
        av_dict_set(&opts, "movflags", movflags.toUtf8().constData(), 0);
#if LIBAVFORMAT_VERSION_INT >= AV_VERSION_INT(62, 6, 101)
        // use_editlist keeps edit lists consistent with fragmented output.
        av_dict_set(&opts, "use_editlist", "1", 0);
#endif
    } else if (QString(m_formatCtx->oformat->name).contains("matroska")) {
        // Flush clusters every second instead of the 5s default: bounds
        // how much encoded data sits in muxer RAM (invisible to live
        // size readouts and lost on crash) to ~1s of content.
        av_dict_set(&opts, "cluster_time_limit", "1000", 0);
    }

    int ret = avformat_write_header(m_formatCtx, &opts);
    av_dict_free(&opts);
    if (m_formatCtx->pb)
        m_bytesWritten.store(std::max<int64_t>(0, avio_tell(m_formatCtx->pb)),
                             std::memory_order_relaxed);

    if (ret < 0 || (m_formatCtx->pb && m_formatCtx->pb->error < 0)) {
        const int code = ret < 0 ? ret : m_formatCtx->pb->error;
        m_lastError = QStringLiteral("The recording header could not be written: %1")
                          .arg(ffmpegErrorText(code));
        return false;
    }

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
        if (data.size() > static_cast<size_t>(std::numeric_limits<int>::max() -
                                              AV_INPUT_BUFFER_PADDING_SIZE))
            return false;
        codecpar->extradata_size = static_cast<int>(data.size());
        codecpar->extradata = static_cast<uint8_t *>(av_mallocz(codecpar->extradata_size + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codecpar->extradata) {
            codecpar->extradata_size = 0;
            return false;
        }
        memcpy(codecpar->extradata, data.data(), data.size());
    }

    return true;
}

bool FFmpegMuxer::writePacket(const EncodedPacket &packet) {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (!m_formatCtx || !m_headerWritten || m_finalized) {
        m_lastError = QStringLiteral("The recording file is not ready for packets");
        return false;
    }
    if (packet.streamIndex < 0 ||
        packet.streamIndex >= static_cast<int>(m_streams.size()) ||
        packet.data.size() > static_cast<size_t>(std::numeric_limits<int>::max())) {
        m_lastError = QStringLiteral("Invalid packet for stream %1")
                          .arg(packet.streamIndex);
        return false;
    }
    m_lastError.clear();

    AVPacket *avPacket = av_packet_alloc();
    if (!avPacket) {
        m_lastError = QStringLiteral("Out of memory allocating a packet");
        return false;
    }

    if (av_new_packet(avPacket, packet.data.size()) < 0) {
        av_packet_free(&avPacket);
        m_lastError = QStringLiteral("Out of memory allocating packet data");
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

    // FFmpeg refuses any packet whose dts does not advance and aborts the
    // whole mux ("non monotonically increasing dts"). One jittery capture
    // timestamp must not end a long recording, so clamp a backwards step to
    // the next representable tick and keep pts consistent with it.
    if (avPacket->dts == AV_NOPTS_VALUE)
        avPacket->dts = avPacket->pts;
    int64_t &lastDts = m_lastDts[packet.streamIndex];
    if (avPacket->dts != AV_NOPTS_VALUE && lastDts != AV_NOPTS_VALUE &&
        avPacket->dts <= lastDts) {
        const int64_t step =
            std::max<int64_t>(1, av_rescale_q(1, {1, 1000}, stream->time_base));
        const int64_t clamped = lastDts + step;
        if (m_clampedTimestamps++ % 100 == 0) {
            qWarning() << "FFmpegMuxer: stream" << packet.streamIndex
                       << "timestamp went backwards (" << avPacket->dts << "after"
                       << lastDts << ") - clamping to" << clamped
                       << "(total clamped:" << m_clampedTimestamps << ")";
        }
        avPacket->dts = clamped;
        if (avPacket->pts != AV_NOPTS_VALUE && avPacket->pts < avPacket->dts)
            avPacket->pts = avPacket->dts;
    }

    const int64_t acceptedDts = avPacket->dts;
    // av_interleaved_write_frame takes ownership and blanks the packet, so
    // the dts must be captured before the call (reading it afterwards yields
    // AV_NOPTS_VALUE and disables the monotonicity guard).
    int ret = av_interleaved_write_frame(m_formatCtx, avPacket);
    if (m_formatCtx->pb)
        m_bytesWritten.store(std::max<int64_t>(0, avio_tell(m_formatCtx->pb)),
                             std::memory_order_relaxed);
    av_packet_free(&avPacket);

    if (ret < 0 || (m_formatCtx->pb && m_formatCtx->pb->error < 0)) {
        const int code = ret < 0 ? ret : m_formatCtx->pb->error;
        m_lastError = QStringLiteral("The recording file could not be written: %1")
                          .arg(ffmpegErrorText(code));
        return false;
    }
    lastDts = acceptedDts;
    return true;
}

bool FFmpegMuxer::finalize() {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (m_finalized) return m_finalizeSuccess;
    if (!m_formatCtx) {
        m_lastError = QStringLiteral("The recording output was never initialized");
        return false;
    }

    m_lastError.clear();
    bool success = true;
    QString failure;
    if (m_headerWritten) {
        // Write the trailer (this updates the file size on disk)
        const int trailer = av_write_trailer(m_formatCtx);
        if (trailer < 0) {
            success = false;
            failure = ffmpegErrorText(trailer);
        }

        // Force a flush to the IO context so avio_tell is accurate
        if (m_formatCtx->pb) {
            avio_flush(m_formatCtx->pb);
            if (m_formatCtx->pb->error < 0) {
                success = false;
                if (failure.isEmpty())
                    failure = ffmpegErrorText(m_formatCtx->pb->error);
            }
        }
    } else {
        success = false;
        failure = QStringLiteral("the header was never written");
    }

    if (m_formatCtx->pb) {
        m_bytesWritten.store(std::max<int64_t>(0, avio_tell(m_formatCtx->pb)),
                             std::memory_order_relaxed);
        if (!(m_formatCtx->oformat->flags & AVFMT_NOFILE)) {
            // A close-only error must be reported before callers announce
            // success, not lost later in the destructor.
            const int closed = m_closeOutput(&m_formatCtx->pb);
            if (closed < 0) {
                success = false;
                if (failure.isEmpty())
                    failure = ffmpegErrorText(closed);
            }
        }
    }

    if (!success) {
        m_lastError = QStringLiteral("The recording file could not be finalized%1")
                          .arg(failure.isEmpty()
                                   ? QString()
                                   : QStringLiteral(": ") + failure);
    }

    m_finalized = true;
    m_finalizeSuccess = success;
    return success;
}

// Ensure the destructor still cleans up
FFmpegMuxer::~FFmpegMuxer() { 
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    cleanup_internal(); 
}

int64_t FFmpegMuxer::bytesWritten() const {
    return m_bytesWritten.load(std::memory_order_relaxed);
}

QString FFmpegMuxer::formatName() const {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    if (m_formatCtx && m_formatCtx->oformat) {
        return QString::fromUtf8(m_formatCtx->oformat->name);
    }
    return QStringLiteral("unknown");
}

QString FFmpegMuxer::lastError() const {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    return m_lastError;
}

void FFmpegMuxer::cleanup_internal() {
    if (m_formatCtx) {
        if (!(m_formatCtx->oformat->flags & AVFMT_NOFILE) && m_formatCtx->pb) {
            m_closeOutput(&m_formatCtx->pb);
        }
        avformat_free_context(m_formatCtx);
        m_formatCtx = nullptr;
    }
    m_streams.clear();
    m_lastDts.clear();
    m_clampedTimestamps = 0;
    m_headerWritten = false;
    m_finalized = false;
    m_finalizeSuccess = false;
    m_globalOffset = -1;
    m_bytesWritten.store(0, std::memory_order_relaxed);
}

void FFmpegMuxer::cleanup() {
    std::lock_guard<std::mutex> lock(m_muxerMutex);
    cleanup_internal();
}

} // namespace TrophyBackend
