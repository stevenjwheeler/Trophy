/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <memory>

namespace GameRecorder {

class IVideoCaptureSource;

/**
 * @brief Factory for creating video capture sources
 *
 * Detects the display server (X11 or Wayland) and creates
 * the appropriate capture source.
 */
class CaptureFactory {
public:
  enum class DisplayServer { Unknown, X11, Wayland };

  /**
   * @brief Detect the current display server
   * @return DisplayServer type
   */
  static DisplayServer detectDisplayServer();

  /**
   * @brief Create the best available capture source
   * @return Capture source, or nullptr on failure
   */
  static std::unique_ptr<IVideoCaptureSource> createBestSource();


  /**
   * @brief Create a capture source for a specific display server
   * @param server Display server type
   * @return Capture source, or nullptr on failure
   */
  static std::unique_ptr<IVideoCaptureSource>
  createSource(DisplayServer server);

  /**
   * @brief Get display server name as string
   */
  static QString displayServerName(DisplayServer server);

  /**
   * @brief Check if PipeWire is available for capture
   */
  static bool isPipeWireAvailable();
};

} // namespace GameRecorder
