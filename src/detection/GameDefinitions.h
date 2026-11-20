/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QMap>
#include <QString>

namespace TrophyBackend {
// Original definitions verified against installed executables, not extracted
// from Discord. User definitions override these exact executable basenames.
inline QMap<QString, QString> trophyGameDefinitions() {
  return {{"terraria.bin.x86_64", "Terraria"}};
}
} // namespace TrophyBackend
