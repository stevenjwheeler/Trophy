/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace TrophyBackend {

/**
 * @brief Locate the helper libraries we ship (Vulkan layer, GL shim).
 *
 * Custom prefixes are the whole reason this exists. Both lookups used to
 * probe a hardcoded /usr/lib/trophy list, which never matched
 * `cmake --install --prefix ~/.local` (binaries in <prefix>/bin, libraries
 * in <prefix>/lib/trophy). Nothing failed loudly: the Vulkan layer simply
 * stopped registering and the GL shim could not enrol — and only once the
 * dev build tree was gone, since that used to shadow the lookup.
 *
 * Candidates are ordered by how much we trust them: relative to the
 * shipped manifest (known prefix), then relative to our own executable
 * (works for any prefix, including relocated trees), then system dirs.
 */

/** <prefix>/share/trophy/vulkan_layer/<file> -> <prefix>. */
inline QString prefixFromManifest(const QString &manifestPath) {
  if (manifestPath.isEmpty())
    return QString();
  QDir info = QFileInfo(manifestPath).absoluteDir();
  for (int i = 0; i < 3; ++i) {
    if (!info.cdUp())
      break;
  }
  return info.absolutePath();
}

/**
 * Binary dir -> install prefix. Only a trailing "bin" is stripped
 * (AppImage /usr/bin, ~/.local/bin, /usr/bin, /app/bin); an AppDir /
 * AppImage mount root (AppRun symlink beside usr/, no bin/ of its own)
 * resolves into its usr/ subtree; anything else (e.g. a CMake build
 * tree) is already the useful root.
 */
inline QString prefixFromBinaryDir(const QString &dir) {
  // Purely lexical except for the AppDir probe below (QDir::cdUp refuses
  // non-existent paths, which is all a unit test ever has).
  const QFileInfo info(dir);
  if (info.fileName() == QStringLiteral("bin"))
    return info.absolutePath();
  if (QDir(dir + "/usr/bin").exists() && !QDir(dir + "/bin").exists())
    return QDir(dir + "/usr").absolutePath();
  return info.absoluteFilePath();
}

/** <prefix>/bin/trophy -> <prefix>. */
inline QString prefixFromAppDir() {
  // Resolve launcher symlinks first: under an AppImage the process is
  // started as <mount>/AppRun (a symlink into usr/bin), and only the
  // resolved location reveals the real usr/ prefix.
  const QString real = QFileInfo(QCoreApplication::applicationFilePath())
                           .canonicalFilePath();
  const QString dir =
      real.isEmpty() ? QCoreApplication::applicationDirPath()
                     : QFileInfo(real).absolutePath();
  return prefixFromBinaryDir(dir);
}

/** Library subdirs under one install prefix (incl. multilib layouts). */
inline QStringList libSubDirs(const QString &prefix) {
  return {prefix + "/lib/trophy", prefix + "/lib64/trophy",
          prefix + "/lib/x86_64-linux-gnu/trophy",
          prefix + "/lib/aarch64-linux-gnu/trophy"};
}

/** Directories that may hold shipped libraries, most likely first. */
inline QStringList shippedLibDirs(const QString &manifestPath = QString()) {
  QStringList prefixes;
  const QString fromManifest = prefixFromManifest(manifestPath);
  if (!fromManifest.isEmpty())
    prefixes << fromManifest;
  prefixes << prefixFromAppDir();

  QStringList dirs;
  for (const QString &prefix : prefixes)
    dirs += libSubDirs(prefix);
  dirs << "/usr/lib/trophy" << "/usr/lib64/trophy"
       << "/usr/local/lib/trophy" << "/usr/local/lib64/trophy";
  return dirs;
}

/**
 * First existing <dir>/<libName>, checking @p firstTryDirs (dev build tree
 * layouts) before the shipped prefixes. Empty when nothing is found.
 */
inline QString findShippedLib(const QString &libName,
                              const QStringList &firstTryDirs = {},
                              const QString &manifestPath = QString()) {
  const QStringList dirs = firstTryDirs + shippedLibDirs(manifestPath);
  for (const QString &dir : dirs) {
    const QString candidate = dir + "/" + libName;
    if (QFileInfo::exists(candidate))
      return candidate;
  }
  return QString();
}

} // namespace TrophyBackend
