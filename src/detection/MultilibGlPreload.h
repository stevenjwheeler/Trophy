/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "PersistentReporterLib.h"
#include "ReporterArchitecture.h"
#include <QTemporaryDir>
#include <QRegularExpression>

namespace TrophyBackend::MultilibGlPreload {

inline QStringList platforms(int bits) {
  // Linux x86 glibc platform names, including older optimized x86_64 names.
  // Unlike $LIB, these do not collide across Arch/Debian/Fedora loaders.
  return bits == 64 ? QStringList{"x86_64", "haswell", "xeon_phi"}
                    : QStringList{"i386", "i486", "i586", "i686"};
}

inline QString key(const QByteArray &native, const QByteArray &compat) {
  return PersistentReporterLib::digest("trophy-gl-platform-v1\n" +
      PersistentReporterLib::digest(native).toLatin1() + '\n' +
      PersistentReporterLib::digest(compat).toLatin1());
}

inline QString tokenDirectory(QString entry) {
  // environment.d uses $$ to hand a literal $ to the dynamic loader. Runtime
  // LD_PRELOAD has one dollar; accept both only in this exact managed shape.
  entry.replace("$$PLATFORM", "$PLATFORM");
  const QString suffix = "/$PLATFORM/libtrophy_gl_shim.so";
  if (!entry.endsWith(suffix)) return {};
  const QString directory = entry.chopped(suffix.size());
  const QFileInfo info(directory);
  const QString prefix = "gl-multilib-v1-";
  const QString hash = info.fileName().mid(prefix.size());
  if (info.absolutePath() != QDir(PersistentReporterLib::directory()).absolutePath() ||
      !info.fileName().startsWith(prefix) ||
      !QRegularExpression("^[0-9a-f]{64}$").match(hash).hasMatch()) return {};
  return directory;
}

inline bool validate(const QString &directory) {
  if (QFileInfo(directory).isSymLink() || !QFileInfo(directory).isDir()) return false;
  QByteArray native, compat;
  for (int bits : {64, 32}) {
    QByteArray expected;
    for (const QString &platform : platforms(bits)) {
      const QString parent = directory + '/' + platform;
      const QString path = parent + "/libtrophy_gl_shim.so";
      if (QFileInfo(parent).isSymLink() || QFileInfo(path).isSymLink()) return false;
      const QByteArray bytes = ReporterArchitecture::readSharedObject(path, bits);
      if (bytes.isEmpty() || (!expected.isEmpty() && expected != bytes)) return false;
      expected = bytes;
    }
    (bits == 64 ? native : compat) = expected;
  }
  return QFileInfo(directory).fileName() == "gl-multilib-v1-" + key(native, compat);
}

inline bool isOwned(const QString &entry) {
  const QString directory = tokenDirectory(entry);
  return !directory.isEmpty() && validate(directory);
}

inline QString install(const QString &source64, const QString &source32) {
  // Capture validated bytes once, before publishing any new enrollment.
  if (QFileInfo(source32).isSymLink()) return {};
  const QByteArray native = ReporterArchitecture::readSharedObject(source64, 64);
  const QByteArray compat = ReporterArchitecture::readSharedObject(source32, 32);
  if (native.isEmpty() || compat.isEmpty()) return {};
  const QString parent = PersistentReporterLib::directory();
  if (parent.contains(QRegularExpression("[\\s:$\"'\\\\]")) || QFileInfo(parent).isSymLink()) return {};
  const QString destination = parent + "/gl-multilib-v1-" + key(native, compat);
  if (QFileInfo::exists(destination) || QFileInfo(destination).isSymLink())
    return validate(destination) ? destination + "/$$PLATFORM/libtrophy_gl_shim.so" : QString();
  if (!QDir().mkpath(parent)) return {};
  QTemporaryDir staging(parent + "/.gl-multilib-XXXXXX");
  if (!staging.isValid()) return {};
  for (int bits : {64, 32}) {
    for (const QString &platform : platforms(bits)) {
      const QString directory = staging.path() + '/' + platform;
      if (!QDir().mkpath(directory)) return {};
      QFile file(directory + "/libtrophy_gl_shim.so");
      const QByteArray &bytes = bits == 64 ? native : compat;
      if (!file.open(QIODevice::WriteOnly) || file.write(bytes) != bytes.size() ||
          !file.setPermissions(QFileDevice::ReadOwner | QFileDevice::ReadGroup | QFileDevice::ReadOther) ||
          !file.flush() || file.error() != QFileDevice::NoError) return {};
      file.close();
    }
  }
  // One directory rename publishes the complete immutable architecture pair.
  // Old versions stay available to already-running processes/login sessions.
  if (!QDir().rename(staging.path(), destination))
    return validate(destination) ? destination + "/$$PLATFORM/libtrophy_gl_shim.so" : QString();
  staging.setAutoRemove(false);
  return destination + "/$$PLATFORM/libtrophy_gl_shim.so";
}
} // namespace TrophyBackend::MultilibGlPreload
