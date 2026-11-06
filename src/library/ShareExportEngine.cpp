/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ShareExportEngine.h"
#include "RecordingAnnotations.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QDebug>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>
#include <sys/stat.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libavutil/pixdesc.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

namespace TrophyBackend {
namespace {
constexpr int kRate = 48000;
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
QList<double> normalizedGains(const ShareExportOptions &options) {
  QList<double> gains = options.audioGains;
  if (gains.isEmpty()) {
    for (int i = 0; i < options.audioStreams.size(); ++i) gains.append(1.0);
  }
  check(gains.size() == options.audioStreams.size(), "Invalid audio volume selection.");
  for (double gain : gains)
    check(std::isfinite(gain) && gain >= 0.0 && gain <= 1.0, "Invalid audio volume.");
  return gains;
}
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

// Each selected track owns a forward-only demux cursor. Decode on demand,
// retaining just one resampled frame of lookahead, rather than accumulating
// audio until the video cursor catches up. Separate cursors also handle files
// whose tracks are stored far apart without queuing other tracks' packets.
class AudioTrack {
public:
  AudioTrack(const QString &path, int index, int64_t originSamples,
             size_t &bufferedBytes, std::atomic_bool &cancel)
      : m_input(open(path, cancel)), m_cancel(cancel), m_index(index),
        m_origin(originSamples), m_bufferedBytes(bufferedBytes) {
    check(index >= 0 && unsigned(index) < m_input->nb_streams &&
              m_input->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO,
          "Audio stream is no longer available.");
    m_stream = m_input->streams[index];
    for (unsigned i = 0; i < m_input->nb_streams; ++i)
      if (int(i) != index) m_input->streams[i]->discard = AVDISCARD_ALL;
    const AVCodec *decoder = avcodec_find_decoder(m_stream->codecpar->codec_id);
    check(decoder, "A decoder for this recording is unavailable.");
    m_codec.reset(avcodec_alloc_context3(decoder));
    check(bool(m_codec), "Cannot allocate decoder.");
    ffcheck(avcodec_parameters_to_context(m_codec.get(), m_stream->codecpar), "Cannot configure decoder");
    m_codec->max_pixels = INT64_C(8192) * 4320;
    m_codec->thread_count = 0; // auto; the context default is a single thread
    ffcheck(avcodec_open2(m_codec.get(), decoder, nullptr), "Cannot start decoder");
  }
  ~AudioTrack() {
    clearSamples();
    if (m_swr) swr_free(&m_swr);
    av_channel_layout_uninit(&m_layout);
  }
  // Used after video EOF to discover the audio tail a block at a time.
  int64_t nextEnd(int64_t at) { return prepare(at) ? m_end : 0; }
  void add(float *mix, int64_t at, int count, float gain) {
    int64_t cursor = at;
    while (prepare(cursor)) {
      if (m_start >= at + count) break; // A genuine gap: retain future PCM.
      const int64_t begin = std::max(m_start, cursor);
      const int64_t end = std::min<int64_t>(m_end, at + count);
      for (int64_t i = begin; i < end; ++i)
        for (int ch = 0; ch < 2; ++ch)
          mix[(i - at) * 2 + ch] += m_samples[size_t((i - m_start) * 2 + ch)] * gain;
      if (m_end >= at + count) break;
      cursor = m_end;
    }
  }
private:
  // Defensive aggregate allocation bound for abnormal decoder frames, not a
  // time/interleaving limit: ordinary tracks retain only one frame apiece.
  void checkBudget(size_t samples) const {
    constexpr size_t limit = 32 * 1024 * 1024;
    check(samples <= limit / sizeof(float) &&
              m_bufferedBytes <= limit - samples * sizeof(float),
          "Decoded audio frames exceed the export memory budget.");
  }
  void append(int64_t start, std::vector<float> samples) {
    m_end = start + int64_t(samples.size() / 2);
    if (samples.empty()) return;
    m_haveOutput = true;
    m_start = start;
    m_samples = std::move(samples);
    m_bufferedBytes += m_samples.capacity() * sizeof(float);
  }
  void clearSamples() {
    m_bufferedBytes -= m_samples.capacity() * sizeof(float);
    std::vector<float>().swap(m_samples);
  }
  bool prepare(int64_t at) {
    while (!m_finished) {
      cancelled(m_cancel);
      if (!m_samples.empty() && m_end > at) return true;
      clearSamples();
      readNext();
    }
    return false;
  }
  void readNext() {
    while (!m_decoderEof) {
      cancelled(m_cancel);
      const int result = avcodec_receive_frame(m_codec.get(), m_frame.get());
      if (result == AVERROR_EOF) {
        m_decoderEof = true;
        break;
      }
      if (result == 0) {
        convert(m_frame.get());
        av_frame_unref(m_frame.get());
        if (!m_samples.empty()) return;
        continue;
      }
      if (result != AVERROR(EAGAIN)) ffcheck(result, "Cannot decode recording");
      check(!m_eofSent, "Audio decoder did not finish after EOF.");
      // receive was fully drained to EAGAIN, so sending the next packet must
      // succeed. No other track's packets are retained on this cursor.
      for (;;) {
        cancelled(m_cancel);
        const int read = av_read_frame(m_input.get(), m_packet.get());
        if (read == AVERROR_EOF) {
          ffcheck(avcodec_send_packet(m_codec.get(), nullptr), "Cannot drain decoder");
          m_eofSent = true;
          break;
        }
        ffcheck(read, "Cannot read recording audio");
        if (m_packet->stream_index == m_index) {
          const int sent = avcodec_send_packet(m_codec.get(), m_packet.get());
          av_packet_unref(m_packet.get());
          ffcheck(sent, "Cannot submit recording packet");
          break;
        }
        av_packet_unref(m_packet.get());
      }
    }
    // Decoder EOF does not drain libswresample. Return one tail block at a
    // time, including a short source that produced no PCM before EOF.
    if (m_swr) {
      const int count = swr_get_out_samples(m_swr, 0);
      check(count >= 0 && count <= (1 << 20), "Audio conversion exceeds its memory budget.");
      checkBudget(size_t(std::max(1, count)) * 2);
      std::vector<float> samples(size_t(std::max(1, count)) * 2);
      auto *out = reinterpret_cast<uint8_t *>(samples.data());
      const int produced = swr_convert(m_swr, &out, int(samples.size() / 2), nullptr, 0);
      ffcheck(produced, "Cannot drain audio conversion");
      if (produced) {
        samples.resize(size_t(produced) * 2);
        append(m_end, std::move(samples));
        return;
      }
    }
    m_finished = true;
  }
  void convert(AVFrame *f) {
    check(f->nb_samples > 0 && f->nb_samples <= (1 << 20) &&
              f->sample_rate >= 8000 && f->sample_rate <= 384000 &&
              f->ch_layout.nb_channels > 0 && f->ch_layout.nb_channels <= 32,
          "Invalid decoded audio layout.");
    if (!m_swr) {
      m_rate = f->sample_rate;
      m_channels = f->ch_layout.nb_channels;
      m_format = f->format;
      ffcheck(av_channel_layout_copy(&m_layout, &f->ch_layout), "Cannot retain source audio layout");
      AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
      ffcheck(swr_alloc_set_opts2(&m_swr, &stereo, AV_SAMPLE_FMT_FLT, kRate,
                  &f->ch_layout, AVSampleFormat(m_format), m_rate, 0, nullptr),
              "Cannot configure audio conversion");
      ffcheck(swr_init(m_swr), "Cannot start audio conversion");
    }
    check(m_rate == f->sample_rate && m_channels == f->ch_layout.nb_channels &&
              m_format == f->format && av_channel_layout_compare(&m_layout, &f->ch_layout) == 0,
          "Audio layout changes within this recording are unsupported.");
    int64_t startSample;
    if (f->best_effort_timestamp == AV_NOPTS_VALUE) startSample = m_end;
    else {
      const int64_t absolute = av_rescale_q(f->best_effort_timestamp, m_stream->time_base, kSamples);
      check(absolute >= -int64_t(kRate) * 172800 && absolute <= int64_t(kRate) * 172800,
            "Invalid audio timestamps.");
      startSample = absolute - m_origin -
          av_rescale_rnd(swr_get_delay(m_swr, m_rate), kRate, m_rate, AV_ROUND_NEAR_INF);
      const auto tolerance = std::clamp<int64_t>(av_rescale_q(2, m_stream->time_base, kSamples), 2, 96);
      // MKV's millisecond packet timestamps are coarser than a sample. Keep
      // continuous PCM sample-locked; preserve genuine larger gaps.
      if (m_haveOutput && std::abs(startSample - m_end) <= tolerance) startSample = m_end;
    }
    check(startSample >= -int64_t(kRate) * 86400 && startSample <= int64_t(kRate) * 86400,
          "Audio timestamps exceed the supported 24-hour timeline.");
    const int count = swr_get_out_samples(m_swr, f->nb_samples);
    check(count >= 0 && count <= (1 << 20), "Audio conversion exceeds its memory budget.");
    checkBudget(size_t(std::max(1, count)) * 2);
    std::vector<float> samples(size_t(std::max(1, count)) * 2);
    auto *out = reinterpret_cast<uint8_t *>(samples.data());
    const int produced = swr_convert(m_swr, &out, int(samples.size() / 2),
        const_cast<const uint8_t **>(f->extended_data), f->nb_samples);
    ffcheck(produced, "Cannot convert audio");
    samples.resize(size_t(produced) * 2);
    append(startSample, std::move(samples));
  }
  Input m_input;
  std::atomic_bool &m_cancel;
  int m_index;
  AVStream *m_stream = nullptr;
  Codec m_codec;
  Frame m_frame = frame();
  Packet m_packet = packet();
  SwrContext *m_swr = nullptr;
  int m_rate = 0, m_channels = 0, m_format = -1;
  AVChannelLayout m_layout{};
  int64_t m_origin;
  int64_t m_end = 0;
  bool m_haveOutput = false;
  int64_t m_start = 0;
  std::vector<float> m_samples;
  bool m_eofSent = false, m_decoderEof = false, m_finished = false;
  size_t &m_bufferedBytes;
};

struct Output {
  AVFormatContext *ctx = nullptr;
  ~Output() { if (ctx) { if (ctx->pb) avio_closep(&ctx->pb); avformat_free_context(ctx); } }
  // Only the mixed audio track is encoded; video packets are copied separately.
  void writeAudio(AVCodecContext *codec, AVStream *stream, AVFrame *f) {
    ffcheck(avcodec_send_frame(codec, f), "Cannot encode export audio");
    auto p = packet();
    while (true) {
      const int result = avcodec_receive_packet(codec, p.get());
      if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) break;
      ffcheck(result, "Cannot receive encoded export audio");
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
  check(codec, "The required audio encoder is unavailable.");
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

QString shareVideoCodecLabel(AVCodecID id) {
  switch (id) {
  case AV_CODEC_ID_H264: return QStringLiteral("H.264");
  case AV_CODEC_ID_HEVC: return QStringLiteral("HEVC (H.265)");
  case AV_CODEC_ID_AV1: return QStringLiteral("AV1");
  case AV_CODEC_ID_MPEG4: return QStringLiteral("MPEG-4 Part 2");
  case AV_CODEC_ID_VP9: return QStringLiteral("VP9");
  default: return QString::fromUtf8(avcodec_get_name(id));
  }
}

bool copyableVideo(AVCodecID id) {
  // Containers we write are MP4; these are the video codecs MP4 can carry.
  return id == AV_CODEC_ID_H264 || id == AV_CODEC_ID_HEVC || id == AV_CODEC_ID_AV1 ||
         id == AV_CODEC_ID_MPEG4;
}

struct SourceGuard {
  QFileInfo info;
  struct stat original{};
  explicit SourceGuard(const QString &source) : info(source) {
    check(info.isFile() && !info.isSymLink(), "Recording is unavailable.");
    check(::lstat(QFile::encodeName(source).constData(), &original) == 0 && S_ISREG(original.st_mode),
          "Recording is unavailable.");
  }
  void verifyUnchanged(const QString &source) const {
    const QFileInfo current(source);
    struct stat latest{};
    check(current.isFile() && ::lstat(QFile::encodeName(source).constData(), &latest) == 0 &&
          S_ISREG(latest.st_mode) && latest.st_dev == original.st_dev && latest.st_ino == original.st_ino &&
          latest.st_size == original.st_size && latest.st_mtim.tv_sec == original.st_mtim.tv_sec &&
          latest.st_mtim.tv_nsec == original.st_mtim.tv_nsec,
          "Source recording changed during export; output was discarded.");
  }
};

// Wall-clock length of the video stream, which is what an export's cost scales
// with. Falls back to the container duration.
double sourceSeconds(const AVStream *video) {
  if (video && video->duration > 0 && video->time_base.num > 0)
    return double(video->duration) * av_q2d(video->time_base);
  return 0.0;
}

QString publish(const QString &temporary, const QString &directory, const QString &source) {
  const QString stem = QFileInfo(source).completeBaseName() + "_Share";
  for (int n = 0; n < 10000; ++n) {
    const QString destination =
        QDir(directory).filePath(stem + (n ? "_" + QString::number(n) : QString()) + ".mp4");
    if (QFile::rename(temporary, destination)) return destination;
    check(QFile::exists(destination), "Cannot publish export file.");
  }
  throw std::runtime_error("Too many export filename collisions.");
}

// Target size for the conversion mode: never upscale, keep even dimensions.
struct ConversionSize { int width, height; };
ConversionSize conversionTarget(int width, int height, int maxHeight) {
  if (maxHeight <= 0 || height <= maxHeight)
    return {std::max(2, width & ~1), std::max(2, height & ~1)};
  const double scale = double(maxHeight) / double(height);
  return {std::max(2, int(std::lround(width * scale)) & ~1),
          std::max(2, maxHeight & ~1)};
}

int conversionBitrateKbps(int width, int height) {
  const double factor = double(width) * double(height) / (1920.0 * 1080.0);
  return std::clamp(int(std::lround(12000.0 * std::max(0.25, factor))), 3000, 40000);
}

// swscale is single-threaded on the legacy path and dominated this conversion:
// 11.9 s of a 14.2 s two-minute export. That is exactly the gap to ffmpeg's
// scale filter, which threads the same library (-filter_threads 1: 17.0 s,
// auto: 9.4 s on the same file).
//
// SwsContext::threads is honoured only by the new sws_scale_frame() entry
// point, which takes its geometry from the frames; the legacy sws_scale()
// ignores it. Measured at 1440p -> 1080p: 2.90 ms/frame with one thread,
// 0.78 ms with eight, on identical output.
//
// libswscale 7 (FFmpeg 6, Ubuntu 24.04 and the AppImage baseline) does NOT
// configure a fresh context from the frames: sws_scale_frame() on an
// allocated-but-uninitialised context reads through null internals and
// segfaults. Explicit geometry plus sws_init_context() works on every version
// still in use, and the threads option still applies (2.22 ms -> 0.65 ms at
// 1440p on 6.1). Later versions accept the same explicit setup, so one path
// serves both.
class ThreadedScaler {
public:
  ~ThreadedScaler() { if (m_ctx) sws_freeContext(m_ctx); }

  bool initialize() {
    if (!buildContext())
      return false;
    m_configured = false;
    return true;
  }

  bool scale(const AVFrame *src, AVFrame *dst) {
    if (!src || !dst)
      return false;
    if (!m_ctx || !m_configured || m_srcWidth != src->width ||
        m_srcHeight != src->height || m_srcFormat != src->format ||
        m_dstWidth != dst->width || m_dstHeight != dst->height ||
        m_dstFormat != dst->format) {
      if (!configure(src, dst))
        return false;
    }
    return sws_scale_frame(m_ctx, dst, src) >= 0;
  }

private:
  bool buildContext() {
    if (m_ctx)
      sws_freeContext(m_ctx);
    m_ctx = sws_alloc_context();
    if (!m_ctx)
      return false;
    const int threads = int(std::clamp<unsigned>(
        std::thread::hardware_concurrency(), 1u, 8u));
    av_opt_set_int(m_ctx, "threads", threads, 0);
    av_opt_set_int(m_ctx, "sws_flags", SWS_BILINEAR, 0);
    return true;
  }

  // Geometry has to be set explicitly, and the context initialised, before the
  // first sws_scale_frame() on libswscale 7.
  bool configure(const AVFrame *src, AVFrame *dst) {
    m_configured = false;
    if (!buildContext())
      return false;
    av_opt_set_int(m_ctx, "srcw", src->width, 0);
    av_opt_set_int(m_ctx, "srch", src->height, 0);
    av_opt_set_int(m_ctx, "src_format", src->format, 0);
    av_opt_set_int(m_ctx, "dstw", dst->width, 0);
    av_opt_set_int(m_ctx, "dsth", dst->height, 0);
    av_opt_set_int(m_ctx, "dst_format", dst->format, 0);
    if (sws_init_context(m_ctx, nullptr, nullptr) < 0) {
      sws_freeContext(m_ctx);
      m_ctx = nullptr;
      return false;
    }
    m_srcWidth = src->width;
    m_srcHeight = src->height;
    m_srcFormat = src->format;
    m_dstWidth = dst->width;
    m_dstHeight = dst->height;
    m_dstFormat = dst->format;
    m_configured = true;
    return true;
  }

  SwsContext *m_ctx = nullptr;
  bool m_configured = false;
  int m_srcWidth = 0, m_srcHeight = 0, m_srcFormat = AV_PIX_FMT_NONE;
  int m_dstWidth = 0, m_dstHeight = 0, m_dstFormat = AV_PIX_FMT_NONE;
};

// Re-encode a recording to H.264 + one stereo AAC track. This is the
// compatibility path for sources whose codec MP4 sharing cannot use as-is
// (HEVC/AV1) or when a smaller output is requested; it is orders of magnitude
// slower than the remux path, so it is never chosen implicitly.
ShareExportResult convertExport(const QString &source, const QString &directory,
                                const ShareExportOptions &options,
                                std::atomic_bool &cancel, std::atomic<double> &progress) {
  const SourceGuard guard(source);
  check(options.audioStreams.size() <= 8, "Invalid export options (up to eight audio tracks).");
  check(options.maxHeight == 0 || options.maxHeight == 720 || options.maxHeight == 1080,
        "Unsupported export resolution.");
  QSet<int> unique;
  for (int index : options.audioStreams) {
    check(index >= 0 && !unique.contains(index), "Invalid audio selection.");
    unique.insert(index);
  }
  const QList<double> gains = normalizedGains(options);
  check(QDir(directory).exists(), "Choose an existing export folder.");

  auto input = open(source, cancel);
  const int vIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
  check(vIndex >= 0, "No video stream found.");
  AVStream *inVideo = input->streams[vIndex];
  check(inVideo->codecpar->width >= 2 && inVideo->codecpar->height >= 2 &&
            inVideo->codecpar->width <= 16384 && inVideo->codecpar->height <= 16384,
        "Unsupported video dimensions.");
  for (int index : options.audioStreams) {
    check(index < int(input->nb_streams) &&
              input->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO,
          "Invalid audio selection.");
    unique.insert(index);
  }

  const ConversionSize target =
      conversionTarget(inVideo->codecpar->width, inVideo->codecpar->height, options.maxHeight);

  Codec decoder(avcodec_alloc_context3(avcodec_find_decoder(inVideo->codecpar->codec_id)));
  check(bool(decoder), "A decoder for this recording is unavailable.");
  ffcheck(avcodec_parameters_to_context(decoder.get(), inVideo->codecpar), "Cannot configure decoder");
  // avcodec_alloc_context3() leaves thread_count at 1, i.e. single-threaded;
  // 0 asks for one thread per core. Frame-threaded decoding is most of this
  // path's throughput: measured on a two-minute 1440p HEVC excerpt, 41.8 s
  // serialised against 10 s threaded, for identical output.
  decoder->thread_count = 0;
  ffcheck(avcodec_open2(decoder.get(), decoder->codec, nullptr), "Cannot start decoder");

  int64_t originUs = 0;
  if (inVideo->start_time != AV_NOPTS_VALUE)
    originUs = av_rescale_q(inVideo->start_time, inVideo->time_base, kUs);
  else if (input->start_time != AV_NOPTS_VALUE)
    originUs = input->start_time;

  int64_t fps = 60;
  const AVRational rate = av_guess_frame_rate(input.get(), inVideo, nullptr);
  if (rate.num > 0 && rate.den > 0)
    fps = std::clamp<int64_t>(std::llround(av_q2d(rate)), 1, 240);
  const int64_t frameStepUs = std::max<int64_t>(1, 1000000 / fps);

  // Encoder preference: NVENC (accepts system frames directly), then VA-API
  // (needs NV12 surfaces uploaded per frame), then software x264.
  struct Candidate { const char *name; bool vaapi; const char *preset; };
  const Candidate candidates[] = {
      {"h264_nvenc", false, "p3"}, {"h264_vaapi", true, nullptr}, {"libx264", false, "veryfast"}};
  Codec videoEncoder;
  bool useVaapi = false;
  QString encoderName;
  for (const Candidate &candidate : candidates) {
    const AVCodec *codec = avcodec_find_encoder_by_name(candidate.name);
    if (!codec)
      continue;
    Codec attempt(avcodec_alloc_context3(codec));
    if (!attempt)
      continue;
    attempt->width = target.width;
    attempt->height = target.height;
    attempt->time_base = AVRational{1, 1000};
    attempt->framerate = AVRational{int(fps), 1};
    attempt->gop_size = int(fps) * 2;
    attempt->max_b_frames = 0;
    attempt->pix_fmt = candidate.vaapi ? AV_PIX_FMT_VAAPI : AV_PIX_FMT_YUV420P;
    attempt->bit_rate = int64_t(conversionBitrateKbps(target.width, target.height)) * 1000;
    if (candidate.preset)
      av_opt_set(attempt->priv_data, "preset", candidate.preset, 0);
    if (candidate.vaapi) {
      AVBufferRef *device = nullptr;
      if (av_hwdevice_ctx_create(&device, AV_HWDEVICE_TYPE_VAAPI, nullptr, nullptr, 0) < 0)
        continue;
      attempt->hw_device_ctx = av_buffer_ref(device);
      AVBufferRef *frames = av_hwframe_ctx_alloc(device);
      av_buffer_unref(&device);
      if (!frames)
        continue;
      auto *context = reinterpret_cast<AVHWFramesContext *>(frames->data);
      context->format = AV_PIX_FMT_VAAPI;
      context->sw_format = AV_PIX_FMT_NV12;
      context->width = target.width;
      context->height = target.height;
      context->initial_pool_size = 20;
      if (av_hwframe_ctx_init(frames) < 0) { av_buffer_unref(&frames); continue; }
      attempt->hw_frames_ctx = av_buffer_ref(frames);
      av_buffer_unref(&frames);
    }
    if (avcodec_open2(attempt.get(), codec, nullptr) < 0)
      continue;
    videoEncoder = std::move(attempt);
    useVaapi = candidate.vaapi;
    encoderName = QString::fromUtf8(candidate.name);
    break;
  }
  check(videoEncoder != nullptr, "No H.264 encoder is available for conversion.");
  qInfo() << "Share export: converting to H.264 with" << encoderName << target.width << "x" << target.height;

  QTemporaryFile temporary(QDir(directory).filePath(".trophy-export-XXXXXX.part"));
  check(temporary.open(), "Cannot create export file.");
  const QString tmp = temporary.fileName();
  temporary.close();

  Output out;
  ffcheck(avformat_alloc_output_context2(&out.ctx, nullptr, "mp4",
                                         QFile::encodeName(tmp).constData()),
          "Cannot create MP4 output");
  for (const AVDictionaryEntry *tag = nullptr;
       (tag = av_dict_get(input->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) != nullptr;) {
    if (qstrcmp(tag->key, "title") == 0 || qstrcmp(tag->key, "trophy-kind") == 0)
      av_dict_set(&out.ctx->metadata, tag->key, tag->value, 0);
  }

  if (out.ctx->oformat->flags & AVFMT_GLOBALHEADER)
    videoEncoder->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  AVStream *outVideo = avformat_new_stream(out.ctx, nullptr);
  check(outVideo != nullptr, "Cannot allocate export stream.");
  ffcheck(avcodec_parameters_from_context(outVideo->codecpar, videoEncoder.get()),
          "Cannot configure export stream");
  outVideo->codecpar->codec_tag = 0;
  outVideo->time_base = videoEncoder->time_base;

  // Share copies always carry exactly one merged stereo track.
  Codec audioCodec;
  AVStream *outAudio = nullptr;
  Frame audioFrame;
  size_t bufferedAudioBytes = 0;
  std::vector<std::unique_ptr<AudioTrack>> tracks;
  if (!unique.isEmpty()) {
    audioCodec = encoder("aac");
    audioCodec->sample_rate = kRate;
    audioCodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
    av_channel_layout_default(&audioCodec->ch_layout, 2);
    audioCodec->time_base = kSamples;
    audioCodec->bit_rate = 192000;
    if (out.ctx->oformat->flags & AVFMT_GLOBALHEADER) audioCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    ffcheck(avcodec_open2(audioCodec.get(), audioCodec->codec, nullptr), "Cannot start AAC export");
    outAudio = addStream(out, audioCodec.get());
    av_dict_set(&outAudio->metadata, "title", "Stereo mix", 0);
    audioFrame = frame();
    audioFrame->format = audioCodec->sample_fmt;
    audioFrame->sample_rate = kRate;
    ffcheck(av_channel_layout_copy(&audioFrame->ch_layout, &audioCodec->ch_layout),
            "Cannot configure audio frame");
    audioFrame->nb_samples = audioCodec->frame_size;
    check(audioFrame->nb_samples > 0 && audioFrame->nb_samples <= 8192,
          "Unsupported export audio frame size.");
    ffcheck(av_frame_get_buffer(audioFrame.get(), 0), "Cannot allocate audio frame");
    const int64_t originSamples = av_rescale_q(originUs, kUs, kSamples);
    for (int index : options.audioStreams) {
      auto track = std::make_unique<AudioTrack>(source, index, originSamples, bufferedAudioBytes, cancel);
      tracks.push_back(std::move(track));
    }
  }

  ffcheck(avio_open2(&out.ctx->pb, QFile::encodeName(tmp).constData(), AVIO_FLAG_WRITE,
      &out.ctx->interrupt_callback, nullptr), "Cannot open export output");
  AVDictionary *muxOptions = nullptr;
  // Standard tags only: see the note in the copy path.
  const int optionResult = av_dict_set(&muxOptions, "movflags", "+faststart", 0);
  if (optionResult < 0) { av_dict_free(&muxOptions); ffcheck(optionResult, "Cannot configure MP4 fast start"); }
  const int header = avformat_write_header(out.ctx, &muxOptions);
  av_dict_free(&muxOptions);
  ffcheck(header, "Cannot write export header");

  int64_t audioAt = 0;
  const int64_t audioFrameSamples = audioFrame ? audioFrame->nb_samples : 0;
  std::vector<float> mix(size_t(audioFrameSamples) * 2, 0.0f);
  auto writeAudioUntil = [&](int64_t untilUs) {
    if (!audioCodec) return;
    const int64_t wanted = av_rescale_q(std::max<int64_t>(0, untilUs), kUs, kSamples);
    while (audioAt < wanted) {
      cancelled(cancel);
      const int n = int(audioFrameSamples);
      std::fill(mix.begin(), mix.end(), 0.0f);
      for (int i = 0; i < int(tracks.size()); ++i)
        tracks[size_t(i)]->add(mix.data(), audioAt, n, float(gains[i]));
      ffcheck(av_frame_make_writable(audioFrame.get()), "Cannot write audio frame");
      for (int ch = 0; ch < 2; ++ch) {
        auto *plane = reinterpret_cast<float *>(audioFrame->data[ch]);
        for (int i = 0; i < n; ++i)
          plane[i] = std::isfinite(mix[size_t(i) * 2 + ch])
                         ? std::clamp(mix[size_t(i) * 2 + ch], -1.0f, 1.0f)
                         : 0.0f;
      }
      audioFrame->pts = audioAt;
      out.writeAudio(audioCodec.get(), outAudio, audioFrame.get());
      audioAt += n;
    }
  };

  ThreadedScaler scaler;
  bool scalerReady = false;
  Frame scaled = frame();
  Frame hardware;
  int64_t lastEndUs = 0;
  bool haveScaled = false;
  bool hdrChecked = false;

  auto encodeDecoded = [&](AVFrame *decoded) {
    if (!hdrChecked) {
      hdrChecked = true;
      check(decoded->color_trc != AVCOL_TRC_SMPTE2084 &&
                decoded->color_trc != AVCOL_TRC_ARIB_STD_B67,
            "HDR-to-SDR share export is not supported yet.");
      scaled->format = useVaapi ? AV_PIX_FMT_NV12 : AV_PIX_FMT_YUV420P;
      scaled->width = target.width;
      scaled->height = target.height;
      ffcheck(av_frame_get_buffer(scaled.get(), 32), "Cannot allocate export frame.");
    }
    const int64_t ptsUs = decoded->best_effort_timestamp == AV_NOPTS_VALUE
        ? lastEndUs
        : av_rescale_q(decoded->best_effort_timestamp, inVideo->time_base, kUs) - originUs;
    // Capture is damage-driven, so a still screen leaves the previous frame
    // covering a stretch of time. Re-encoding can fill that honestly: send the
    // frame we already scaled again, at the frame rate, so the output is
    // constant-rate and no sample spans the still. Each repeat costs a
    // near-empty P-frame rather than a keyframe.
    if (haveScaled && ptsUs - lastEndUs > frameStepUs * 3 / 2) {
      for (int64_t at = lastEndUs + frameStepUs; at + frameStepUs / 2 < ptsUs;
           at += frameStepUs) {
        cancelled(cancel);
        writeAudioUntil(at);
        const int64_t repeatMs = av_rescale_q(at, kUs, videoEncoder->time_base);
        AVFrame *repeat = scaled.get();
        scaled->pts = repeatMs;
        if (useVaapi) {
          if (!hardware)
            hardware = frame();
          av_frame_unref(hardware.get());
          ffcheck(av_hwframe_get_buffer(videoEncoder->hw_frames_ctx, hardware.get(), 0),
                  "Cannot allocate export surface.");
          ffcheck(av_hwframe_transfer_data(hardware.get(), scaled.get(), 0),
                  "Cannot upload export frame.");
          hardware->pts = repeatMs;
          repeat = hardware.get();
        }
        ffcheck(avcodec_send_frame(videoEncoder.get(), repeat),
                "Cannot encode export frame");
        auto drained = packet();
        while (true) {
          const int result = avcodec_receive_packet(videoEncoder.get(), drained.get());
          if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
          ffcheck(result, "Cannot receive encoded export packet");
          av_packet_rescale_ts(drained.get(), videoEncoder->time_base, outVideo->time_base);
          drained->stream_index = outVideo->index;
          ffcheck(av_interleaved_write_frame(out.ctx, drained.get()),
                  "Cannot write export (check free disk space)");
        }
      }
    }
    writeAudioUntil(ptsUs);
    if (!scalerReady) {
      scalerReady = true;
      check(scaler.initialize(), "Cannot configure export scaling.");
    }
    ffcheck(av_frame_make_writable(scaled.get()), "Cannot write export frame.");
    check(scaler.scale(decoded, scaled.get()), "Cannot scale export frame");
    haveScaled = true;
    const int64_t ptsMs = av_rescale_q(ptsUs, kUs, videoEncoder->time_base);
    AVFrame *feed = scaled.get();
    scaled->pts = ptsMs;
    if (useVaapi) {
      if (!hardware)
        hardware = frame();
      av_frame_unref(hardware.get());
      ffcheck(av_hwframe_get_buffer(videoEncoder->hw_frames_ctx, hardware.get(), 0),
              "Cannot allocate export surface.");
      ffcheck(av_hwframe_transfer_data(hardware.get(), scaled.get(), 0),
              "Cannot upload export frame.");
      hardware->pts = ptsMs;
      feed = hardware.get();
    }
    ffcheck(avcodec_send_frame(videoEncoder.get(), feed), "Cannot encode export frame");
    auto encoded = packet();
    while (true) {
      const int result = avcodec_receive_packet(videoEncoder.get(), encoded.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
      ffcheck(result, "Cannot receive encoded export packet");
      av_packet_rescale_ts(encoded.get(), videoEncoder->time_base, outVideo->time_base);
      encoded->stream_index = outVideo->index;
      ffcheck(av_interleaved_write_frame(out.ctx, encoded.get()),
              "Cannot write export (check free disk space)");
    }
    lastEndUs = ptsUs + (decoded->duration > 0
        ? av_rescale_q(decoded->duration, inVideo->time_base, kUs)
        : int64_t(1000000 / std::max<int64_t>(1, fps)));
  };

  const qint64 totalBytes = std::max<qint64>(1, guard.info.size());
  qint64 readBytes = 0;
  Frame decoded = frame();
  auto pkt = packet();
  auto drainDecoder = [&](bool flushing) {
    while (true) {
      const int result = avcodec_receive_frame(decoder.get(), decoded.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
      ffcheck(result, "Cannot decode recording");
      encodeDecoded(decoded.get());
      av_frame_unref(decoded.get());
    }
    (void)flushing;
  };
  for (unsigned i = 0; i < input->nb_streams; ++i)
    if (int(i) != vIndex) input->streams[i]->discard = AVDISCARD_ALL;
  while (true) {
    cancelled(cancel);
    const int read = av_read_frame(input.get(), pkt.get());
    if (read == AVERROR_EOF) break;
    ffcheck(read, "Cannot read recording");
    readBytes += pkt->size + 32;
    if (pkt->stream_index == vIndex) {
      ffcheck(avcodec_send_packet(decoder.get(), pkt.get()), "Cannot decode recording");
      drainDecoder(false);
    }
    av_packet_unref(pkt.get());
    progress.store(std::min(0.99, double(input->pb ? avio_tell(input->pb) : readBytes) / double(totalBytes)));
  }
  ffcheck(avcodec_send_packet(decoder.get(), nullptr), "Cannot drain decoder");
  drainDecoder(true);
  ffcheck(avcodec_send_frame(videoEncoder.get(), nullptr), "Cannot drain export encoder");
  {
    auto encoded = packet();
    while (true) {
      const int result = avcodec_receive_packet(videoEncoder.get(), encoded.get());
      if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) break;
      ffcheck(result, "Cannot receive encoded export packet");
      av_packet_rescale_ts(encoded.get(), videoEncoder->time_base, outVideo->time_base);
      encoded->stream_index = outVideo->index;
      ffcheck(av_interleaved_write_frame(out.ctx, encoded.get()),
              "Cannot write export (check free disk space)");
    }
  }
  if (audioCodec) {
    for (;;) {
      int64_t wanted = av_rescale_q(lastEndUs, kUs, kSamples);
      for (auto &track : tracks) wanted = std::max(wanted, track->nextEnd(audioAt));
      if (wanted <= audioAt) break;
      writeAudioUntil(av_rescale_q_rnd(wanted, kSamples, kUs, AV_ROUND_UP));
    }
    out.writeAudio(audioCodec.get(), outAudio, nullptr);
  }
  ffcheck(av_write_trailer(out.ctx), "Cannot finalize export");
  avio_flush(out.ctx->pb);
  ffcheck(out.ctx->pb->error, "Cannot flush export");
  ffcheck(avio_closep(&out.ctx->pb), "Cannot close export output");
  cancelled(cancel);
  guard.verifyUnchanged(source);
  progress.store(1);
  return {publish(tmp, directory, source), {}, sourceSeconds(inVideo),
            inVideo->codecpar->codec_id == AV_CODEC_ID_AV1};
}

} // namespace

QVariantMap ShareExportEngine::inspect(const QString &source, std::atomic_bool &cancel) {
  try {
    auto input = open(source, cancel);
    const int index = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(index >= 0, "No video stream found.");
    const AVCodecID videoCodecId = input->streams[index]->codecpar->codec_id;
    const QString videoCodec = QString::fromUtf8(avcodec_get_name(videoCodecId));
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
    return {{"tracks", tracks},
            {"durationSeconds", input->duration > 0 ? double(input->duration) / AV_TIME_BASE : 0.0},
            {"videoCodec", videoCodec},
            {"videoCodecLabel", shareVideoCodecLabel(videoCodecId)},
            // Copy needs a video codec MP4 can carry; it keeps the original
            // codec, so H.264 is the one every sharing service accepts.
            {"copyable", copyableVideo(videoCodecId)},
            {"shareable", videoCodecId == AV_CODEC_ID_H264},
            // Cost model, in seconds of export per second of source, from
            // measured runs on the dev host (NVENC, SSD). A two-minute 1440p
            // excerpt copies in 0.2 s, mixes three tracks in 2.6 s, and
            // converts to 1080p in 12 s (H.264/HEVC) or 20 s (AV1, whose
            // software decoder is the expensive part). Guidance for the
            // dialog, not a promise: a host without a hardware encoder
            // converts far slower.
            {"estimateCopyBase", 0.0017},
            {"estimateCopyPerTrack", 0.0100},   // each extra mixed track
            {"estimateConvert", videoCodecId == AV_CODEC_ID_AV1 ? 0.165 : 0.100}};
  } catch (const std::exception &e) { return {{"error", QString::fromUtf8(e.what())}}; }
}

ShareExportResult ShareExportEngine::run(const QString &source, const QString &directory,
    const ShareExportOptions &options, std::atomic_bool &cancel, std::atomic<double> &progress) {
  try {
    if (options.mode == ShareExportOptions::Mode::Convert)
      return convertExport(source, directory, options, cancel, progress);
    const SourceGuard guard(source);
    check(options.audioStreams.size() <= 8, "Invalid export options (up to eight audio tracks).");
    QSet<int> unique;
    for (int index : options.audioStreams) {
      check(index >= 0 && !unique.contains(index), "Invalid audio selection.");
      unique.insert(index);
    }
    const QList<double> gains = normalizedGains(options);
    check(QDir(directory).exists(), "Choose an existing export folder.");

    auto input = open(source, cancel);
    const int vIndex = av_find_best_stream(input.get(), AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    check(vIndex >= 0, "No video stream found.");
    AVStream *inVideo = input->streams[vIndex];
    check(copyableVideo(inVideo->codecpar->codec_id),
          "This recording's video codec cannot be placed in an MP4 container.");
    check(inVideo->codecpar->width >= 2 && inVideo->codecpar->height >= 2 &&
              inVideo->codecpar->width <= 16384 && inVideo->codecpar->height <= 16384,
          "Unsupported video dimensions.");

    int singleAudioIndex = -1;
    for (int index : options.audioStreams) {
      check(index < int(input->nb_streams) &&
                input->streams[index]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO,
            "Invalid audio selection.");
      if (options.audioStreams.size() == 1)
        singleAudioIndex = index;
    }
    // A single already-stereo AAC track needs no mixing and is copied as-is.
    // Anything else is mixed into one stereo AAC track, because chat apps
    // cannot switch between tracks.
    // A reduced gain needs re-encoding; stream copy cannot apply volume.
    const bool copySingleAudio = singleAudioIndex >= 0 &&
        gains.value(0, 1.0) >= 0.999 &&
        input->streams[singleAudioIndex]->codecpar->codec_id == AV_CODEC_ID_AAC &&
        input->streams[singleAudioIndex]->codecpar->ch_layout.nb_channels == 2;

    int64_t originUs = 0;
    if (inVideo->start_time != AV_NOPTS_VALUE)
      originUs = av_rescale_q(inVideo->start_time, inVideo->time_base, kUs);
    else if (input->start_time != AV_NOPTS_VALUE)
      originUs = input->start_time;
    check(originUs >= -(INT64_C(1) << 40) && originUs <= (INT64_C(1) << 40),
          "Invalid recording timestamp origin.");

    QTemporaryFile temporary(QDir(directory).filePath(".trophy-export-XXXXXX.part"));
    check(temporary.open(), "Cannot create export file.");
    const QString tmp = temporary.fileName();
    temporary.close();

    Output out;
    // The filename is required: the MP4 fast-start second pass uses ctx->url.
    ffcheck(avformat_alloc_output_context2(&out.ctx, nullptr, "mp4",
                                           QFile::encodeName(tmp).constData()),
            "Cannot create MP4 output");
    for (const AVDictionaryEntry *tag = nullptr;
         (tag = av_dict_get(input->metadata, "", tag, AV_DICT_IGNORE_SUFFIX)) != nullptr;) {
      if (qstrcmp(tag->key, "title") == 0 || qstrcmp(tag->key, "trophy-kind") == 0)
        av_dict_set(&out.ctx->metadata, tag->key, tag->value, 0);
    }

    AVStream *outVideo = avformat_new_stream(out.ctx, nullptr);
    check(outVideo != nullptr, "Cannot allocate export stream.");
    ffcheck(avcodec_parameters_copy(outVideo->codecpar, inVideo->codecpar), "Cannot copy video parameters");
    // Let the muxer pick the tag, except for HEVC: MP4 defaults to 'hev1',
    // which Apple's players (Safari, QuickTime, iOS) refuse. 'hvc1' carries
    // the same bitstream with parameter sets in hvcC and plays everywhere.
    outVideo->codecpar->codec_tag =
        inVideo->codecpar->codec_id == AV_CODEC_ID_HEVC ? MKTAG('h', 'v', 'c', '1') : 0;
    outVideo->time_base = inVideo->time_base;
    outVideo->avg_frame_rate = inVideo->avg_frame_rate;
    outVideo->sample_aspect_ratio = inVideo->sample_aspect_ratio;

    Codec audioCodec;
    AVStream *outAudio = nullptr;
    AVStream *copiedAudio = nullptr;
    AVStream *inCopiedAudio = nullptr;
    Frame audioFrame;
    size_t bufferedAudioBytes = 0;
    std::vector<std::unique_ptr<AudioTrack>> tracks;
    if (copySingleAudio) {
      inCopiedAudio = input->streams[singleAudioIndex];
      copiedAudio = avformat_new_stream(out.ctx, nullptr);
      check(copiedAudio != nullptr, "Cannot allocate export stream.");
      ffcheck(avcodec_parameters_copy(copiedAudio->codecpar, inCopiedAudio->codecpar),
              "Cannot copy audio parameters");
      copiedAudio->codecpar->codec_tag = 0;
      copiedAudio->time_base = inCopiedAudio->time_base;
    } else if (!unique.isEmpty()) {
      // Mixed audio is AAC, not MP3. MP3 carried in MP4 is legal but Apple's
      // players do not reliably handle mp4a/MPEG-1, which defeats the point of
      // a compatibility export. Native AAC costs ~35 s more on a two-hour
      // three-track export (85 s -> ~119 s) and makes every share copy
      // H.264 + AAC regardless of which mode produced it.
      audioCodec = encoder("aac");
      audioCodec->sample_rate = kRate;
      audioCodec->sample_fmt = AV_SAMPLE_FMT_FLTP;
      av_channel_layout_default(&audioCodec->ch_layout, 2);
      audioCodec->time_base = kSamples;
      audioCodec->bit_rate = 192000;
      if (out.ctx->oformat->flags & AVFMT_GLOBALHEADER) audioCodec->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
      ffcheck(avcodec_open2(audioCodec.get(), audioCodec->codec, nullptr),
              "Cannot start the export audio encoder");
      outAudio = addStream(out, audioCodec.get());
      av_dict_set(&outAudio->metadata, "title", "Stereo mix", 0);
      audioFrame = frame();
      audioFrame->format = audioCodec->sample_fmt;
      audioFrame->sample_rate = kRate;
      ffcheck(av_channel_layout_copy(&audioFrame->ch_layout, &audioCodec->ch_layout),
              "Cannot configure audio frame");
      audioFrame->nb_samples = audioCodec->frame_size;
      check(audioFrame->nb_samples > 0 && audioFrame->nb_samples <= 8192,
            "Unsupported export audio frame size.");
      ffcheck(av_frame_get_buffer(audioFrame.get(), 0), "Cannot allocate audio frame");
      const int64_t originSamples = av_rescale_q(originUs, kUs, kSamples);
      for (int index : options.audioStreams) {
        auto track = std::make_unique<AudioTrack>(source, index, originSamples, bufferedAudioBytes, cancel);
        tracks.push_back(std::move(track));
      }
    }

    ffcheck(avio_open2(&out.ctx->pb, QFile::encodeName(tmp).constData(), AVIO_FLAG_WRITE,
        &out.ctx->interrupt_callback, nullptr), "Cannot open export output");
    // Plain faststart: use_metadata_tags writes a keys/mdta box that ffmpeg
    // itself warns about ("UDTA parsing failed retrying raw"), and stricter
    // players refuse it. The title lands in the standard ©nam entry instead.
    AVDictionary *muxOptions = nullptr;
    const int optionResult = av_dict_set(&muxOptions, "movflags", "+faststart", 0);
    if (optionResult < 0) { av_dict_free(&muxOptions); ffcheck(optionResult, "Cannot configure MP4 fast start"); }
    const int header = avformat_write_header(out.ctx, &muxOptions);
    av_dict_free(&muxOptions);
    ffcheck(header, "Cannot write export header");

    int64_t audioAt = 0;
    const int64_t audioFrameSamples = audioFrame ? audioFrame->nb_samples : 0;
    // Reused across frames: one small allocation per export instead of ~300k
    // for a two-hour mix.
    std::vector<float> mix(size_t(audioFrameSamples) * 2, 0.0f);
    auto writeAudioUntil = [&](int64_t untilUs) {
      if (!audioCodec) return;
      const int64_t target = av_rescale_q(untilUs, kUs, kSamples);
      while (audioAt < target) {
        cancelled(cancel);
        const int n = int(audioFrameSamples);
        std::fill(mix.begin(), mix.end(), 0.0f);
        for (int i = 0; i < int(tracks.size()); ++i)
          tracks[size_t(i)]->add(mix.data(), audioAt, n, float(gains[i]));
        ffcheck(av_frame_make_writable(audioFrame.get()), "Cannot write audio frame");
        for (int ch = 0; ch < 2; ++ch) {
          auto *plane = reinterpret_cast<float *>(audioFrame->data[ch]);
          for (int i = 0; i < n; ++i)
            plane[i] = std::isfinite(mix[size_t(i) * 2 + ch])
                           ? std::clamp(mix[size_t(i) * 2 + ch], -1.0f, 1.0f)
                           : 0.0f;
        }
        audioFrame->pts = audioAt;
        out.writeAudio(audioCodec.get(), outAudio, audioFrame.get());
        audioAt += n;
      }
    };

    const qint64 totalBytes = std::max<qint64>(1, guard.info.size());
    qint64 readBytes = 0;
    const int64_t originTicks = av_rescale_q(originUs, kUs, outVideo->time_base);
    // Audio cursors read ahead only as far as the next requested mix block;
    // packet interleaving no longer determines whether PCM is available.
    int64_t lastEndUs = 0;
    auto pkt = packet();
    for (unsigned i = 0; i < input->nb_streams; ++i)
      if (int(i) != vIndex && !(copiedAudio && int(i) == singleAudioIndex))
        input->streams[i]->discard = AVDISCARD_ALL;
    while (true) {
      cancelled(cancel);
      const int read = av_read_frame(input.get(), pkt.get());
      if (read == AVERROR_EOF) break;
      ffcheck(read, "Cannot read recording");
      readBytes += pkt->size + 32;
      if (pkt->stream_index == vIndex) {
        // av_interleaved_write_frame consumes and blanks the packet; retain
        // duration in its original time base before rescaling or handing off.
        const int64_t stepUs = pkt->duration > 0
            ? av_rescale_q(pkt->duration, inVideo->time_base, kUs)
            : (inVideo->avg_frame_rate.num > 0 && inVideo->avg_frame_rate.den > 0
                   ? int64_t(1000000) * inVideo->avg_frame_rate.den /
                         inVideo->avg_frame_rate.num
                   : int64_t(16667));
        const int64_t ptsUs = pkt->pts == AV_NOPTS_VALUE
            ? lastEndUs
            : av_rescale_q(pkt->pts, inVideo->time_base, kUs) - originUs;
        if (audioCodec)
          writeAudioUntil(std::max<int64_t>(0, ptsUs));
        av_packet_rescale_ts(pkt.get(), inVideo->time_base, outVideo->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts = std::max<int64_t>(0, pkt->pts - originTicks);
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= originTicks;
        // A recording can open with a packet that carries no timestamp at all
        // (MKV keeps DTS-less heads). Forwarding that made the MP4 muxer warn
        // and invent one; give it the frame's own timeline position instead.
        // Only the both-missing case is filled in: when a stream has B-frames,
        // deriving DTS from PTS for one packet can put it ahead of the next
        // packet's real DTS, which the muxer rejects.
        if (pkt->pts == AV_NOPTS_VALUE) {
          pkt->pts = pkt->dts != AV_NOPTS_VALUE
              ? pkt->dts
              : av_rescale_q(std::max<int64_t>(0, ptsUs), kUs, outVideo->time_base);
          if (pkt->dts == AV_NOPTS_VALUE)
            pkt->dts = pkt->pts;
        }
        pkt->stream_index = outVideo->index;
        pkt->pos = -1;
        ffcheck(av_interleaved_write_frame(out.ctx, pkt.get()),
                "Cannot write export (check free disk space)");
        // Advance the fallback clock even for packets that carry no duration,
        // so a source with no timestamps at all still gets a monotonic
        // sequence instead of a run of identical ones.
        lastEndUs = ptsUs + std::max<int64_t>(1, stepUs);
      } else if (copiedAudio != nullptr && pkt->stream_index == singleAudioIndex) {
        av_packet_rescale_ts(pkt.get(), inCopiedAudio->time_base, copiedAudio->time_base);
        const int64_t shift = av_rescale_q(originUs, kUs, copiedAudio->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
        pkt->stream_index = copiedAudio->index;
        pkt->pos = -1;
        ffcheck(av_interleaved_write_frame(out.ctx, pkt.get()),
                "Cannot write export (check free disk space)");
      }
      av_packet_unref(pkt.get());
      progress.store(std::min(0.99, double(input->pb ? avio_tell(input->pb) : readBytes) / double(totalBytes)));
    }

    if (audioCodec) {
      for (;;) {
        int64_t wanted = av_rescale_q(lastEndUs, kUs, kSamples);
        for (auto &track : tracks) wanted = std::max(wanted, track->nextEnd(audioAt));
        if (wanted <= audioAt) break;
        writeAudioUntil(av_rescale_q_rnd(wanted, kSamples, kUs, AV_ROUND_UP));
      }
      out.writeAudio(audioCodec.get(), outAudio, nullptr);
    }
    ffcheck(av_write_trailer(out.ctx), "Cannot finalize export");
    avio_flush(out.ctx->pb);
    ffcheck(out.ctx->pb->error, "Cannot flush export");
    ffcheck(avio_closep(&out.ctx->pb), "Cannot close export output");
    cancelled(cancel);
    guard.verifyUnchanged(source);
    progress.store(1);
    return {publish(tmp, directory, source), {}, sourceSeconds(inVideo),
            inVideo->codecpar->codec_id == AV_CODEC_ID_AV1};
  } catch (const std::exception &e) {
    return {{}, QString::fromUtf8(e.what())};
  }
}
} // namespace TrophyBackend
