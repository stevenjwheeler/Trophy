/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QStringList>

namespace TrophyBackend {

// Exact executable basenames only: never exclude Proton or generic setup.exe.
inline const QStringList &visualCppInstallerNames() {
  static const QStringList names{
      "vc_redist.x86", "vc_redist.x64", "vc_redist.arm64",
      "vcredist_x86", "vcredist_x64", "vcredist_arm64", "vcredist_ia64"};
  return names;
}

inline const QStringList &prerequisiteInstallerNames() {
  static const QStringList names = visualCppInstallerNames() + QStringList{"dxsetup"};
  return names;
}

inline bool gameSupportHelper(const QString &normalizedBasename) {
  return normalizedBasename == "xalia" || normalizedBasename == "vc_redist" ||
         prerequisiteInstallerNames().contains(normalizedBasename);
}

} // namespace TrophyBackend
