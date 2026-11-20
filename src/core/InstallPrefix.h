/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QDir>
#include <QFileInfo>

namespace TrophyBackend {
inline QString prefixFromBinaryDir(const QString &directory) {
  const QFileInfo info(directory);
  if (info.fileName() == QStringLiteral("bin"))
    return info.absolutePath();
  if (QDir(directory + "/usr/bin").exists() &&
      !QDir(directory + "/bin").exists())
    return QDir(directory + "/usr").absolutePath();
  return info.absoluteFilePath();
}
} // namespace TrophyBackend
