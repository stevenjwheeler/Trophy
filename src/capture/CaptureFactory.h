/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <memory>
#include <functional>

class QObject;

namespace TrophyBackend {

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
   * @brief Last confirmed ScreenCast portal availability; false before probing.
   * This getter never makes D-Bus calls and does not probe PipeWire nodes.
   */
  static bool isPipeWireAvailable();

  // Probe the ScreenCast version property without introspection or a picker.
  // Completion runs on context's thread; destroying context cancels delivery.
  // The caller should publish availability changes from this callback.
  static void requestPipeWireAvailability(QObject *context,
                                         std::function<void(bool)> completion);
};

} // namespace TrophyBackend
