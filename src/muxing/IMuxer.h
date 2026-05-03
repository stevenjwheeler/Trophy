/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <cstdint>
#include <vector>

namespace TrophyBackend {

struct EncodedPacket; // Forward declaration

/**
 * @brief Stream information for muxer setup
 */
struct StreamInfo {
  enum class Type { Video, Audio };

  Type type;
  int codecId;                    // FFmpeg codec ID
  std::vector<uint8_t> extraData; // Codec extradata (SPS/PPS, etc.)

  // Settings metadata
  int bitrate = 0;
  QString preset;

  // Video-specific
  int width = 0;
  int height = 0;
  int fps = 0;

  // Audio-specific
  int sampleRate = 0;
  int channels = 0;
  int bitsPerSample = 0;
};

/**
 * @brief Abstract interface for container muxers
 *
 * Handles writing encoded video and audio packets to container files.
 */
class IMuxer : public QObject {
  Q_OBJECT

public:
  explicit IMuxer(QObject *parent = nullptr) : QObject(parent) {}
  ~IMuxer() override = default;

  /**
   * @brief Initialize muxer with output file path
   * @param outputPath Path to output file
   * @return true on success
   */
  virtual bool initialize(const QString &outputPath) = 0;

  /**
   * @brief Add a stream to the muxer
   * @param info Stream information
   * @return Stream index, or -1 on error
   */
  virtual int addStream(const StreamInfo &info) = 0;

  /**
   * @brief Write container header (call after all streams added)
   * @return true on success
   */
  virtual bool writeHeader() = 0;

  /**
   * @brief Write an encoded packet
   * @param packet Encoded packet to write
   * @return true on success
   */
  virtual bool writePacket(const EncodedPacket &packet) = 0;

  /**
   * @brief Finalize and close the container
   * @return true on success
   */
  virtual bool finalize() = 0;

  /**
   * @brief Get output file path
   */
  virtual QString outputPath() const = 0;

  /**
   * @brief Get bytes written so far
   */
  virtual int64_t bytesWritten() const = 0;

  /**
   * @brief Get container format name
   */
  virtual QString formatName() const = 0;

signals:
  void muxingError(const QString &error);
};

} // namespace TrophyBackend
