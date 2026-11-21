/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QList>
#include <QString>
#include <QStringList>

namespace TrophyBackend {
struct LauncherGame {
  // Steam manifests attest installation-directory membership; other sources
  // must supply an exact executable, optionally scoped to a Wine prefix.
  uint steamId = 0;
  QString name, directory;
  QString executable{}, winePrefix{};
  bool bootstrap = false;
  QStringList sources{};
  bool runtimeTool = false;
};

// Local installed metadata only. Call on the census worker, never from QML.
class LauncherLibrary {
public:
  static QList<LauncherGame> installed();
  static QList<LauncherGame> read(const QString &home, const QString &config,
                                  const QString &data);
};
} // namespace TrophyBackend
