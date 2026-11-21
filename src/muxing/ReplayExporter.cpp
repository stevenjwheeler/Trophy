/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ReplayExporter.h"
#include <QSize>
#include <QTemporaryDir>
#include <algorithm>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace TrophyBackend {
namespace {
struct Input {
  AVFormatContext *context = nullptr;
  ~Input() { avformat_close_input(&context); }
  bool open(const QString &path) {
    return avformat_open_input(&context, path.toUtf8().constData(), nullptr, nullptr) >= 0 &&
           avformat_find_stream_info(context, nullptr) >= 0 && context->nb_streams &&
           context->streams[0]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO;
  }
};
struct Output {
  AVFormatContext *context = nullptr;
  ~Output() {
    if (context) {
      if (context->pb) avio_closep(&context->pb);
      avformat_free_context(context);
    }
  }
};
struct Codec {
  AVCodecContext *context = nullptr;
  ~Codec() { avcodec_free_context(&context); }
};
struct Frame {
  AVFrame *frame = av_frame_alloc();
  ~Frame() { av_frame_free(&frame); }
};
struct Packet {
  AVPacket *packet = av_packet_alloc();
  ~Packet() { av_packet_free(&packet); }
};
struct Scaler {
  SwsContext *context = nullptr;
  ~Scaler() { sws_freeContext(context); }
};

bool normalize(const QString &source, const QString &path, QSize canvas) {
  Input input;
  if (!input.open(source)) return false;
  auto *video = input.context->streams[0];
  const AVCodec *decoderCodec = avcodec_find_decoder(video->codecpar->codec_id);
  const AVCodec *encoderCodec = avcodec_find_encoder_by_name("libx264");
  if (!decoderCodec || !encoderCodec) return false;
  Codec decoder, encoder;
  decoder.context = avcodec_alloc_context3(decoderCodec);
  encoder.context = avcodec_alloc_context3(encoderCodec);
  if (!decoder.context || !encoder.context) return false;
  if (avcodec_parameters_to_context(decoder.context, video->codecpar) < 0) return false;
  decoder.context->pkt_timebase = video->time_base;
  decoder.context->thread_count = 1;
  decoder.context->err_recognition = AV_EF_EXPLODE;
  if (avcodec_open2(decoder.context, decoderCodec, nullptr) < 0) return false;
  auto *enc = encoder.context;
  enc->width = canvas.width() & ~1;
  enc->height = canvas.height() & ~1;
  enc->pix_fmt = AV_PIX_FMT_YUV420P;
  enc->time_base = {1, 90000};
  enc->framerate = video->avg_frame_rate.num > 0 ? video->avg_frame_rate : AVRational{30, 1};
  enc->sample_aspect_ratio = {1, 1};
  enc->gop_size = std::max(1, int(av_q2d(enc->framerate)));
  enc->max_b_frames = 0;
  enc->thread_count = 1;
  enc->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
  av_opt_set(enc->priv_data, "preset", "fast", 0);
  av_opt_set(enc->priv_data, "crf", "18", 0);
  if (avcodec_open2(enc, encoderCodec, nullptr) < 0) return false;
  Output output;
  if (avformat_alloc_output_context2(&output.context, nullptr, "matroska",
                                     path.toUtf8().constData()) < 0 || !output.context)
    return false;
  if (av_dict_copy(&output.context->metadata, input.context->metadata, 0) < 0) return false;
  if (av_dict_set(&output.context->metadata, "trophy-encoder", "x264", 0) < 0) return false;
  for (unsigned i = 0; i < input.context->nb_streams; ++i) {
    auto *stream = avformat_new_stream(output.context, nullptr);
    if (!stream) return false;
    stream->time_base = i == 0 ? enc->time_base : input.context->streams[i]->time_base;
    const int copied = i == 0 ? avcodec_parameters_from_context(stream->codecpar, enc)
        : avcodec_parameters_copy(stream->codecpar, input.context->streams[i]->codecpar);
    if (copied < 0 || av_dict_copy(&stream->metadata, input.context->streams[i]->metadata, 0) < 0)
      return false;
    stream->codecpar->codec_tag = 0;
  }
  if (avio_open(&output.context->pb, path.toUtf8().constData(), AVIO_FLAG_WRITE) < 0 ||
      avformat_write_header(output.context, nullptr) < 0) return false;
  Frame decoded, scaled;
  Packet packet, encoded;
  Scaler scaler;
  if (!decoded.frame || !scaled.frame || !packet.packet || !encoded.packet) return false;
  scaled.frame->format = enc->pix_fmt;
  scaled.frame->width = enc->width;
  scaled.frame->height = enc->height;
  if (av_frame_get_buffer(scaled.frame, 32) < 0) return false;
  const auto drainEncoder = [&] {
    int status;
    while ((status = avcodec_receive_packet(enc, encoded.packet)) >= 0) {
      encoded.packet->stream_index = 0;
      av_packet_rescale_ts(encoded.packet, enc->time_base, output.context->streams[0]->time_base);
      const int written = av_interleaved_write_frame(output.context, encoded.packet);
      av_packet_unref(encoded.packet);
      if (written < 0) return false;
    }
    return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
  };
  int64_t lastPts = AV_NOPTS_VALUE;
  const auto drainDecoder = [&] {
    int status;
    while ((status = avcodec_receive_frame(decoder.context, decoded.frame)) >= 0) {
      const auto *frame = decoded.frame;
      if (frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT)) return false;
      if (frame->best_effort_timestamp == AV_NOPTS_VALUE || frame->width <= 0 ||
          frame->height <= 0 || av_frame_make_writable(scaled.frame) < 0) return false;
      const double fit = std::min(double(enc->width) / frame->width,
                                  double(enc->height) / frame->height);
      const int width = std::max(2, int(frame->width * fit) & ~1);
      const int height = std::max(2, int(frame->height * fit) & ~1);
      scaler.context = sws_getCachedContext(scaler.context, frame->width, frame->height,
          AVPixelFormat(frame->format), width, height, enc->pix_fmt,
          SWS_BILINEAR, nullptr, nullptr, nullptr);
      if (!scaler.context) return false;
      ptrdiff_t lines[4];
      for (int i = 0; i < 4; ++i) lines[i] = scaled.frame->linesize[i];
      if (av_image_fill_black(scaled.frame->data, lines, enc->pix_fmt,
                              AVCOL_RANGE_MPEG, enc->width, enc->height) < 0) return false;
      const int x = ((enc->width - width) / 2) & ~1;
      const int y = ((enc->height - height) / 2) & ~1;
      uint8_t *dest[4] = {
          scaled.frame->data[0] + y * scaled.frame->linesize[0] + x,
          scaled.frame->data[1] + (y / 2) * scaled.frame->linesize[1] + x / 2,
          scaled.frame->data[2] + (y / 2) * scaled.frame->linesize[2] + x / 2, nullptr};
      if (sws_scale(scaler.context, frame->data, frame->linesize, 0, frame->height,
                    dest, scaled.frame->linesize) != height) return false;
      auto pts = av_rescale_q(frame->best_effort_timestamp, video->time_base, enc->time_base);
      if (lastPts != AV_NOPTS_VALUE) pts = std::max(pts, lastPts + 1);
      scaled.frame->pts = lastPts = pts;
      if (avcodec_send_frame(enc, scaled.frame) < 0 || !drainEncoder()) return false;
      av_frame_unref(decoded.frame);
    }
    return status == AVERROR(EAGAIN) || status == AVERROR_EOF;
  };
  int status;
  while ((status = av_read_frame(input.context, packet.packet)) >= 0) {
    bool ok;
    if (packet.packet->stream_index == 0) {
      ok = avcodec_send_packet(decoder.context, packet.packet) >= 0 && drainDecoder();
    } else {
      const int stream = packet.packet->stream_index;
      av_packet_rescale_ts(packet.packet, input.context->streams[stream]->time_base,
                           output.context->streams[stream]->time_base);
      ok = av_interleaved_write_frame(output.context, packet.packet) >= 0;
    }
    av_packet_unref(packet.packet);
    if (!ok) return false;
  }
  if (status != AVERROR_EOF || avcodec_send_packet(decoder.context, nullptr) < 0 ||
      !drainDecoder() || avcodec_send_frame(enc, nullptr) < 0 || !drainEncoder() ||
      av_write_trailer(output.context) < 0) return false;
  avio_flush(output.context->pb);
  if (output.context->pb->error < 0) return false;
  return avio_closep(&output.context->pb) >= 0;
}
}

bool ReplayExporter::saveTail(const QList<ReplaySegment> &segments, double seconds,
                              const QString &path, QString *error, double *keptSeconds,
                              double endSeconds) {
  if (segments.isEmpty())
    return ReplayClipper::saveTail(segments, seconds, path, error, keptSeconds, endSeconds);
  QSize canvas;
  bool mixed = false;
  AVCodecID codec = AV_CODEC_ID_NONE;
  for (const auto &segment : segments) {
    Input input;
    if (!input.open(segment.path)) {
      if (error) *error = QStringLiteral("Could not read a buffered replay segment.");
      return false;
    }
    const auto *parameters = input.context->streams[0]->codecpar;
    const QSize current(parameters->width, parameters->height);
    if (canvas.isValid() && (canvas != current || codec != parameters->codec_id)) mixed = true;
    canvas = current;
    codec = parameters->codec_id;
  }
  if (!mixed)
    return ReplayClipper::saveTail(segments, seconds, path, error, keptSeconds, endSeconds);
  QTemporaryDir temporary;
  if (!temporary.isValid() || !canvas.isValid()) {
    if (error) *error = QStringLiteral("Could not prepare mixed-resolution replay export.");
    return false;
  }
  QList<ReplaySegment> normalized;
  for (const auto &segment : segments) {
    const QString target = temporary.filePath(QString::number(normalized.size()) + ".mkv");
    if (!normalize(segment.path, target, canvas)) {
      if (error) *error = QStringLiteral("Could not normalize replay video; mixed-resolution export requires libx264.");
      return false;
    }
    normalized.append({target, segment.startOffsetSec});
  }
  return ReplayClipper::saveTail(normalized, seconds, path, error, keptSeconds, endSeconds);
}
}
