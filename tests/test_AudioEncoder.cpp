/*
 * Trophy - Unit Tests
 * AudioEncoder: AAC/Opus init, packet production, flush, NaN safety.
 */

#include <QCoreApplication>
#include <cmath>
#include <gtest/gtest.h>

#include "encoding/AudioEncoder.h"
#include "encoding/IEncoder.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

using namespace TrophyBackend;

class AudioEncoderTestEnvironment : public ::testing::Environment {
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
    testing::AddGlobalTestEnvironment(new AudioEncoderTestEnvironment);

// Feed a sine sweep; returns total packets produced (encode + flush).
static int encodeSine(AudioEncoder &enc, int totalFrames, float freq = 440.0f) {
  const int ch = 2, rate = 48000;
  std::vector<float> buf(1024 * ch);
  EncodedPacket pkt;
  int produced = 0;
  int fed = 0;
  int batch = 0;
  while (fed < totalFrames) {
    int n = std::min(1024, totalFrames - fed);
    for (int i = 0; i < n; ++i)
      for (int c = 0; c < ch; ++c)
        buf[i * ch + c] =
            0.3f * std::sin(2.0f * 3.14159265f * freq * (fed + i) / rate);
    if (enc.encodeAudio(buf.data(), n, pkt))
      ++produced;
    fed += n;
    if (++batch > 64)
      break; // safety: encoder must produce within a few frames
  }
  std::vector<EncodedPacket> flushed;
  produced += enc.flush(flushed);
  return produced;
}

TEST(AudioEncoderTest, AacInitializesAndProducesPackets) {
  if (!avcodec_find_encoder(AV_CODEC_ID_AAC))
    GTEST_SKIP() << "FFmpeg AAC encoder unavailable";

  AudioEncoder enc;
  AudioEncoderConfig cfg;
  cfg.sampleRate = 48000;
  cfg.channels = 2;
  cfg.bitrate = 128;
  cfg.codec = "aac";
  ASSERT_TRUE(enc.initialize(cfg));
  EXPECT_EQ(enc.codecId(), (int)AV_CODEC_ID_AAC);

  EXPECT_GT(encodeSine(enc, 8192), 0);
}

TEST(AudioEncoderTest, OpusInitializesWhenAvailable) {
  if (!avcodec_find_encoder(AV_CODEC_ID_OPUS))
    GTEST_SKIP() << "FFmpeg Opus encoder unavailable";

  AudioEncoder enc;
  AudioEncoderConfig cfg;
  cfg.codec = "opus";
  ASSERT_TRUE(enc.initialize(cfg));
  EXPECT_GT(encodeSine(enc, 960 * 8, 220.0f), 0);
}

TEST(AudioEncoderTest, UnknownCodecFailsInit) {
  AudioEncoder enc;
  AudioEncoderConfig cfg;
  cfg.codec = "not-a-codec";
  EXPECT_FALSE(enc.initialize(cfg));
}

TEST(AudioEncoderTest, StartOffsetShiftsFirstTimestampOnce) {
  if (!avcodec_find_encoder(AV_CODEC_ID_AAC))
    GTEST_SKIP() << "FFmpeg AAC encoder unavailable";

  AudioEncoder enc;
  AudioEncoderConfig cfg;
  cfg.sampleRate = 48000;
  cfg.channels = 2;
  cfg.bitrate = 128;
  cfg.codec = "aac";
  ASSERT_TRUE(enc.initialize(cfg));

  // 4800 samples @48kHz = 100ms: a track starting 100ms after video.
  enc.setStartOffsetSamples(4800);
  enc.setStartOffsetSamples(9600); // second call ignored (one-shot)

  std::vector<float> buf(1024 * 2, 0.0f);
  EncodedPacket pkt;
  bool produced = false;
  for (int i = 0; i < 8 && !produced; ++i)
    produced = enc.encodeAudio(buf.data(), 1024, pkt);
  ASSERT_TRUE(produced);
  // 100ms shift minus AAC's inherent one-frame priming delay
  // (1024 samples ≈ 21ms): proves the offset landed on the packet.
  EXPECT_GE(pkt.pts, 79);
  EXPECT_LE(pkt.pts, 100);
}

TEST(AudioEncoderTest, NanInputDoesNotCrash) {
  if (!avcodec_find_encoder(AV_CODEC_ID_AAC))
    GTEST_SKIP() << "FFmpeg AAC encoder unavailable";

  AudioEncoder enc;
  AudioEncoderConfig cfg;
  ASSERT_TRUE(enc.initialize(cfg));

  std::vector<float> nanBuf(1024 * 2, std::nanf(""));
  EncodedPacket pkt;
  // Must not crash; return value only signals packet production.
  for (int i = 0; i < 4; ++i)
    enc.encodeAudio(nanBuf.data(), 1024, pkt);
  std::vector<EncodedPacket> flushed;
  enc.flush(flushed);
  SUCCEED();
}

TEST(AudioEncoderTest, PtsNonDecreasingAfterPriming) {
  if (!avcodec_find_encoder(AV_CODEC_ID_AAC))
    GTEST_SKIP() << "FFmpeg AAC encoder unavailable";

  AudioEncoder enc;
  AudioEncoderConfig cfg;
  ASSERT_TRUE(enc.initialize(cfg));

  std::vector<float> buf(1024 * 2, 0.1f);
  EncodedPacket pkt;
  // NOTE: the first packet may carry negative pts from encoder priming
  // samples; monotonicity only holds from the second packet on.
  bool haveFirst = false;
  int64_t lastPts = 0;
  int packets = 0;
  for (int i = 0; i < 12; ++i) {
    if (enc.encodeAudio(buf.data(), 1024, pkt)) {
      if (haveFirst)
        EXPECT_GE(pkt.pts, lastPts);
      haveFirst = true;
      lastPts = pkt.pts;
      ++packets;
    }
  }
  EXPECT_GT(packets, 0);
}
