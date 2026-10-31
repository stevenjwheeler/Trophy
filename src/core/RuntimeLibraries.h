/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QStringList>

namespace TrophyBackend {

// Which shared libraries this process actually mapped, split by origin.
//
// The AppImage deliberately mixes two sources: Qt/FFmpeg/codecs come from the
// bundle, while libva and the driver/OS stack come from the host. When a
// machine misbehaves, the first question is always which half it used, so the
// answer is written to the log once at startup instead of being reconstructed
// from a stack of guesses.
struct LibraryInventory {
  QStringList host;
  QStringList bundle;
  QStringList bundleRoots;
};

// `mapsPath` is normally /proc/self/maps; `bundleRoots` are the directories
// that count as "ours" (the AppImage payload or the installed prefix).
LibraryInventory collectLibraryInventory(const QString &mapsPath,
                                         const QStringList &bundleRoots);

QString formatLibraryInventory(const LibraryInventory &inventory);

// Convenience wrapper using /proc/self/maps and the real bundle roots.
QString runtimeLibraryReport();

} // namespace TrophyBackend
