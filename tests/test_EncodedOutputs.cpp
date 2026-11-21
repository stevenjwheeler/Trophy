/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "core/ReplayManager.h"
#include "core/RecordingManager.h"
#include "encoding/FFmpegEncoder.h"
#include "encoding/AudioEncoder.h"
#include "capture/IVideoCaptureSource.h"
#include "core/StorageManager.h"
#include "settings/SettingsManager.h"
#include "audio/AudioRoutingInfo.h"
#include "library/RecordingAnnotations.h"
#include <QSize>
#include <QFile>
#include <QSignalSpy>
#include <QTest>
#include <algorithm>
#include <cstring>
#include <QCoreApplication>
#include <QFileInfo>
#include <QTemporaryDir>
#include <gtest/gtest.h>
extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/pixfmt.h>
}
using namespace TrophyBackend;

namespace {
thread_local int missingReferences = 0;
void checkReferences(void *context, int level, const char *format, va_list args) {
  if (std::strstr(format, "co located POCs unavailable")) ++missingReferences;
  av_log_default_callback(context, level, format, args);
}
struct DecodeLogScope {
  DecodeLogScope() { missingReferences = 0; av_log_set_callback(checkReferences); }
  ~DecodeLogScope() { av_log_set_callback(av_log_default_callback); }
};
}

namespace TrophyBackend {
class EncodedOutputsTest : public ::testing::Test {
protected:
  using DvrPacketBuffer = ReplayManager::Buffer;
  using RecordingFileOutput = RecordingManager::FileOutput;
  QTemporaryDir directory;
  QByteArray oldConfig;
  EncodedStream stream;
  std::vector<EncodedFrame> packets;
  void SetUp() override {
    if (!QCoreApplication::instance()) {
      static int argc = 1;
      static char name[] = "encoded-outputs";
      static char *argv[] = {name, nullptr};
      new QCoreApplication(argc, argv);
    }
    ASSERT_TRUE(directory.isValid());
    oldConfig = qgetenv("XDG_CONFIG_HOME");
    qputenv("XDG_CONFIG_HOME", directory.path().toUtf8());
    if (!avcodec_find_encoder_by_name("libx264")) GTEST_SKIP() << "libx264 unavailable";
    encode(160, 96, 120);
  }
  void TearDown() override {
    if (oldConfig.isNull()) qunsetenv("XDG_CONFIG_HOME");
    else qputenv("XDG_CONFIG_HOME", oldConfig);
  }
  void encode(int width, int height, int frames, const QString &preset = "ultrafast") {
    packets.clear();
    FFmpegEncoder encoder(IEncoder::EncoderType::Software_x264, "libx264");
    EncoderConfig config;
    config.width = width;
    config.height = height;
    config.fps = 30;
    config.bitrate = 500;
    config.preset = preset;
    config.keyframeInterval = 1;
    config.inputPixelFormat = AV_PIX_FMT_BGRA;
    ASSERT_TRUE(encoder.initialize(config));
    StreamInfo video{};
    video.type = StreamInfo::Type::Video;
    video.codecId = encoder.codecId();
    video.extraData = encoder.extraData();
    video.pixelFormat = encoder.encodedPixelFormat();
    video.width = width;
    video.height = height;
    video.fps = 30;
    stream.generation++;
    stream.game = QStringLiteral("Packet game");
    stream.streams = {video};
    std::vector<uint8_t> pixels(width * height * 4, 128);
    for (int i = 0; i < frames; ++i) {
      VideoFrame frame{pixels.data(), width, height, width * 4,
                       int64_t(i) * 1000000 / 30, AV_PIX_FMT_BGRA};
      EncodedPacket packet;
      if (encoder.encodeFrame(frame, packet)) {
        packet.streamIndex = 0;
        packets.push_back(std::make_shared<const EncodedPacket>(std::move(packet)));
      }
    }
    std::vector<EncodedPacket> flushed;
    encoder.flush(flushed);
    for (auto &packet : flushed) {
      packet.streamIndex = 0;
      packets.push_back(std::make_shared<const EncodedPacket>(std::move(packet)));
    }
    ASSERT_FALSE(packets.empty());
  }
  int countFrames(const QString &path, QSize *size = nullptr) {
    AVFormatContext *context = nullptr;
    if (avformat_open_input(&context, path.toUtf8().constData(), nullptr, nullptr) < 0) return -1;
    if (avformat_find_stream_info(context, nullptr) < 0) { avformat_close_input(&context); return -1; }
    if (size) *size = {context->streams[0]->codecpar->width, context->streams[0]->codecpar->height};
    auto *packet = av_packet_alloc();
    int count = 0;
    while (av_read_frame(context, packet) >= 0) {
      if (packet->stream_index == 0) ++count;
      av_packet_unref(packet);
    }
    av_packet_free(&packet);
    avformat_close_input(&context);
    return count;
  }
  int decodeFrames(const QString &path) {
    DecodeLogScope logs;
    AVFormatContext *input = nullptr;
    if (avformat_open_input(&input, path.toUtf8().constData(), nullptr, nullptr) < 0) return -1;
    AVCodecContext *decoder = nullptr;
    AVFrame *frame = av_frame_alloc();
    AVPacket *packet = av_packet_alloc();
    bool ok = frame && packet && avformat_find_stream_info(input, nullptr) >= 0;
    if (ok) {
      const auto *codec = avcodec_find_decoder(input->streams[0]->codecpar->codec_id);
      decoder = codec ? avcodec_alloc_context3(codec) : nullptr;
      ok = decoder && avcodec_parameters_to_context(decoder, input->streams[0]->codecpar) >= 0;
      if (ok) {
        decoder->thread_count = 1;
        decoder->err_recognition = AV_EF_EXPLODE;
        ok = avcodec_open2(decoder, codec, nullptr) >= 0;
      }
    }
    int frames = 0;
    const auto drain = [&] {
      int result;
      while ((result = avcodec_receive_frame(decoder, frame)) >= 0) {
        if (frame->decode_error_flags || (frame->flags & AV_FRAME_FLAG_CORRUPT)) return false;
        ++frames;
        av_frame_unref(frame);
      }
      return result == AVERROR(EAGAIN) || result == AVERROR_EOF;
    };
    int result = 0;
    while (ok && (result = av_read_frame(input, packet)) >= 0) {
      if (packet->stream_index == 0)
        ok = avcodec_send_packet(decoder, packet) >= 0 && drain();
      av_packet_unref(packet);
    }
    ok = ok && result == AVERROR_EOF && avcodec_send_packet(decoder, nullptr) >= 0 && drain();
    av_packet_free(&packet);
    av_frame_free(&frame);
    avcodec_free_context(&decoder);
    avformat_close_input(&input);
    return ok && !missingReferences ? frames : -1;
  }
  void seedReplay(ReplayManager &replay, const QJsonObject &history = {}) {
    replay.m_buffer = std::make_shared<DvrPacketBuffer>(directory.path(), 30);
    replay.m_game = stream.game;
    ASSERT_TRUE(replay.m_buffer->begin(stream));
    for (const auto &packet : packets) ASSERT_TRUE(replay.m_buffer->write(packet));
    ASSERT_TRUE(replay.m_buffer->end());
    replay.m_buffer->setAudioHistory(stream.generation, history);
  }
  std::shared_ptr<RecordingFileOutput> recordingOutput(RecordingManager &manager) {
    return manager.m_output;
  }
  void addInterleavedAudio() {
    AudioEncoder encoder;
    AudioEncoderConfig config;
    config.sampleRate = 48000;
    config.channels = 2;
    config.bitrate = 128;
    ASSERT_TRUE(encoder.initialize(config));
    StreamInfo audio{};
    audio.type = StreamInfo::Type::Audio;
    audio.codecId = encoder.codecId();
    audio.extraData = encoder.extraData();
    audio.sampleRate = 48000;
    audio.channels = 2;
    stream.streams.push_back(audio);
    std::vector<float> samples(1024 * 2, 0.1f);
    for (int at = 0; at < 48000 * 4; at += 1024) {
      EncodedPacket packet;
      if (encoder.encodeAudio(samples.data(), 1024, packet)) {
        packet.streamIndex = 1;
        packets.push_back(std::make_shared<const EncodedPacket>(std::move(packet)));
      }
    }
    std::vector<EncodedPacket> flushed;
    encoder.flush(flushed);
    for (auto &packet : flushed) {
      packet.streamIndex = 1;
      packets.push_back(std::make_shared<const EncodedPacket>(std::move(packet)));
    }
    std::stable_sort(packets.begin(), packets.end(), [](const auto &a, const auto &b) {
      return a->dts < b->dts;
    });
  }
};
} // namespace TrophyBackend
TEST_F(EncodedOutputsTest, FileOutputWaitsForKeyframeAndRebasesLateAttachment) {
  RecordingFileOutput output(directory.path());
  ASSERT_TRUE(output.begin(stream));
  int selected = 0;
  for (const auto &packet : packets) {
    if (packet->pts < 1100) continue;
    ASSERT_TRUE(output.write(packet));
    if (output.started()) ++selected;
  }
  ASSERT_TRUE(output.end());
  const auto parts = output.takeParts();
  ASSERT_EQ(parts.size(), 1);
  EXPECT_TRUE(parts[0].succeeded);
  EXPECT_GT(parts[0].durationMs, 1000);
  EXPECT_LT(parts[0].durationMs, 3000);
  EXPECT_EQ(countFrames(parts[0].path), selected);
}
TEST_F(EncodedOutputsTest, FileOutputKeepsNativeGenerationSizesAndCompletedHistory) {
  RecordingFileOutput output(directory.path());
  ASSERT_TRUE(output.begin(stream));
  for (const auto &packet : packets) ASSERT_TRUE(output.write(packet));
  ASSERT_TRUE(output.end());
  const QJsonObject history{{"test", true}};
  output.setAudioHistory(stream.generation, history);
  auto first = output.takeParts();
  ASSERT_EQ(first.size(), 1);
  EXPECT_EQ(first[0].history, history);
  encode(320, 180, 60);
  ASSERT_TRUE(output.begin(stream));
  for (const auto &packet : packets) ASSERT_TRUE(output.write(packet));
  ASSERT_TRUE(output.end());
  const auto second = output.takeParts();
  ASSERT_EQ(second.size(), 1);
  EXPECT_NE(first[0].path, second[0].path);
  QSize size;
  EXPECT_GT(countFrames(first[0].path, &size), 0);
  EXPECT_EQ(size, QSize(160, 96));
  EXPECT_GT(countFrames(second[0].path, &size), 0);
  EXPECT_EQ(size, QSize(320, 180));
}
TEST_F(EncodedOutputsTest, SnapshotFreezesOpenGopAndSurvivesBufferDestruction) {
  DvrPacketBuffer::Snapshot snapshot;
  {
    DvrPacketBuffer buffer(directory.path(), 2);
    ASSERT_TRUE(buffer.begin(stream));
    for (const auto &packet : packets)
      if (packet->pts < 1800) ASSERT_TRUE(buffer.write(packet));
    snapshot = buffer.snapshot(2);
    ASSERT_FALSE(snapshot.pieces.isEmpty());
    ASSERT_FALSE(snapshot.pieces.last().packets.empty());
    const auto last = snapshot.pieces.last().packets.back()->pts;
    for (const auto &packet : packets)
      if (packet->pts >= 1800) ASSERT_TRUE(buffer.write(packet));
    ASSERT_TRUE(buffer.end());
    EXPECT_EQ(snapshot.pieces.last().packets.back()->pts, last);
    EXPECT_LT(snapshot.endMs, 1900);
    buffer.clear();
  }
  QString error;
  double seconds = 0;
  const auto path = directory.filePath("snapshot.mkv");
  ASSERT_TRUE(snapshot.save(path, &error, &seconds)) << error.toStdString();
  EXPECT_GT(countFrames(path), 40);
  EXPECT_LT(seconds, 2);
}
TEST_F(EncodedOutputsTest, MixedNativeGenerationsNormalizeOnlyDuringExport) {
  DvrPacketBuffer buffer(directory.path(), 10);
  ASSERT_TRUE(buffer.begin(stream));
  for (const auto &packet : packets) ASSERT_TRUE(buffer.write(packet));
  ASSERT_TRUE(buffer.end());
  encode(320, 180, 60);
  stream.timelineStartMs = 4000;
  ASSERT_TRUE(buffer.begin(stream));
  for (const auto &packet : packets) ASSERT_TRUE(buffer.write(packet));
  ASSERT_TRUE(buffer.end());
  const auto snapshot = buffer.snapshot(10);
  ASSERT_FALSE(snapshot.pieces.isEmpty());
  QSize firstSize;
  EXPECT_GT(countFrames(snapshot.pieces.first().block->path, &firstSize), 0);
  EXPECT_EQ(firstSize, QSize(160, 96));
  QString error;
  double seconds = 0;
  const auto path = directory.filePath("mixed.mkv");
  ASSERT_TRUE(snapshot.save(path, &error, &seconds)) << error.toStdString();
  QSize exported;
  EXPECT_GT(countFrames(path, &exported), 150);
  EXPECT_EQ(exported, QSize(320, 180));
  EXPECT_GT(countFrames(snapshot.pieces.first().block->path, &firstSize), 0);
  EXPECT_EQ(firstSize, QSize(160, 96));
}
TEST_F(EncodedOutputsTest, PruningReleasesOnlyUnreservedPacketFiles) {
  DvrPacketBuffer buffer(directory.path(), 1);
  ASSERT_TRUE(buffer.begin(stream));
  for (const auto &packet : packets)
    if (packet->pts < 1800) ASSERT_TRUE(buffer.write(packet));
  auto snapshot = buffer.snapshot(1);
  ASSERT_FALSE(snapshot.pieces.isEmpty());
  const auto reserved = snapshot.pieces.first().block->path;
  ASSERT_TRUE(QFileInfo::exists(reserved));
  for (const auto &packet : packets)
    if (packet->pts >= 1800) ASSERT_TRUE(buffer.write(packet));
  ASSERT_TRUE(buffer.end());
  EXPECT_LE(buffer.health()["retainedVideoSeconds"].toDouble(), 2.1);
  buffer.clear();
  EXPECT_TRUE(QFileInfo::exists(reserved));
  snapshot = {};
  EXPECT_FALSE(QFileInfo::exists(reserved));
}
TEST_F(EncodedOutputsTest, ReplayPublishesHistoryBeforeSavedNotification) {
  SettingsManager settings;
  settings.setRecordingsPath(directory.path());
  settings.setClipBufferSeconds(30);
  ReplayManager replay;
  replay.setSettingsManager(&settings);
  AudioRoutingInfo history;
  history.known = history.system = true;
  history.primary = 0;
  seedReplay(replay, history.toJson());
  bool annotatedAtSignal = false;
  QObject::connect(&replay, &ReplayManager::replaySaved,
      [&annotatedAtSignal, history](const QString &path, double) {
    annotatedAtSignal = RecordingAnnotations::audioRouting(path) == history.toJson();
  });
  QSignalSpy saved(&replay, &ReplayManager::replaySaved);
  QSignalSpy failed(&replay, &ReplayManager::replayFailed);
  replay.saveReplay();
  ASSERT_TRUE(QTest::qWaitFor([&] { return !saved.isEmpty() || !failed.isEmpty(); }));
  ASSERT_TRUE(failed.isEmpty());
  ASSERT_EQ(saved.size(), 1);
  EXPECT_TRUE(annotatedAtSignal);
  EXPECT_EQ(countFrames(saved[0][0].toString()), int(packets.size()));
}
TEST_F(EncodedOutputsTest, StorageFullRefusesReplayWithoutDeletingBufferedPackets) {
  SettingsManager settings;
  settings.setRecordingsPath(directory.path());
  QFile existing(directory.filePath("existing.mkv"));
  ASSERT_TRUE(existing.open(QIODevice::WriteOnly));
  ASSERT_EQ(existing.write(QByteArray(8192, 'x')), 8192);
  existing.close();
  StorageManager storage(directory.path());
  storage.setMaxSize(1);
  storage.refresh();
  ASSERT_TRUE(storage.isOverLimit());
  ReplayManager replay;
  replay.setSettingsManager(&settings);
  replay.setStorageManager(&storage);
  seedReplay(replay);
  QSignalSpy failed(&replay, &ReplayManager::replayFailed);
  replay.saveReplay();
  ASSERT_EQ(failed.size(), 1);
  EXPECT_TRUE(failed[0][0].toString().contains("Storage is full"));
  EXPECT_GT(replay.captureHealth()["bufferSeconds"].toDouble(), 0);
  EXPECT_TRUE(QFileInfo::exists(existing.fileName()));
}
TEST_F(EncodedOutputsTest, SingleGopCacheFilesWithReorderedFramesRemainClippable) {
  encode(160, 96, 120, "medium");
  DvrPacketBuffer buffer(directory.path(), 30);
  ASSERT_TRUE(buffer.begin(stream));
  DvrPacketBuffer::Snapshot liveSnapshot;
  ASSERT_GT(packets.size(), 66u);
  for (size_t i = 0; i < packets.size(); ++i) {
    ASSERT_TRUE(buffer.write(packets[i]));
    if (i == 65) liveSnapshot = buffer.snapshot(30);
  }
  ASSERT_TRUE(buffer.end());
  const auto snapshot = buffer.snapshot(30);
  ASSERT_GE(snapshot.pieces.size(), 3);
  QString error;
  double seconds = 0;
  const auto path = directory.filePath("reordered-gops.mkv");
  ASSERT_TRUE(snapshot.save(path, &error, &seconds)) << error.toStdString();
  EXPECT_EQ(countFrames(path), int(packets.size()));
  EXPECT_EQ(decodeFrames(path), int(packets.size()));
  EXPECT_GT(seconds, 3.5);
  const auto livePath = directory.filePath("reordered-live-gop.mkv");
  ASSERT_TRUE(liveSnapshot.save(livePath, &error, &seconds)) << error.toStdString();
  EXPECT_EQ(countFrames(livePath), 66);
  EXPECT_EQ(decodeFrames(livePath), 66);
}
TEST_F(EncodedOutputsTest, ReorderedVideoWithLeadingAudioRetainsItsReferenceTail) {
  if (!avcodec_find_encoder(AV_CODEC_ID_AAC)) GTEST_SKIP() << "AAC unavailable";
  encode(160, 96, 120, "medium");
  addInterleavedAudio();
  DvrPacketBuffer buffer(directory.path(), 30);
  ASSERT_TRUE(buffer.begin(stream));
  QList<std::pair<int, DvrPacketBuffer::Snapshot>> live;
  int videos = 0;
  for (const auto &packet : packets) {
    ASSERT_TRUE(buffer.write(packet));
    if (packet->streamIndex == 0) {
      ++videos;
      if (videos == 4 || videos == 8 || videos == 15 || videos == 33 || videos == 65 || videos == 90)
        live.append({videos, buffer.snapshot(30)});
    }
  }
  ASSERT_TRUE(buffer.end());
  const auto snapshot = buffer.snapshot(30);
  QString error;
  double seconds = 0;
  const auto path = directory.filePath("reordered-audio-tail.mkv");
  ASSERT_TRUE(snapshot.save(path, &error, &seconds)) << error.toStdString();
  EXPECT_EQ(countFrames(path), 120);
  EXPECT_EQ(decodeFrames(path), 120);
  for (const auto &[expected, saved] : live) {
    SCOPED_TRACE(expected);
    const auto path = directory.filePath(QString("audio-tail-%1.mkv").arg(expected));
    ASSERT_TRUE(saved.save(path, &error, &seconds)) << error.toStdString();
    EXPECT_EQ(countFrames(path), expected);
    EXPECT_EQ(decodeFrames(path), expected);
  }
}
TEST_F(EncodedOutputsTest, RecordingPathIsAnnouncedBeforeTheFileCanBeCreated) {
  SettingsManager settings;
  settings.setRecordingsPath(directory.path());
  StorageManager storage(directory.path());
  RecordingManager recording;
  recording.setSettingsManager(&settings);
  recording.setStorageManager(&storage);
  QSignalSpy prepared(&recording, &RecordingManager::recordingPathPrepared);
  recording.startRecording(stream.game);
  ASSERT_EQ(prepared.size(), 1);
  const auto path = prepared[0][0].toString();
  EXPECT_FALSE(QFileInfo::exists(path));
  auto output = recordingOutput(recording);
  ASSERT_TRUE(output);
  ASSERT_TRUE(output->begin(stream));
  for (const auto &packet : packets) ASSERT_TRUE(output->write(packet));
  EXPECT_EQ(output->path(), path);
  ASSERT_TRUE(output->end());
  EXPECT_EQ(countFrames(path), int(packets.size()));
  recording.stopRecording(); // Cancel the queued native start: no portal is used.
  QCoreApplication::processEvents();
}
TEST_F(EncodedOutputsTest, SnapshotRejectsIncompleteOrUnavailableProducerDecodeClocks) {
  DvrPacketBuffer buffer(directory.path(), 30);
  ASSERT_TRUE(buffer.begin(stream));
  for (const auto &packet : packets) ASSERT_TRUE(buffer.write(packet));
  ASSERT_TRUE(buffer.end());
  const auto snapshot = buffer.snapshot(30);
  ASSERT_FALSE(snapshot.pieces.isEmpty());
  ASSERT_FALSE(snapshot.pieces[0].videoDtsMs.isEmpty());
  for (int mode = 0; mode < 3; ++mode) {
    SCOPED_TRACE(mode);
    auto invalid = snapshot;
    auto &clock = invalid.pieces[0].videoDtsMs;
    if (mode == 0) clock.removeLast();
    else if (mode == 1) clock.append(clock.back() + 1);
    else clock[0] = AV_NOPTS_VALUE;
    QString error;
    double seconds = 0;
    const auto path = directory.filePath(QString("bad-clock-%1.mkv").arg(mode));
    EXPECT_FALSE(invalid.save(path, &error, &seconds));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_FALSE(QFileInfo::exists(path));
  }
}
