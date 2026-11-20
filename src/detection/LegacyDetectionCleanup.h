/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>

namespace TrophyBackend {
// One-way upgrade cleanup. No reporter registration/enrollment code remains.
inline void removeLegacyDetectionEnrollment() {
  const auto data =
      QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
  QFile::remove(data + "/vulkan/implicit_layer.d/trophy_layer.json");
  const auto config =
      QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
  const QString path = config + "/environment.d/trophy-gl-shim.conf";
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return;
  const auto original = file.read(1048577);
  if (original.size() > 1048576)
    return; // Do not rewrite unexpected input.
  QStringList kept;
  for (const auto &line : QString::fromUtf8(original).split('\n')) {
    if (!line.trimmed().startsWith("LD_PRELOAD=")) {
      kept.append(line);
      continue;
    }
    QStringList entries;
    for (const auto &entry : line.trimmed().mid(11).split(
             QRegularExpression("[:\\s]+"), Qt::SkipEmptyParts)) {
      const auto name = QFileInfo(entry).fileName();
      if (name == "libtrophy_gl_shim.so" ||
          (name.startsWith("libtrophy_gl_shim-") && name.endsWith(".so")))
        continue;
      entries.append(entry);
    }
    if (!entries.isEmpty())
      kept.append("LD_PRELOAD=" + entries.join(':'));
  }
  file.close();
  QSaveFile output(path);
  if (output.open(QIODevice::WriteOnly)) {
    const auto bytes = kept.join('\n').toUtf8();
    if (output.write(bytes) == bytes.size())
      output.commit();
  }
}
} // namespace TrophyBackend
