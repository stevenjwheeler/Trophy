/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace TrophyBackend {
namespace EncoderTuning {

/**
 * Map the generic preset names exposed in Settings ("ultrafast" … "slow")
 * onto NVENC's p1–p7 presets.
 *
 * NVENC does not accept x264's preset names: handing it "veryfast" makes
 * avcodec_open2 fail. That is why the encoder used to ignore
 * EncoderConfig::preset and hard-code p4 instead.
 */
inline QString nvencPreset(const QString &generic) {
  const QString preset = generic.trimmed().toLower();
  if (preset == QStringLiteral("ultrafast")) return QStringLiteral("p1");
  if (preset == QStringLiteral("superfast")) return QStringLiteral("p2");
  if (preset == QStringLiteral("veryfast")) return QStringLiteral("p3");
  if (preset == QStringLiteral("faster")) return QStringLiteral("p4");
  if (preset == QStringLiteral("fast")) return QStringLiteral("p5");
  if (preset == QStringLiteral("medium")) return QStringLiteral("p6");
  if (preset == QStringLiteral("slow")) return QStringLiteral("p7");
  // Unknown/empty keeps NVENC's own medium default instead of failing.
  return QStringLiteral("p4");
}

/** x264 accepts the generic preset names directly; reject anything else. */
inline QString x264Preset(const QString &generic) {
  const QString preset = generic.trimmed().toLower();
  if (preset == QStringLiteral("ultrafast") ||
      preset == QStringLiteral("superfast") ||
      preset == QStringLiteral("veryfast") ||
      preset == QStringLiteral("faster") || preset == QStringLiteral("fast") ||
      preset == QStringLiteral("medium") || preset == QStringLiteral("slow")) {
    return preset;
  }
  return QStringLiteral("medium");
}

} // namespace EncoderTuning
} // namespace TrophyBackend
