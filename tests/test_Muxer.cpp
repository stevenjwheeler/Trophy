/*
 * Trophy - Unit Tests
 * FFmpegMuxer end-to-end: real x264 video + AAC audio packets into MKV,
 * then re-open the file and verify container integrity.
 */

#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>

#include <cmath>
#include <cerrno>
#include <atomic>
#include <future>
#include <thread>
#include <sys/stat.h>

#include "capture/IVideoCaptureSource.h"
#include "encoding/AudioEncoder.h"
#include "encoding/EncoderFactory.h"
#include "encoding/FFmpegEncoder.h"
#include "encoding/IEncoder.h"
#include "muxing/FFmpegMuxer.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
}

using namespace TrophyBackend;

class MuxerTestEnvironment : public ::testing::Environment {
public:
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char arg0[] = "test";
      static char *argv[] = {arg0, nullptr};
      new QCoreApplication(argc, argv);
    }
  }
};

static auto *env =
    testing::AddGlobalTestEnvironment(new MuxerTestEnvironment);

class MuxerTest : public ::testing::Test {
protected:
  QTemporaryDir tempDir;
  void SetUp() override { ASSERT_TRUE(tempDir.isValid()); }
  static void failOutputClose(FFmpegMuxer &muxer) {
    closeCalls = 0;
    muxer.m_closeOutput = [](AVIOContext **io) {
      ++closeCalls;
      const int result = avio_closep(io);
      return result < 0 ? result : AVERROR(EIO);
    };
  }
  static void restoreOutputClose(FFmpegMuxer &muxer) {
    muxer.m_closeOutput = &avio_closep;
  }
  inline static int closeCalls = 0;
  static std::unique_lock<std::mutex> lockIo(FFmpegMuxer &muxer) {
    return std::unique_lock<std::mutex>(muxer.m_muxerMutex);
  }
};

TEST_F(MuxerTest, ByteTelemetryDoesNotWaitForNativeIoLock) {
  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(tempDir.path() + "/telemetry.mkv"));
  StreamInfo audio;
  audio.type = StreamInfo::Type::Audio;
  audio.codecId = AV_CODEC_ID_PCM_S16LE;
  audio.sampleRate = 48000;
  audio.channels = 2;
  ASSERT_GE(muxer.addStream(audio), 0);
  ASSERT_TRUE(muxer.writeHeader());
  const auto before = muxer.bytesWritten();
  ASSERT_GT(before, 0);
  std::promise<void> held, release;
  auto heldSignal = held.get_future();
  auto releaseSignal = release.get_future();
  std::atomic<bool> released{false};
  std::thread writer([&] {
    auto lock = lockIo(muxer);
    held.set_value();
    // Deadlock guard only; success asserts lock ownership, not elapsed time.
    releaseSignal.wait_for(std::chrono::seconds(5));
    released.store(true);
  });
  heldSignal.wait();
  const auto snapshot = muxer.bytesWritten();
  const bool completedWhileLocked = !released.load();
  release.set_value();
  writer.join();
  EXPECT_TRUE(completedWhileLocked);
  EXPECT_EQ(snapshot, before);
}

TEST_F(MuxerTest, CloseFailureIsRetainedAndByteCountSurvivesClose) {
  FFmpegMuxer muxer;
  const QString path = tempDir.path() + "/close-failure.mkv";
  ASSERT_TRUE(muxer.initialize(path));
  StreamInfo audio;
  audio.type = StreamInfo::Type::Audio;
  audio.codecId = AV_CODEC_ID_PCM_S16LE;
  audio.sampleRate = 48000;
  audio.channels = 2;
  ASSERT_GE(muxer.addStream(audio), 0);
  ASSERT_TRUE(muxer.writeHeader());
  failOutputClose(muxer);
  EXPECT_FALSE(muxer.finalize());
  EXPECT_FALSE(muxer.finalize());
  EXPECT_EQ(closeCalls, 1);
  EXPECT_GT(muxer.bytesWritten(), 0);
  EXPECT_TRUE(QFileInfo::exists(path)); // Preserve partial output for recovery.
  ASSERT_TRUE(muxer.initialize(tempDir.path() + "/fresh.mkv"));
  EXPECT_EQ(muxer.bytesWritten(), 0);
  restoreOutputClose(muxer);
  ASSERT_GE(muxer.addStream(audio), 0);
  ASSERT_TRUE(muxer.writeHeader());
  EXPECT_TRUE(muxer.finalize());
  EXPECT_GT(muxer.bytesWritten(), 0);
}

static bool haveX264() {
  return avcodec_find_encoder_by_name("libx264") != nullptr;
}

TEST_F(MuxerTest, MkvRoundTripWithVideoAndAudio) {
  if (!haveX264())
    GTEST_SKIP() << "libx264 unavailable";
  if (!avcodec_find_encoder(AV_CODEC_ID_AAC))
    GTEST_SKIP() << "AAC encoder unavailable";

  const int W = 320, H = 240, FPS = 30;

  // --- video: tiny software encode ---
  FFmpegEncoder venc(IEncoder::EncoderType::Software_x264, "libx264");
  EncoderConfig vcfg;
  vcfg.width = W;
  vcfg.height = H;
  vcfg.fps = FPS;
  vcfg.bitrate = 500;
  vcfg.preset = "ultrafast";
  vcfg.inputPixelFormat = AV_PIX_FMT_BGRA;
  ASSERT_TRUE(venc.initialize(vcfg));

  // --- audio ---
  AudioEncoder aenc;
  AudioEncoderConfig acfg;
  acfg.sampleRate = 48000;
  acfg.channels = 2;
  acfg.bitrate = 128;
  acfg.codec = "aac";
  ASSERT_TRUE(aenc.initialize(acfg));

  // --- mux ---
  const QString outPath = tempDir.path() + "/roundtrip.mkv";
  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(outPath));

  StreamInfo vinfo;
  vinfo.type = StreamInfo::Type::Video;
  vinfo.codecId = venc.codecId();
  vinfo.extraData = venc.extraData();
  vinfo.pixelFormat = venc.encodedPixelFormat();
  vinfo.width = W;
  vinfo.height = H;
  vinfo.fps = FPS;
  vinfo.bitrate = 500;
  const int vStream = muxer.addStream(vinfo);
  ASSERT_GE(vStream, 0);

  StreamInfo ainfo;
  ainfo.type = StreamInfo::Type::Audio;
  ainfo.codecId = aenc.codecId();
  ainfo.extraData = aenc.extraData();
  ainfo.sampleRate = 48000;
  ainfo.channels = 2;
  ainfo.bitrate = 128;
  const int aStream = muxer.addStream(ainfo);
  ASSERT_GE(aStream, 0);

  QMap<QString, QString> meta;
  meta["title"] = "MuxerTest";
  EXPECT_TRUE(muxer.setMetadata(meta));
  ASSERT_TRUE(muxer.writeHeader());

  // Feed 60 video frames (simple gradient, BGRA). x264 buffers several
  // frames for lookahead/threads, so only a long-enough run produces
  // packets before flush.
  std::vector<uint8_t> bgra(W * H * 4);
  int videoPackets = 0;
  for (int f = 0; f < 60; ++f) {
    for (int y = 0; y < H; ++y)
      for (int x = 0; x < W; ++x) {
        uint8_t *p = &bgra[(y * W + x) * 4];
        p[0] = (uint8_t)((x + f * 10) & 0xFF);
        p[1] = (uint8_t)((y + f * 5) & 0xFF);
        p[2] = 128;
        p[3] = 255;
      }
    VideoFrame frame{bgra.data(), W, H, W * 4, (int64_t)f * 1000000 / FPS,
                     AV_PIX_FMT_BGRA};
    EncodedPacket pkt;
    if (venc.encodeFrame(frame, pkt)) {
      pkt.streamIndex = vStream;
      EXPECT_TRUE(muxer.writePacket(pkt));
      ++videoPackets;
    }
  }
  std::vector<EncodedPacket> vflushed;
  for (int n = venc.flush(vflushed), i = 0; i < n; ++i) {
    vflushed[i].streamIndex = vStream;
    EXPECT_TRUE(muxer.writePacket(vflushed[i]));
    ++videoPackets;
  }
  EXPECT_GT(videoPackets, 0);

  // Feed audio (sine, longer than one AAC frame).
  std::vector<float> sine(4096 * 2);
  for (size_t i = 0; i < sine.size(); ++i)
    sine[i] = 0.2f * std::sin(2.0f * 3.14159265f * 440.0f * i / 48000.0f);
  int audioPackets = 0;
  EncodedPacket apkt;
  for (int off = 0; off < 4096; off += 1024) {
    if (aenc.encodeAudio(sine.data() + off * 2, 1024, apkt)) {
      apkt.streamIndex = aStream;
      EXPECT_TRUE(muxer.writePacket(apkt));
      ++audioPackets;
    }
  }
  std::vector<EncodedPacket> aflushed;
  audioPackets += aenc.flush(aflushed);
  for (auto &p : aflushed) {
    p.streamIndex = aStream;
    EXPECT_TRUE(muxer.writePacket(p));
  }
  EXPECT_GT(audioPackets, 0);

  EXPECT_TRUE(muxer.finalize());
  EXPECT_GT(muxer.bytesWritten(), 0);

  // --- verify container integrity by re-opening ---
  AVFormatContext *ctx = nullptr;
  ASSERT_EQ(
      avformat_open_input(&ctx, outPath.toUtf8().constData(), nullptr, nullptr),
      0);
  ASSERT_EQ(avformat_find_stream_info(ctx, nullptr), 0);
  EXPECT_EQ(ctx->nb_streams, 2u);
  bool sawVideo = false, sawAudio = false;
  for (unsigned i = 0; i < ctx->nb_streams; ++i) {
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO) {
      sawVideo = true;
      EXPECT_EQ(ctx->streams[i]->codecpar->width, W);
      EXPECT_EQ(ctx->streams[i]->codecpar->height, H);
    }
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_AUDIO)
      sawAudio = true;
  }
  EXPECT_TRUE(sawVideo);
  EXPECT_TRUE(sawAudio);
  AVDictionaryEntry *title =
      av_dict_get(ctx->metadata, "title", nullptr, 0);
  ASSERT_NE(title, nullptr);
  EXPECT_STREQ(title->value, "MuxerTest");
  avformat_close_input(&ctx);
}

TEST_F(MuxerTest, Mp4ExtensionSelectsMp4Format) {  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(tempDir.path() + "/out.mp4"));
  EXPECT_EQ(muxer.formatName(), "mp4");
  EXPECT_EQ(muxer.outputPath(), tempDir.path() + "/out.mp4");
}

TEST_F(MuxerTest, WriteBeforeHeaderFails) {
  if (!haveX264())
    GTEST_SKIP() << "libx264 unavailable";

  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(tempDir.path() + "/early.mkv"));
  EncodedPacket pkt;
  pkt.pts = 0;
  pkt.dts = 0;
  pkt.streamIndex = 0;
  EXPECT_FALSE(muxer.writePacket(pkt));
}

// Regression: the NVENC/CUDA upload path once produced frames whose right
// chroma half was exactly zero (converting av_hwframe_transfer_data
// YUV420P->NV12 copies half the chroma on this stack). Encode color bars
// via NVENC when present, decode, and require real chroma everywhere.
TEST_F(MuxerTest, NvencFullChromaWhenAvailable) {
  if (avcodec_find_encoder_by_name("h264_nvenc") == nullptr)
    GTEST_SKIP() << "h264_nvenc unavailable";
  if (avcodec_find_decoder(AV_CODEC_ID_H264) == nullptr)
    GTEST_SKIP() << "h264 decoder unavailable";

  const int W = 640, H = 360;
  FFmpegEncoder venc(IEncoder::EncoderType::NVENC_H264, "h264_nvenc");
  EncoderConfig vcfg;
  vcfg.width = W;
  vcfg.height = H;
  vcfg.fps = 30;
  vcfg.bitrate = 2000;
  vcfg.preset = "p4";
  vcfg.inputPixelFormat = AV_PIX_FMT_BGRA;
  if (!venc.initialize(vcfg))
    GTEST_SKIP() << "NVENC init failed (no GPU?)";

  const QString outPath = tempDir.path() + "/nvenc.mkv";
  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(outPath));
  StreamInfo vinfo;
  vinfo.type = StreamInfo::Type::Video;
  vinfo.codecId = venc.codecId();
  vinfo.extraData = venc.extraData();
  vinfo.pixelFormat = venc.encodedPixelFormat();
  vinfo.width = W;
  vinfo.height = H;
  vinfo.fps = 30;
  vinfo.bitrate = 2000;
  const int vs = muxer.addStream(vinfo);
  ASSERT_GE(vs, 0);
  ASSERT_TRUE(muxer.writeHeader());

  std::vector<uint8_t> bgra(W * H * 4);
  for (int y = 0; y < H; ++y)
    for (int x = 0; x < W; ++x) {
      uint8_t *p = &bgra[(y * W + x) * 4];
      const int bar = x / (W / 8);
      static const uint8_t cols[8][3] = {{0, 0, 0},     {0, 0, 255},
                                         {0, 255, 0},   {0, 255, 255},
                                         {255, 0, 0},   {255, 0, 255},
                                         {255, 255, 0}, {255, 255, 255}};
      p[0] = cols[bar][0];
      p[1] = cols[bar][1];
      p[2] = cols[bar][2];
      p[3] = 255;
    }
  int produced = 0;
  for (int f = 0; f < 30; ++f) {
    VideoFrame frame{bgra.data(), W, H, W * 4, (int64_t)f * 33333,
                     AV_PIX_FMT_BGRA};
    EncodedPacket pkt;
    if (venc.encodeFrame(frame, pkt)) {
      pkt.streamIndex = vs;
      EXPECT_TRUE(muxer.writePacket(pkt));
      ++produced;
    }
  }
  std::vector<EncodedPacket> vf;
  produced += venc.flush(vf);
  for (auto &pkt : vf) {
    pkt.streamIndex = vs;
    EXPECT_TRUE(muxer.writePacket(pkt));
  }
  EXPECT_GT(produced, 0);
  EXPECT_TRUE(muxer.finalize());

  // Decode back and inspect chroma halves.
  AVFormatContext *ctx = nullptr;
  ASSERT_EQ(
      avformat_open_input(&ctx, outPath.toUtf8().constData(), nullptr, nullptr),
      0);
  ASSERT_EQ(avformat_find_stream_info(ctx, nullptr), 0);
  int vstream = -1;
  for (unsigned i = 0; i < ctx->nb_streams; ++i) {
    if (ctx->streams[i]->codecpar->codec_type == AVMEDIA_TYPE_VIDEO)
      vstream = (int)i;
  }
  ASSERT_GE(vstream, 0);
  const AVCodec *dec =
      avcodec_find_decoder(ctx->streams[vstream]->codecpar->codec_id);
  ASSERT_NE(dec, nullptr);
  AVCodecContext *dctx = avcodec_alloc_context3(dec);
  ASSERT_EQ(avcodec_parameters_to_context(dctx, ctx->streams[vstream]->codecpar),
            0);
  ASSERT_EQ(avcodec_open2(dctx, dec, nullptr), 0);

  AVPacket *ap = av_packet_alloc();
  AVFrame *fr = av_frame_alloc();
  bool gotFrame = false;
  double rStd = 0;
  while (!gotFrame && av_read_frame(ctx, ap) >= 0) {
    if (ap->stream_index != vstream) {
      av_packet_unref(ap);
      continue;
    }
    if (avcodec_send_packet(dctx, ap) < 0) {
      av_packet_unref(ap);
      continue;
    }
    av_packet_unref(ap);
    while (avcodec_receive_frame(dctx, fr) == 0) {
      // U plane is W/2 wide; require variance in the right half.
      const int uw = fr->width / 2, uh = fr->height / 2;
      double mean = 0;
      for (int y = 0; y < uh; y += 4)
        for (int x = uw / 2; x < uw; x += 4)
          mean += fr->data[1][y * fr->linesize[1] + x];
      mean /= ((uh / 4) * (uw / 4));
      double var = 0;
      for (int y = 0; y < uh; y += 4)
        for (int x = uw / 2; x < uw; x += 4) {
          const double d =
              fr->data[1][y * fr->linesize[1] + x] - mean;
          var += d * d;
        }
      rStd = std::sqrt(var / ((uh / 4) * (uw / 4)));
      gotFrame = true;
      break;
    }
  }
  av_frame_free(&fr);
  av_packet_free(&ap);
  avcodec_free_context(&dctx);
  avformat_close_input(&ctx);

  ASSERT_TRUE(gotFrame) << "no decodable frame in NVENC output";
  EXPECT_GT(rStd, 1.0) << "right-half chroma is flat: NVENC upload broken";
}

TEST_F(MuxerTest, AddStreamAfterHeaderRejected) {
  // Post-header addStream must fail loudly (-1), not corrupt the trailer:
  // DVR handoff builds successor legs ahead of time and a late stream
  // must never sneak into a sealed header. PCM needs no encoder.
  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(tempDir.path() + "/sealed.mkv"));
  StreamInfo v;
  v.type = StreamInfo::Type::Audio;
  v.codecId = AV_CODEC_ID_PCM_S16LE;
  v.sampleRate = 48000;
  v.channels = 2;
  v.bitrate = 1536;
  ASSERT_GE(muxer.addStream(v), 0);
  ASSERT_TRUE(muxer.writeHeader());
  StreamInfo late = v;
  EXPECT_EQ(muxer.addStream(late), -1);
  EXPECT_TRUE(muxer.finalize());
}

TEST_F(MuxerTest, DoubleFinalizeIsSafe) {
  // Teardown races (DVR rotation + game-close stop) can finalize twice:
  // the second call must be a no-op success, never a second trailer.
  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(tempDir.path() + "/twice.mkv"));
  StreamInfo v;
  v.type = StreamInfo::Type::Audio;
  v.codecId = AV_CODEC_ID_PCM_S16LE;
  v.sampleRate = 48000;
  v.channels = 2;
  v.bitrate = 1536;
  ASSERT_GE(muxer.addStream(v), 0);
  ASSERT_TRUE(muxer.writeHeader());
  EXPECT_TRUE(muxer.finalize());
  EXPECT_TRUE(muxer.finalize());
}

TEST_F(MuxerTest, ReinitAfterPartialFailureSucceeds) {
  // initialize() is cleanup-first: abandoning a half-built muxer (portal
  // failure mid-start) and reusing the object must yield a working file,
  // not state leaked from the first attempt.
  FFmpegMuxer muxer;
  ASSERT_TRUE(muxer.initialize(tempDir.path() + "/abandoned.mkv"));
  EncodedPacket pkt; // pre-header write fails; object now "used"
  pkt.streamIndex = 0;
  EXPECT_FALSE(muxer.writePacket(pkt));

  const QString out = tempDir.path() + "/reused.mkv";
  ASSERT_TRUE(muxer.initialize(out));
  StreamInfo v;
  v.type = StreamInfo::Type::Audio;
  v.codecId = AV_CODEC_ID_PCM_S16LE;
  v.sampleRate = 48000;
  v.channels = 2;
  v.bitrate = 1536;
  ASSERT_GE(muxer.addStream(v), 0);
  ASSERT_TRUE(muxer.writeHeader());
  EXPECT_TRUE(muxer.finalize());
  EXPECT_TRUE(QFileInfo(out).size() > 0);
}

TEST_F(MuxerTest, BytesWrittenTracksHeaderAndPackets) {
  // Live size readout regression: the muxer byte counter (AVIO position)
  // must grow with the header and every packet, without needing real
  // encoder output. RecordingManager::recordingSize prefers this over
  // stat() while recording because disk lags behind buffering.
  FFmpegMuxer muxer;
  const QString out = tempDir.path() + "/counter.mkv";
  ASSERT_TRUE(muxer.initialize(out));
  EXPECT_EQ(muxer.bytesWritten(), 0);

  StreamInfo v;
  v.type = StreamInfo::Type::Audio;
  v.codecId = AV_CODEC_ID_PCM_S16LE; // no extradata needed for header
  v.sampleRate = 48000;
  v.channels = 2;
  v.bitrate = 1536;
  ASSERT_GE(muxer.addStream(v), 0);
  ASSERT_TRUE(muxer.writeHeader());

  const int64_t afterHeader = muxer.bytesWritten();
  EXPECT_GT(afterHeader, 0);

  EncodedPacket pkt;
  pkt.streamIndex = 0;
  pkt.data = std::vector<uint8_t>(4096, 0x42);
  pkt.duration = 20;
  pkt.isKeyframe = false;
  // One small packet stays in the matroska cluster buffer (by design),
  // so span several 1s clusters to force flushes and counter growth.
  for (int i = 0; i < 300; ++i) {
    pkt.pts = i * 20;
    pkt.dts = i * 20;
    ASSERT_TRUE(muxer.writePacket(pkt));
  }
  EXPECT_GT(muxer.bytesWritten(), afterHeader);

  // The counter includes AVIO-buffered bytes, so it must never read
  // *below* the on-disk size (the old stat()-based readout's failure).
  EXPECT_GE(muxer.bytesWritten(), QFileInfo(out).size());
  EXPECT_TRUE(muxer.finalize());
}

TEST_F(MuxerTest, NativeEnospcRemainsFailureAndReinitializeRecovers) {
  // /dev/full is a character device whose writes return real ENOSPC.
  // No custom AVIO or injected FFmpeg result is involved, and no file is
  // created/truncated outside this fixture's temporary directory.
  struct stat device {};
  if (stat("/dev/full", &device) != 0 || !S_ISCHR(device.st_mode))
    GTEST_SKIP() << "/dev/full unavailable";
  FFmpegMuxer muxer;
  StreamInfo audio;
  audio.type = StreamInfo::Type::Audio;
  audio.codecId = AV_CODEC_ID_PCM_S16LE;
  audio.sampleRate = 48000;
  audio.channels = 2;
  ASSERT_TRUE(muxer.initialize("/dev/full"));
  ASSERT_EQ(muxer.addStream(audio), 0);
  // Some FFmpeg versions flush the header immediately, others buffer it.
  const bool header = muxer.writeHeader();
  EncodedPacket packet;
  packet.streamIndex = 0;
  packet.pts = packet.dts = 0;
  packet.duration = 20;
  packet.data.assign(4096, 0);
  if (header)
    muxer.writePacket(packet);
  EXPECT_FALSE(muxer.finalize());
  EXPECT_FALSE(muxer.finalize()) << "an earlier I/O failure must stay failed";
  EXPECT_FALSE(muxer.writePacket(packet));
  EXPECT_FALSE(muxer.writeHeader());

  const QString recovered = tempDir.path() + "/recovered.mkv";
  ASSERT_TRUE(muxer.initialize(recovered));
  ASSERT_EQ(muxer.addStream(audio), 0);
  ASSERT_TRUE(muxer.writeHeader());
  ASSERT_TRUE(muxer.writePacket(packet));
  ASSERT_TRUE(muxer.finalize());
  ASSERT_TRUE(muxer.finalize());
  EXPECT_FALSE(muxer.writePacket(packet));
  EXPECT_FALSE(muxer.writeHeader());
  AVFormatContext *ctx = nullptr;
  ASSERT_EQ(avformat_open_input(&ctx, recovered.toUtf8().constData(), nullptr,
                               nullptr), 0);
  EXPECT_EQ(avformat_find_stream_info(ctx, nullptr), 0);
  EXPECT_EQ(ctx->nb_streams, 1u);
  avformat_close_input(&ctx);
}
