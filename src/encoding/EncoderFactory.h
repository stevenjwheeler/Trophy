/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "IEncoder.h"
#include <QString>
#include <memory>
#include <vector>

namespace TrophyBackend {

/**
 * @brief Information about an available encoder
 */
struct EncoderInfo {
  IEncoder::EncoderType type;
  QString name;
  QString description;
  bool isHardware;
  int priority; // Lower = preferred

  bool operator<(const EncoderInfo &other) const {
    return priority < other.priority;
  }
};

/**
 * @brief Factory for creating video encoders
 *
 * Detects available hardware encoders (NVENC, VA-API) and
 * falls back to software (x264) if none are available.
 */
class EncoderFactory {
public:
  /**
   * @brief Detect all available encoders
   * @return List of available encoders, sorted by priority
   */
  static std::vector<EncoderInfo> detectAvailableEncoders();

  /**
   * @brief Create encoder by type
   * @param type Encoder type
   * @return Encoder instance, or nullptr on failure
   */
  static std::unique_ptr<IEncoder> create(IEncoder::EncoderType type);

  /**
   * @brief Create encoder by name
   * @param name Encoder name (e.g., "nvenc_h264", "vaapi_h264", "x264", "auto")
   * @return Encoder instance, or nullptr on failure
   */
  static std::unique_ptr<IEncoder> create(const QString &name);

  /**
   * @brief Create the best available encoder
   * @param preferHardware Prefer hardware encoders if available
   * @return Encoder instance, or nullptr on failure
   */
  static std::unique_ptr<IEncoder> createBest(bool preferHardware = true);

  /**
   * @brief Check if a specific encoder type is available
   */
  static bool isAvailable(IEncoder::EncoderType type);

private:
  static bool probeNvenc();
  static bool probeVaapi();
  static bool probeX264();
};

} // namespace TrophyBackend
