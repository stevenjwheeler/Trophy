/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSaveFile>
#include <QStandardPaths>

namespace TrophyBackend::PersistentReporterLib {

inline QString directory() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         "/trophy/reporters";
}

inline bool isBundled(const QString &source) {
  // APPIMAGE alone may be inherited by a native install. Resolve symlinks as
  // well: an override outside the actual AppDir must retain native semantics.
  const QString appDir = QFileInfo(QString::fromLocal8Bit(qgetenv("APPDIR")))
                             .canonicalFilePath();
  const QString canonical = QFileInfo(source).canonicalFilePath();
  return !qgetenv("APPDIR").isEmpty() && !appDir.isEmpty() &&
         !canonical.isEmpty() && canonical.startsWith(appDir + '/');
}

inline QString digest(const QByteArray &bytes) {
  return QString::fromLatin1(
      QCryptographicHash::hash(bytes, QCryptographicHash::Sha256).toHex());
}

// Called only by explicit enrollment, never by lookup/status getters. Existing
// versions are intentionally retained: other processes may still map them.
inline QString install(const QString &source, const QString &baseName = {}) {
  if (!isBundled(source))
    return source;
  QFile input(source);
  if (!input.open(QIODevice::ReadOnly))
    return {};
  const QByteArray bytes = input.readAll();
  if (input.error() != QFileDevice::NoError || bytes.isEmpty())
    return {};
  const QString name = (baseName.isEmpty() ? QFileInfo(source).completeBaseName()
                                         : baseName) + '-' +
                       digest(bytes) + ".so";
  const QString destination = directory() + '/' + name;
  if (QFileInfo::exists(destination)) {
    QFile existing(destination);
    if (QFileInfo(destination).isSymLink() ||
        !existing.open(QIODevice::ReadOnly) || existing.readAll() != bytes ||
        existing.error() != QFileDevice::NoError)
      return {}; // Never replace a damaged/foreign content-addressed object.
    return destination;
  }
  if (!QDir().mkpath(directory()))
    return {};
  QSaveFile output(destination);
  if (!output.open(QIODevice::WriteOnly) ||
      output.write(bytes) != bytes.size() ||
      !output.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadGroup |
                             QFileDevice::ReadOther) ||
      !output.commit())
    return {};
  return destination;
}

inline bool isOwned(const QString &path, const QString &baseName) {
  const QFileInfo info(path);
  const QString prefix = baseName + '-';
  const QString name = info.fileName();
  if (info.isSymLink() || !info.isFile() ||
      info.absolutePath() != QDir(directory()).absolutePath() ||
      !name.startsWith(prefix) || !name.endsWith(".so"))
    return false;
  const QString hash = name.mid(prefix.size(), name.size() - prefix.size() - 3);
  if (hash.size() != 64)
    return false;
  QFile file(path);
  if (!file.open(QIODevice::ReadOnly))
    return false;
  const QByteArray bytes = file.readAll();
  return !bytes.isEmpty() && file.error() == QFileDevice::NoError &&
         digest(bytes) == hash;
}
} // namespace TrophyBackend::PersistentReporterLib
