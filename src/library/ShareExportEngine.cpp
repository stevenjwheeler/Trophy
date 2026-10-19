/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ShareExportEngine.h"
#include "RecordingAnnotations.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QSet>
#include <QDateTime>
#include <algorithm>
#include <cmath>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace TrophyBackend {
namespace {
constexpr int kRate = 48000;
constexpr int64_t kMaxUs = INT64_C(86400) * AV_TIME_BASE;
constexpr AVRational kUs{1, AV_TIME_BASE};
constexpr AVRational kSamples{1, kRate};
void check(bool ok, const char *message) { if (!ok) throw std::runtime_error(message); }
void ffcheck(int code, const char *message) {
  if (code >= 0) return;
  char error[AV_ERROR_MAX_STRING_SIZE];
  av_strerror(code, error, sizeof(error));
  throw std::runtime_error(std::string(message) + ": " + error);
}
void cancelled(std::atomic_bool &cancel) { check(!cancel.load(), "Export cancelled."); }
struct InputFree { void operator()(AVFormatContext *p) const { avformat_close_input(&p); } };
struct CodecFree { void operator()(AVCodecContext *p) const { avcodec_free_context(&p); } };
struct FrameFree { void operator()(AVFrame *p) const { av_frame_free(&p); } };
struct PacketFree { void operator()(AVPacket *p) const { av_packet_free(&p); } };
using Input = std::unique_ptr<AVFormatContext, InputFree>;
using Codec = std::unique_ptr<AVCodecContext, CodecFree>;
using Frame = std::unique_ptr<AVFrame, FrameFree>;
using Packet = std::unique_ptr<AVPacket, PacketFree>;
Frame frame() { Frame f(av_frame_alloc()); check(bool(f), "Cannot allocate export frame."); return f; }
Packet packet() { Packet p(av_packet_alloc()); check(bool(p), "Cannot allocate export packet."); return p; }
Input open(const QString &path, std::atomic_bool &cancel) {
  AVFormatContext *raw = avformat_alloc_context();
  check(raw, "Cannot allocate input.");
  raw->interrupt_callback = {[](void *p) { return static_cast<std::atomic_bool *>(p)->load() ? 1 : 0; }, &cancel};
  const int result = avformat_open_input(&raw, QFile::encodeName(path).constData(), nullptr, nullptr);
  Input input(raw);
  ffcheck(result, "Cannot open recording");
  ffcheck(avformat_find_stream_info(input.get(), nullptr), "Cannot read recording streams");
  cancelled(cancel);
  return input;
}
class Reader {
public:
  Input input;
  Codec codec;
  Frame decoded = frame();
  Packet encoded = packet();
  AVStream *stream;
  bool draining = false;
  std::atomic_bool &cancel;
  Reader(const QString &path, int index, std::atomic_bool &stop) : input(open(path, stop)), cancel(stop) {
    if (index < 0) index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(index >= 0 && unsigned(index) < input->nb_streams, "Selected stream is unavailable.");
    stream = input->streams[index];
    check(stream->time_base.num > 0 && stream->time_base.den > 0, "Invalid stream clock.");
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    check(decoder, "A decoder for this recording is unavailable.");
    codec.reset(avcodec_alloc_context3(decoder));
    check(bool(codec), "Cannot allocate decoder.");
    ffcheck(avcodec_parameters_to_context(codec.get(), stream->codecpar), "Cannot configure decoder");
    codec->thread_count = 2;
    codec->max_pixels = INT64_C(8192) * 4320;
    ffcheck(avcodec_open2(codec.get(), decoder, nullptr), "Cannot start decoder");
  }
  bool next() {
    av_frame_unref(decoded.get());
    while (true) {
      cancelled(cancel);
      const int result = avcodec_receive_frame(codec.get(), decoded.get());
      if (result == 0) return true;
      if (result == AVERROR_EOF) return false;
      ffcheck(result == AVERROR(EAGAIN) ? 0 : result, "Cannot decode recording");
      check(!draining, "Decoder stalled while draining.");
      int read;
      do {
        av_packet_unref(encoded.get());
        read = av_read_frame(input.get(), encoded.get());
        cancelled(cancel);
      } while (read >= 0 && encoded->stream_index != stream->index);
      if (read == AVERROR_EOF) {
        draining = true;
        ffcheck(avcodec_send_packet(codec.get(), nullptr), "Cannot drain decoder");
      } else {
        ffcheck(read, "Cannot read recording");
        ffcheck(avcodec_send_packet(codec.get(), encoded.get()), "Cannot submit recording packet");
      }
    }
  }
};
class AudioReader {
  Reader reader;
  SwrContext *swr = nullptr;
  std::vector<float> samples;
  int rate = 0, channels = 0, format = -1;
  AVChannelLayout layout{};
  int64_t start = 0, nextSample = 0, origin;
  bool eof = false, done = false;
  bool haveOutput = false;
  bool chunk() {
    while (!done) {
      const bool have = !eof && reader.next();
      eof |= !have;
      if (!have && !swr) { done = true; return false; }
      if (have) {
        auto *f = reader.decoded.get();
        check(f->nb_samples > 0 && f->nb_samples <= (1 << 20) &&
            f->sample_rate >= 8000 && f->sample_rate <= 384000 &&
            f->ch_layout.nb_channels > 0 && f->ch_layout.nb_channels <= 32,
            "Invalid decoded audio layout.");
        if (!swr) {
          rate = f->sample_rate; channels = f->ch_layout.nb_channels; format = f->format;
          ffcheck(av_channel_layout_copy(&layout, &f->ch_layout), "Cannot retain source audio layout");
          AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
          ffcheck(swr_alloc_set_opts2(&swr, &stereo, AV_SAMPLE_FMT_FLT, kRate,
              &f->ch_layout, AVSampleFormat(format), rate, 0, nullptr), "Cannot configure audio conversion");
          ffcheck(swr_init(swr), "Cannot start audio conversion");
        }
        check(rate == f->sample_rate && channels == f->ch_layout.nb_channels && format == f->format &&
              av_channel_layout_compare(&layout, &f->ch_layout) == 0,
              "Audio layout changes within this recording are unsupported.");
        if (f->best_effort_timestamp == AV_NOPTS_VALUE) start = nextSample;
        else {
          const int64_t absolute = av_rescale_q(f->best_effort_timestamp, reader.stream->time_base, kSamples);
          check(absolute >= -int64_t(kRate) * 172800 && absolute <= int64_t(kRate) * 172800, "Invalid audio timestamps.");
          start = absolute - origin - av_rescale_rnd(swr_get_delay(swr, rate), kRate, rate, AV_ROUND_NEAR_INF);
          const auto tolerance = std::clamp<int64_t>(av_rescale_q(2, reader.stream->time_base, kSamples), 2, 96);
          // MKV's millisecond packet timestamps are coarser than a sample.
          // Keep continuous PCM sample-locked; preserve genuine larger gaps.
          if (haveOutput && std::abs(start - nextSample) <= tolerance) start = nextSample;
        }
      } else start = nextSample;
      check(start >= -int64_t(kRate) * 86400 && start <= int64_t(kRate) * 86400,
            "Audio timestamps exceed the supported 24-hour timeline.");
      const int count = swr_get_out_samples(swr, have ? reader.decoded->nb_samples : 0);
      check(count >= 0 && count <= (1 << 20), "Audio conversion exceeds its memory budget.");
      samples.resize(size_t(std::max(4096, count)) * 2);
      auto *out = reinterpret_cast<uint8_t *>(samples.data());
      const int produced = swr_convert(swr, &out, int(samples.size() / 2),
          have ? const_cast<const uint8_t **>(reader.decoded->extended_data) : nullptr,
          have ? reader.decoded->nb_samples : 0);
      ffcheck(produced, "Cannot convert audio");
      samples.resize(size_t(produced) * 2);
      nextSample = start + produced;
      if (produced > 0) { haveOutput = true; return true; }
      if (eof) done = true;
    }
    return false;
  }
public:
  AudioReader(const QString &path, int index, int64_t originUs, std::atomic_bool &stop)
      : reader(path, index, stop), origin(av_rescale_q(originUs, kUs, kSamples)) {
    check(reader.stream->codecpar->codec_type == AVMEDIA_TYPE_AUDIO, "Selected stream is not audio.");
  }
  ~AudioReader() { swr_free(&swr); av_channel_layout_uninit(&layout); }
  void add(float *mix, int64_t at, int count, float gain) {
    while (!done) {
      if (samples.empty() || nextSample <= at) { if (!chunk()) break; }
      if (start >= at + count) break; // Real gap/late audio: leave silence.
      const auto begin = std::max(start, at), end = std::min(nextSample, at + count);
      for (int64_t i = begin; i < end; ++i)
        for (int ch = 0; ch < 2; ++ch)
          mix[(i - at) * 2 + ch] += samples[size_t(i - start) * 2 + ch] * gain;
      if (nextSample >= at + count) break;
      samples.clear();
    }
  }
};
struct Output {
  AVFormatContext *ctx = nullptr;
  ~Output() { if (ctx) { if (ctx->pb) avio_closep(&ctx->pb); avformat_free_context(ctx); } }
  void write(AVCodecContext *codec, AVStream *stream, AVFrame *f) {
    ffcheck(avcodec_send_frame(codec, f), "Cannot encode export");
    auto p = packet();
    while (true) {
      const int result = avcodec_receive_packet(codec, p.get());
      if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) break;
      ffcheck(result, "Cannot receive encoded export packet");
      check(stream && stream->index >= 0 && unsigned(stream->index) < ctx->nb_streams &&
            p->size > 0 && p->data, "Encoder produced an invalid export packet.");
      av_packet_rescale_ts(p.get(), codec->time_base, stream->time_base);
      p->stream_index = stream->index;
      ffcheck(av_interleaved_write_frame(ctx, p.get()), "Cannot write export (check free disk space)");
    }
  }
};
Codec encoder(const char *name) {
  const AVCodec *codec = avcodec_find_encoder_by_name(name);
  check(codec, "Required H.264/AAC software encoder is unavailable.");
  Codec ctx(avcodec_alloc_context3(codec));
  check(bool(ctx), "Cannot allocate export encoder.");
  return ctx;
}
AVStream *addStream(Output &out, AVCodecContext *codec) {
  AVStream *stream = avformat_new_stream(out.ctx, nullptr);
  check(stream, "Cannot allocate export stream.");
  stream->time_base = codec->time_base;
  ffcheck(avcodec_parameters_from_context(stream->codecpar, codec), "Cannot configure export stream");
  return stream;
}
}

QVariantMap ShareExportEngine::inspect(const QString &source, std::atomic_bool &cancel) {
  try {
    auto input = open(source, cancel);
    const int index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(index >= 0, "No video stream found.");
    const auto routing = AudioRoutingInfo::fromJson(RecordingAnnotations::audioRouting(source));
    int audioCount = 0;
    for (unsigned i = 0; i < input->nb_streams; ++i)
      if (input->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO) ++audioCount;
    QVariantList tracks;
    for (unsigned i = 0; i < input->nb_streams; ++i) {
      if (input->streams[i]->codecpar->codec_type != AVMEDIA_TYPE_AUDIO) continue;
      check(tracks.size() < 32, "Too many audio streams.");
      const auto *title = av_dict_get(input->streams[i]->metadata, "title", nullptr, 0);
      const int ordinal = tracks.size();
      QString label = title ? QString::fromUtf8(title->value).trimmed() : QString{};
      bool inferred = false;
      if (label.isEmpty()) {
        // Older Trophy separate-track recordings used this order without
        // stream titles. Never use it to exclude a track or claim routing evidence.
        if (!routing.known && audioCount == 3) {
          label = QStringList{"Game / System", "Voice", "Microphone"}[ordinal];
          inferred = true;
        } else {
          label = QString("Track %1 — source unknown").arg(ordinal + 1);
        }
      }
      if (routing.known) {
        if (ordinal == routing.primary) label = routing.system ? (routing.game ? "Game / System" : "System") : "Game";
        else if (ordinal == routing.voiceTrack) label = "Voice";
        else if (ordinal == routing.micTrack) label = "Microphone";
      }
      tracks.append(QVariantMap{{"index", int(i)}, {"ordinal", ordinal}, {"label", label}, {"inferred", inferred}});
    }
    if (routing.known && routing.separate && routing.system && !routing.voice &&
        routing.primary >= 0 && routing.primary < tracks.size() && routing.voiceTrack >= 0 && routing.voiceTrack < tracks.size()) {
      auto row = tracks[routing.voiceTrack].toMap(); row["defaultExcluded"] = true; tracks[routing.voiceTrack] = row;
    }
    return {{"tracks", tracks}, {"durationSeconds", input->duration > 0 ? double(input->duration) / AV_TIME_BASE : 0.0}};
  } catch (const std::exception &e) { return {{"error", QString::fromUtf8(e.what())}}; }
}

ShareExportResult ShareExportEngine::run(const QString &source, const QString &directory,
    const ShareExportOptions &options, std::atomic_bool &cancel, std::atomic<double> &progress) {
  try {
    const QFileInfo fingerprint(source);
    check(fingerprint.isFile() && !fingerprint.isSymLink(), "Recording is unavailable.");
    struct stat original {};
    check(::lstat(QFile::encodeName(source).constData(), &original) == 0 && S_ISREG(original.st_mode), "Recording is unavailable.");
    check(options.maxHeight == 0 || options.maxHeight == 720 || options.maxHeight == 1080,
          "Unsupported export resolution.");
    check(options.targetBytes >= 0 && options.audioStreams.size() <= 8, "Invalid export options (up to eight audio tracks).");
    QSet<int> unique;
    for (int index : options.audioStreams) { check(index >= 0 && !unique.contains(index), "Invalid audio selection."); unique.insert(index); }
    check(QDir(directory).exists(), "Choose an existing export folder.");
    QTemporaryFile temporary(QDir(directory).filePath(".trophy-export-XXXXXX.part"));
    check(temporary.open(), "Cannot create export file.");
    const QString tmp = temporary.fileName();
    temporary.close();
    Reader video(source, -1, cancel);
    check(video.next(), "Recording has no decodable video frames.");
    auto *first = video.decoded.get();
    check(first->width >= 2 && first->height >= 2 && first->width <= 16384 && first->height <= 16384,
          "Unsupported video dimensions.");
    check(first->color_trc != AVCOL_TRC_SMPTE2084 && first->color_trc != AVCOL_TRC_ARIB_STD_B67,
          "HDR-to-SDR share export is not supported yet.");
    const int64_t origin = first->best_effort_timestamp == AV_NOPTS_VALUE ? 0 :
        av_rescale_q(first->best_effort_timestamp, video.stream->time_base, kUs);
    check(origin >= -kMaxUs && origin <= kMaxUs, "Invalid recording timestamp origin.");
    AVRational rate = av_guess_frame_rate(video.input.get(), video.stream, first);
    const bool knownRate = rate.num > 0 && rate.den > 0 && av_q2d(rate) >= 1;
    const int64_t inputFrameUs = knownRate ? std::max<int64_t>(1, av_rescale_q(1, av_inv_q(rate), kUs)) : 16667;
    const bool capRate = !knownRate || av_q2d(rate) > 60;
    if (capRate) rate = {60, 1};
    const int64_t frameUs = std::max<int64_t>(1, av_rescale_q(1, av_inv_q(rate), kUs));
    const int inputWidth = first->width, inputHeight = first->height;
    const int inputFormat = first->format;
    const double scale = options.maxHeight > 0 ? std::min(1.0, double(options.maxHeight) / inputHeight) : 1.0;
    Output out;
    ffcheck(avformat_alloc_output_context2(&out.ctx, nullptr, "mp4", QFile::encodeName(tmp).constData()), "Cannot create MP4");
    check(out.ctx, "Cannot allocate MP4 writer.");
    out.ctx->interrupt_callback = video.input->interrupt_callback;
    out.ctx->max_interleave_delta = AV_TIME_BASE;
    auto v = encoder("libx264");
    v->width = std::max(2, int(inputWidth * scale) & ~1);
    v->height = std::max(2, int(inputHeight * scale) & ~1);
    v->pix_fmt = AV_PIX_FMT_YUV420P; v->time_base = {1, 90000}; v->framerate = rate;
    v->gop_size = std::max(1, int(av_q2d(rate) * 2)); v->max_b_frames = 0; v->thread_count = 2;
    v->sample_aspect_ratio = first->sample_aspect_ratio.num > 0 ? first->sample_aspect_ratio : AVRational{1, 1};
    v->color_range = AVCOL_RANGE_MPEG; v->colorspace = AVCOL_SPC_BT709;
    v->color_primaries = AVCOL_PRI_BT709; v->color_trc = AVCOL_TRC_BT709;
    if (out.ctx->oformat->flags & AVFMT_GLOBALHEADER) v->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ffcheck(av_opt_set(v->priv_data, "preset", "veryfast", 0), "Cannot set export preset");
    if (options.targetBytes > 0) {
      check(video.input->duration > 0, "A size target requires a known recording duration.");
      const double seconds = double(video.input->duration) / AV_TIME_BASE;
      const double bitrate = double(options.targetBytes) * 8 * 0.94 / seconds - (unique.isEmpty() ? 0 : 192000);
      check(bitrate >= 100000, "Size target is too small for this recording. Choose a larger target.");
      v->bit_rate = int64_t(std::min(bitrate, 100000000.0));
    } else ffcheck(av_opt_set(v->priv_data, "crf", "20", 0), "Cannot set export quality");
    ffcheck(avcodec_open2(v.get(), v->codec, nullptr), "Cannot start H.264 export");
    AVStream *vs = addStream(out, v.get());
    if (const auto *title = av_dict_get(video.input->metadata, "title", nullptr, 0))
      ffcheck(av_dict_set(&out.ctx->metadata, "title", title->value, 0), "Cannot copy export title");
    if (const auto *kind = av_dict_get(video.input->metadata, "trophy-kind", nullptr, 0))
      ffcheck(av_dict_set(&out.ctx->metadata, "trophy-kind", kind->value, 0), "Cannot copy export kind");
    Codec a;
    AVStream *as = nullptr;
    Frame audio;
    std::vector<std::unique_ptr<AudioReader>> readers;
    if (!unique.isEmpty()) {
      a = encoder("aac"); a->sample_rate = kRate; a->sample_fmt = AV_SAMPLE_FMT_FLTP;
      av_channel_layout_default(&a->ch_layout, 2); a->time_base = kSamples; a->bit_rate = 192000;
      if (out.ctx->oformat->flags & AVFMT_GLOBALHEADER) a->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      ffcheck(avcodec_open2(a.get(), a->codec, nullptr), "Cannot start AAC export");
      as = addStream(out, a.get());
      av_dict_set(&as->metadata, "title", "Stereo mix", 0);
      audio = frame(); audio->format = a->sample_fmt; audio->sample_rate = kRate;
      ffcheck(av_channel_layout_copy(&audio->ch_layout, &a->ch_layout), "Cannot configure audio frame");
      audio->nb_samples = a->frame_size;
      check(audio->nb_samples > 0 && audio->nb_samples <= 8192, "Unsupported AAC frame size.");
      ffcheck(av_frame_get_buffer(audio.get(), 0), "Cannot allocate audio frame");
      for (int index : options.audioStreams) readers.push_back(std::make_unique<AudioReader>(source, index, origin, cancel));
    }
    ffcheck(avio_open2(&out.ctx->pb, QFile::encodeName(tmp).constData(), AVIO_FLAG_WRITE,
        &out.ctx->interrupt_callback, nullptr), "Cannot open export output");
    AVDictionary *muxOptions = nullptr;
    const int optionResult = av_dict_set(&muxOptions, "movflags", "+faststart+use_metadata_tags", 0);
    if (optionResult < 0) { av_dict_free(&muxOptions); ffcheck(optionResult, "Cannot configure MP4 fast start"); }
    const int header = avformat_write_header(out.ctx, &muxOptions);
    av_dict_free(&muxOptions);
    ffcheck(header, "Cannot write export header");
    auto converted = frame(); converted->format = v->pix_fmt; converted->width = v->width; converted->height = v->height;
    converted->color_range = v->color_range; converted->colorspace = v->colorspace;
    converted->color_primaries = v->color_primaries; converted->color_trc = v->color_trc;
    converted->sample_aspect_ratio = v->sample_aspect_ratio;
    ffcheck(av_frame_get_buffer(converted.get(), 32), "Cannot allocate video conversion frame");
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> sws(nullptr, sws_freeContext);
    int64_t audioAt = 0, lastVideo = -1, lastSlot = -1, endUs = 0, seenFrames = 0;
    auto writeAudioUntil = [&](int64_t untilUs) {
      if (!a) return;
      const int64_t target = av_rescale_q(untilUs, kUs, kSamples);
      while (audioAt < target) {
        cancelled(cancel);
        const int n = audio->nb_samples;
        std::vector<float> mix(size_t(n) * 2, 0.0f);
        for (auto &reader : readers) reader->add(mix.data(), audioAt, n, 1.0f / readers.size());
        ffcheck(av_frame_make_writable(audio.get()), "Cannot write audio frame");
        for (int ch = 0; ch < 2; ++ch) {
          auto *plane = reinterpret_cast<float *>(audio->data[ch]);
          for (int i = 0; i < n; ++i) plane[i] = std::isfinite(mix[size_t(i) * 2 + ch]) ?
              std::clamp(mix[size_t(i) * 2 + ch], -1.0f, 1.0f) : 0.0f;
        }
        audio->pts = audioAt;
        out.write(a.get(), as, audio.get());
        audioAt += n;
      }
    };
    do {
      cancelled(cancel);
      auto *decoded = video.decoded.get();
      check(decoded->width == inputWidth && decoded->height == inputHeight && decoded->data[0],
            "Video layout changed or frame data is invalid.");
      check(decoded->color_trc != AVCOL_TRC_SMPTE2084 && decoded->color_trc != AVCOL_TRC_ARIB_STD_B67,
            "HDR-to-SDR share export is not supported yet.");
      int64_t at = seenFrames * inputFrameUs;
      if (decoded->best_effort_timestamp != AV_NOPTS_VALUE) {
        const auto absolute = av_rescale_q(decoded->best_effort_timestamp, video.stream->time_base, kUs);
        check(absolute >= -kMaxUs * 2 && absolute <= kMaxUs * 2, "Invalid video timestamps.");
        at = absolute - origin;
      }
      ++seenFrames;
      check(at >= 0 && at <= kMaxUs, "Video timestamps exceed the supported 24-hour timeline.");
      endUs = std::max(endUs, at + inputFrameUs);
      if (lastVideo >= 0 && at <= lastVideo) continue;
      // Do not drop ordinary 30/60fps MKV frames just because their 1ms
      // timestamps alternate 33/34 or 16/17ms. Only cap genuinely high rates.
      if (capRate) {
        const int64_t slot = av_rescale_q(at, kUs, av_inv_q(rate));
        if (slot <= lastSlot) continue;
        lastSlot = slot;
      }
      writeAudioUntil(at);
      ffcheck(av_frame_make_writable(converted.get()), "Cannot write video frame");
      const auto *descriptor = av_pix_fmt_desc_get(AVPixelFormat(decoded->format));
      check(descriptor && !(descriptor->flags & (AV_PIX_FMT_FLAG_HWACCEL | AV_PIX_FMT_FLAG_PAL)), "Unsupported decoded pixel format.");
      int minimumStride[4]{};
      ffcheck(av_image_fill_linesizes(minimumStride, AVPixelFormat(decoded->format), inputWidth), "Invalid decoded strides");
      const int planes = av_pix_fmt_count_planes(AVPixelFormat(decoded->format));
      check(planes > 0 && planes <= 4, "Invalid decoded video planes.");
      for (int plane = 0; plane < planes; ++plane)
        check(decoded->data[plane] && std::abs(int64_t(decoded->linesize[plane])) >= minimumStride[plane], "Invalid decoded video plane.");
      if (!sws) {
        sws.reset(sws_getContext(inputWidth, inputHeight, AVPixelFormat(decoded->format),
            v->width, v->height, v->pix_fmt, SWS_BILINEAR, nullptr, nullptr, nullptr));
        check(bool(sws), "Cannot convert video for export.");
        const int sourceSpace = decoded->colorspace == AVCOL_SPC_BT470BG || decoded->colorspace == AVCOL_SPC_SMPTE170M ? SWS_CS_ITU601 : SWS_CS_ITU709;
        ffcheck(sws_setColorspaceDetails(sws.get(), sws_getCoefficients(sourceSpace), decoded->color_range == AVCOL_RANGE_JPEG,
            sws_getCoefficients(SWS_CS_ITU709), 0, 0, 1 << 16, 1 << 16), "Cannot configure export colors");
      }
      check(decoded->format == inputFormat && !(decoded->flags & AV_FRAME_FLAG_CORRUPT), "Video pixel format changed or a frame is corrupt.");
      check(sws_scale(sws.get(), decoded->data, decoded->linesize, 0, inputHeight,
          converted->data, converted->linesize) == v->height, "Video conversion was incomplete.");
      converted->pts = av_rescale_q(at, kUs, v->time_base);
      out.write(v.get(), vs, converted.get());
      lastVideo = at;
      if (video.input->duration > 0) progress.store(std::clamp(double(at) / video.input->duration, 0.0, 0.99));
    } while (video.next());
    writeAudioUntil(endUs);
    out.write(v.get(), vs, nullptr);
    if (a) out.write(a.get(), as, nullptr);
    ffcheck(av_write_trailer(out.ctx), "Cannot finalize export");
    avio_flush(out.ctx->pb);
    ffcheck(out.ctx->pb->error, "Cannot flush export");
    ffcheck(avio_closep(&out.ctx->pb), "Cannot close export output");
    cancelled(cancel);
    const QFileInfo current(source);
    struct stat latest {};
    check(current.isFile() && ::lstat(QFile::encodeName(source).constData(), &latest) == 0 &&
          S_ISREG(latest.st_mode) && latest.st_dev == original.st_dev && latest.st_ino == original.st_ino &&
          latest.st_size == original.st_size && latest.st_mtim.tv_sec == original.st_mtim.tv_sec &&
          latest.st_mtim.tv_nsec == original.st_mtim.tv_nsec,
          "Source recording changed during export; output was discarded.");
    const QString stem = fingerprint.completeBaseName() + "_Share";
    for (int n = 0; n < 10000; ++n) {
      const QString destination = QDir(directory).filePath(stem + (n ? "_" + QString::number(n) : QString()) + ".mp4");
      if (QFile::rename(tmp, destination)) { progress.store(1); return {destination, {}}; }
      check(QFile::exists(destination), "Cannot publish export file.");
    }
    throw std::runtime_error("Too many export filename collisions.");
  } catch (const std::exception &e) { return {{}, QString::fromUtf8(e.what())}; }
}
}
