/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace TrophyBackend {

/**
 * @brief Install/remove/query the Vulkan implicit detection layer.
 *
 * The layer manifest must point at the real .so. This class builds the
 * destination JSON structurally (QJsonDocument) instead of string
 * surgery, which previously corrupted the already-expanded CMake path
 * into garbage like ".../lib/tmp/.../trophy_layer.so" and silently
 * disabled Vulkan detection for every game.
 *
 * Layouts: dev runs use <appDir>/vulkan_layer/ (manifest + lib side by
 * side); installs use <datadir>/trophy/vulkan_layer/trophy_layer.json
 * with the lib in <libdir>/trophy. Destination is always
 * ~/.local/share/vulkan/implicit_layer.d/trophy_layer.json.
 */
class VulkanLayer {
public:
  static QString destManifestPath();

  /** Source manifest we ship (dev build tree or installed datadir). */
  static QString sourceManifestPath();

  /** Library matching the source manifest (side-by-side or libdir). */
  static QString libraryPathForSource(const QString &srcManifestPath);

  /** True when the destination manifest exists and points at a real lib. */
  static bool isRegistered();

  /** Opt-in: persist bundle-owned helpers and refresh the manifest atomically. */
  static bool ensureRegistered();

  /** Remove our destination manifest (opt-out). */
  static bool removeRegistration();

  /** Structural copy: template manifest + concrete library path. */
  static bool writeManifest(const QString &srcJsonPath,
                            const QString &libraryPath,
                            const QString &destPath);
};

} // namespace TrophyBackend
