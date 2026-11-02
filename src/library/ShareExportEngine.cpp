/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "ShareExportEngine.h"
#include "RecordingAnnotations.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryFile>
#include <QSet>
#include <algorithm>
#include <cmath>
#include <deque>
#include <memory>
#include <stdexcept>
#include <vector>
#include <sys/stat.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/opt.h>
#include <libswresample/swresample.h>
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

// One selected track, decoded from packets taken from the single demux pass
// (no per-track file reopening), resampled to 48 kHz stereo float and buffered
// on the video timeline until the mixer consumes it.
class AudioTrack {
public:
  AudioTrack(AVStream *stream, int64_t originSamples)
      : m_stream(stream), m_origin(originSamples) {
    const AVCodec *decoder = avcodec_find_decoder(stream->codecpar->codec_id);
    check(decoder, "A decoder for this recording is unavailable.");
    m_codec.reset(avcodec_alloc_context3(decoder));
    check(bool(m_codec), "Cannot allocate decoder.");
    ffcheck(avcodec_parameters_to_context(m_codec.get(), stream->codecpar), "Cannot configure decoder");
    m_codec->max_pixels = INT64_C(8192) * 4320;
    ffcheck(avcodec_open2(m_codec.get(), decoder, nullptr), "Cannot start decoder");
  }
  ~AudioTrack() {
    if (m_swr) swr_free(&m_swr);
    av_channel_layout_uninit(&m_layout);
  }
  bool hasOutput() const { return m_haveOutput; }
  int64_t endSample() const { return m_end; }
  void push(AVPacket *packet) {
    ffcheck(avcodec_send_packet(m_codec.get(), packet), "Cannot submit recording packet");
    drain();
  }
  void flush() {
    ffcheck(avcodec_send_packet(m_codec.get(), nullptr), "Cannot drain decoder");
    drain();
  }
  void add(float *mix, int64_t at, int count, float gain) {
    while (!m_chunks.empty()) {
      const Chunk &first = m_chunks.front();
      if (first.start + int64_t(first.samples.size() / 2) <= at) m_chunks.pop_front();
      else break;
    }
    for (const Chunk &chunk : m_chunks) {
      const int64_t chunkEnd = chunk.start + int64_t(chunk.samples.size() / 2);
      const int64_t begin = std::max<int64_t>(chunk.start, at);
      const int64_t end = std::min<int64_t>(chunkEnd, at + count);
      if (begin >= end) continue;
      for (int64_t i = begin; i < end; ++i)
        for (int ch = 0; ch < 2; ++ch)
          mix[(i - at) * 2 + ch] += chunk.samples[size_t((i - chunk.start) * 2 + ch)] * gain;
    }
  }
private:
  struct Chunk { int64_t start; std::vector<float> samples; };
  void drain() {
    while (true) {
      const int result = avcodec_receive_frame(m_codec.get(), m_frame.get());
      if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) break;
      ffcheck(result, "Cannot decode recording");
      convert(m_frame.get());
      av_frame_unref(m_frame.get());
    }
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
    std::vector<float> samples(size_t(std::max(4096, count)) * 2);
    auto *out = reinterpret_cast<uint8_t *>(samples.data());
    const int produced = swr_convert(m_swr, &out, int(samples.size() / 2),
        const_cast<const uint8_t **>(f->extended_data), f->nb_samples);
    ffcheck(produced, "Cannot convert audio");
    samples.resize(size_t(produced) * 2);
    m_end = startSample + produced;
    if (produced > 0) {
      m_haveOutput = true;
      m_chunks.push_back({startSample, std::move(samples)});
    }
  }
  AVStream *m_stream;
  Codec m_codec;
  Frame m_frame = frame();
  SwrContext *m_swr = nullptr;
  int m_rate = 0, m_channels = 0, m_format = -1;
  AVChannelLayout m_layout{};
  int64_t m_origin;
  int64_t m_end = 0;
  bool m_haveOutput = false;
  std::deque<Chunk> m_chunks;
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
            {"shareable", videoCodecId == AV_CODEC_ID_H264}};
  } catch (const std::exception &e) { return {{"error", QString::fromUtf8(e.what())}}; }
}

ShareExportResult ShareExportEngine::run(const QString &source, const QString &directory,
    const ShareExportOptions &options, std::atomic_bool &cancel, std::atomic<double> &progress) {
  try {
    const SourceGuard guard(source);
    check(options.audioStreams.size() <= 8, "Invalid export options (up to eight audio tracks).");
    QSet<int> unique;
    for (int index : options.audioStreams) {
      check(index >= 0 && !unique.contains(index), "Invalid audio selection.");
      unique.insert(index);
    }
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
    const bool copySingleAudio = singleAudioIndex >= 0 &&
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
    outVideo->codecpar->codec_tag = 0; // Let the MP4 muxer choose the tag.
    outVideo->time_base = inVideo->time_base;
    outVideo->avg_frame_rate = inVideo->avg_frame_rate;
    outVideo->sample_aspect_ratio = inVideo->sample_aspect_ratio;

    Codec audioCodec;
    AVStream *outAudio = nullptr;
    AVStream *copiedAudio = nullptr;
    AVStream *inCopiedAudio = nullptr;
    Frame audioFrame;
    std::vector<std::unique_ptr<AudioTrack>> tracks;
    QHash<int, AudioTrack *> trackByStream;
    if (copySingleAudio) {
      inCopiedAudio = input->streams[singleAudioIndex];
      copiedAudio = avformat_new_stream(out.ctx, nullptr);
      check(copiedAudio != nullptr, "Cannot allocate export stream.");
      ffcheck(avcodec_parameters_copy(copiedAudio->codecpar, inCopiedAudio->codecpar),
              "Cannot copy audio parameters");
      copiedAudio->codecpar->codec_tag = 0;
      copiedAudio->time_base = inCopiedAudio->time_base;
    } else if (!unique.isEmpty()) {
      // MP3 encodes ~3x faster than the native AAC encoder, and MP4 carries
      // it as mp4a everywhere that matters. Fall back to AAC when the build
      // has no libmp3lame.
      const char *mixEncoder =
          avcodec_find_encoder_by_name("libmp3lame") ? "libmp3lame" : "aac";
      audioCodec = encoder(mixEncoder);
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
        auto track = std::make_unique<AudioTrack>(input->streams[index], originSamples);
        trackByStream.insert(index, track.get());
        tracks.push_back(std::move(track));
      }
    }

    ffcheck(avio_open2(&out.ctx->pb, QFile::encodeName(tmp).constData(), AVIO_FLAG_WRITE,
        &out.ctx->interrupt_callback, nullptr), "Cannot open export output");
    AVDictionary *muxOptions = nullptr;
    const int optionResult = av_dict_set(&muxOptions, "movflags", "+faststart+use_metadata_tags", 0);
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
        for (auto &track : tracks) track->add(mix.data(), audioAt, n, 1.0f / float(tracks.size()));
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
    // Emit mixed audio slightly behind the video clock: audio packets for a
    // timestamp can arrive just after the video packet that precedes them, and
    // treating "not read yet" as silence would punch holes in the mix.
    constexpr int64_t kAudioLeadUs = 1000000;
    int64_t lastVideoUs = 0;
    int64_t lastEndUs = 0;
    auto pkt = packet();
    while (true) {
      cancelled(cancel);
      const int read = av_read_frame(input.get(), pkt.get());
      if (read == AVERROR_EOF) break;
      ffcheck(read, "Cannot read recording");
      readBytes += pkt->size + 32;
      if (pkt->stream_index == vIndex) {
        const int64_t ptsUs = pkt->pts == AV_NOPTS_VALUE
            ? lastEndUs
            : av_rescale_q(pkt->pts, inVideo->time_base, kUs) - originUs;
        if (audioCodec)
          writeAudioUntil(std::max<int64_t>(0, ptsUs - kAudioLeadUs));
        av_packet_rescale_ts(pkt.get(), inVideo->time_base, outVideo->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts = std::max<int64_t>(0, pkt->pts - originTicks);
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= originTicks;
        pkt->stream_index = outVideo->index;
        pkt->pos = -1;
        ffcheck(av_interleaved_write_frame(out.ctx, pkt.get()),
                "Cannot write export (check free disk space)");
        lastVideoUs = ptsUs;
        lastEndUs = ptsUs + (pkt->duration > 0
            ? av_rescale_q(pkt->duration, inVideo->time_base, kUs) : 0);
      } else if (copiedAudio != nullptr && pkt->stream_index == singleAudioIndex) {
        av_packet_rescale_ts(pkt.get(), inCopiedAudio->time_base, copiedAudio->time_base);
        const int64_t shift = av_rescale_q(originUs, kUs, copiedAudio->time_base);
        if (pkt->pts != AV_NOPTS_VALUE) pkt->pts -= shift;
        if (pkt->dts != AV_NOPTS_VALUE) pkt->dts -= shift;
        pkt->stream_index = copiedAudio->index;
        pkt->pos = -1;
        ffcheck(av_interleaved_write_frame(out.ctx, pkt.get()),
                "Cannot write export (check free disk space)");
      } else if (AudioTrack *track = trackByStream.value(pkt->stream_index, nullptr)) {
        track->push(pkt.get());
        if (audioCodec)
          writeAudioUntil(std::max<int64_t>(0, lastVideoUs - kAudioLeadUs));
      }
      av_packet_unref(pkt.get());
      progress.store(std::min(0.99, double(readBytes) / double(totalBytes)));
    }

    if (audioCodec) {
      int64_t endUs = lastEndUs;
      for (auto &track : tracks) {
        track->flush();
        if (track->hasOutput())
          endUs = std::max(endUs, av_rescale_q(track->endSample(), kSamples, kUs));
      }
      // Drain the tail the video timeline did not reach.
      writeAudioUntil(std::max<int64_t>(0, endUs));
      out.writeAudio(audioCodec.get(), outAudio, nullptr);
    }
    ffcheck(av_write_trailer(out.ctx), "Cannot finalize export");
    avio_flush(out.ctx->pb);
    ffcheck(out.ctx->pb->error, "Cannot flush export");
    ffcheck(avio_closep(&out.ctx->pb), "Cannot close export output");
    cancelled(cancel);
    guard.verifyUnchanged(source);
    progress.store(1);
    return {publish(tmp, directory, source), {}};
  } catch (const std::exception &e) {
    return {{}, QString::fromUtf8(e.what())};
  }
}
} // namespace TrophyBackend
