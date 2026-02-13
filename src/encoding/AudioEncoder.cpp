/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AudioEncoder.h"
#include "IEncoder.h"

#include <QDebug>
#include <cstring>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/opt.h>
#include <libavutil/channel_layout.h>
}

namespace GameRecorder {

AudioEncoder::AudioEncoder(QObject *parent)
    : QObject(parent)
{
}

AudioEncoder::~AudioEncoder()
{
    cleanup();
}

bool AudioEncoder::initialize(const AudioEncoderConfig &config)
{
    if (m_initialized) {
        cleanup();
    }

    // Find encoder
    const AVCodec *codec = nullptr;
    if (config.codec == "opus") {
        codec = avcodec_find_encoder(AV_CODEC_ID_OPUS);
    } else {
        codec = avcodec_find_encoder(AV_CODEC_ID_AAC);
    }

    if (!codec) {
        qCritical() << "Audio codec not found:" << config.codec;
        return false;
    }

    m_codecCtx = avcodec_alloc_context3(codec);
    if (!m_codecCtx) {
        qCritical() << "Failed to allocate audio codec context";
        return false;
    }

    // Configure encoder
    m_codecCtx->sample_rate = config.sampleRate;
    m_codecCtx->bit_rate = config.bitrate * 1000;
    m_codecCtx->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m_codecCtx->time_base = {1, config.sampleRate};

    // Set channel layout
    av_channel_layout_default(&m_codecCtx->ch_layout, config.channels);

    // Open encoder
    int ret = avcodec_open2(m_codecCtx, codec, nullptr);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        qCritical() << "Failed to open audio codec:" << err;
        cleanup();
        return false;
    }

    // Allocate frame
    m_frame = av_frame_alloc();
    if (!m_frame) {
        cleanup();
        return false;
    }

    m_frame->format = m_codecCtx->sample_fmt;
    m_frame->nb_samples = m_codecCtx->frame_size;
    av_channel_layout_copy(&m_frame->ch_layout, &m_codecCtx->ch_layout);

    if (av_frame_get_buffer(m_frame, 0) < 0) {
        qCritical() << "Failed to allocate audio frame buffer";
        cleanup();
        return false;
    }

    // Allocate packet
    m_packet = av_packet_alloc();
    if (!m_packet) {
        cleanup();
        return false;
    }

    m_sampleCount = 0;
    m_initialized = true;

    qInfo() << "Audio encoder initialized:" << config.codec
            << config.sampleRate << "Hz," << config.channels << "ch,"
            << config.bitrate << "kbps";

    return true;
}

bool AudioEncoder::encodeAudio(const float *samples, int frameCount, EncodedPacket &output)
{
    if (!m_initialized) {
        return false;
    }

    if (av_frame_make_writable(m_frame) < 0) {
        emit encodingError("Failed to make audio frame writable");
        return false;
    }

    // Copy samples to frame (convert interleaved to planar if needed)
    int channels = m_frame->ch_layout.nb_channels;
    int frameSamples = std::min(frameCount, m_frame->nb_samples);

    // FFmpeg expects planar float for AAC
    for (int ch = 0; ch < channels; ch++) {
        float *dst = reinterpret_cast<float*>(m_frame->data[ch]);
        for (int s = 0; s < frameSamples; s++) {
            dst[s] = samples[s * channels + ch];
        }
    }

    m_frame->pts = m_sampleCount;
    m_sampleCount += frameSamples;

    // Send frame
    int ret = avcodec_send_frame(m_codecCtx, m_frame);
    if (ret < 0) {
        char err[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(ret, err, sizeof(err));
        emit encodingError(QString("Error sending audio frame: %1").arg(err));
        return false;
    }

    // Receive packet
    ret = avcodec_receive_packet(m_codecCtx, m_packet);
    if (ret == AVERROR(EAGAIN)) {
        return false;  // Need more frames
    }
    if (ret < 0) {
        return false;
    }

    return processPacket(m_packet, output);
}

int AudioEncoder::flush(std::vector<EncodedPacket> &packets)
{
    if (!m_initialized) {
        return 0;
    }

    avcodec_send_frame(m_codecCtx, nullptr);

    int count = 0;
    while (true) {
        int ret = avcodec_receive_packet(m_codecCtx, m_packet);
        if (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) {
            break;
        }
        if (ret < 0) {
            break;
        }

        EncodedPacket output;
        if (processPacket(m_packet, output)) {
            packets.push_back(std::move(output));
            count++;
        }
    }

    return count;
}

bool AudioEncoder::processPacket(AVPacket *packet, EncodedPacket &output)
{
    output.data.resize(packet->size);
    memcpy(output.data.data(), packet->data, packet->size);
    output.pts = packet->pts;
    output.dts = packet->dts;
    output.isKeyframe = true;  // Audio frames are always keyframes
    output.streamIndex = 1;    // Audio stream

    av_packet_unref(packet);
    return true;
}

int AudioEncoder::codecId() const
{
    return m_codecCtx ? m_codecCtx->codec_id : 0;
}

std::vector<uint8_t> AudioEncoder::extraData() const
{
    if (!m_codecCtx || !m_codecCtx->extradata) {
        return {};
    }
    return std::vector<uint8_t>(m_codecCtx->extradata,
                                 m_codecCtx->extradata + m_codecCtx->extradata_size);
}

void AudioEncoder::cleanup()
{
    if (m_packet) {
        av_packet_free(&m_packet);
    }
    if (m_frame) {
        av_frame_free(&m_frame);
    }
    if (m_codecCtx) {
        avcodec_free_context(&m_codecCtx);
    }
    m_initialized = false;
}

} // namespace GameRecorder
