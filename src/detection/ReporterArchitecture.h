/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>

namespace TrophyBackend::ReporterArchitecture {

inline bool isX86SharedObject(const QByteArray &bytes, int bits) {
  if (bytes.size() < 20 || bytes.left(4) != QByteArray("\x7f" "ELF", 4)) return false;
  const auto byte = [&](int i) { return static_cast<unsigned char>(bytes[i]); };
  return byte(4) == (bits == 32 ? 1 : 2) && byte(5) == 1 && byte(6) == 1 &&
         byte(16) == 3 && byte(17) == 0 && // ET_DYN, little endian
         byte(18) == (bits == 32 ? 3 : 62) && byte(19) == 0;
}

inline QByteArray readSharedObject(const QString &path, int bits) {
  const QFileInfo info(path);
  // Never open FIFOs/devices or unbounded override files on the owner thread.
  if (!info.isFile() || info.size() < 20 || info.size() > 16 * 1024 * 1024) return {};
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly)) return {};
  const QByteArray bytes = file.readAll();
  return file.error() == QFileDevice::NoError && isX86SharedObject(bytes, bits) ? bytes : QByteArray();
}

// Only companions of this primary installation/build, never an unrelated
// system prefix. Returning a present but invalid candidate lets activation fail
// closed instead of silently downgrading a broken multilib package.
inline QString companion32(const QString &primary) {
  if (primary.isEmpty()) return {};
  const QFileInfo info(primary);
  const QString directory = info.absolutePath();
  const QString name = info.fileName();
  QStringList candidates{directory + "/reporters32/" + name};
  if (QFileInfo(directory).fileName() == "vulkan_layer")
    candidates << QDir::cleanPath(directory + "/../reporters32/" + name);
  for (const auto &libdir : {QString("lib"), QString("lib64"), QString("lib/x86_64-linux-gnu")}) {
    const QString suffix = '/' + libdir + "/trophy/" + name;
    if (info.absoluteFilePath().endsWith(suffix)) {
      const QString prefix = info.absoluteFilePath().chopped(suffix.size());
      candidates.prepend(prefix + "/lib/trophy/reporters32/" + name);
    }
  }
  for (const QString &candidate : candidates) {
    const QFileInfo file(candidate);
    if (file.exists() || file.isSymLink()) return QDir::cleanPath(candidate);
  }
  return {};
}
} // namespace TrophyBackend::ReporterArchitecture
