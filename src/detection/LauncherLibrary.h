/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QList>
#include <QString>
#include <QStringList>
#include <QVariantList>
#include <QVariantMap>

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
  // Store identity is evidence for merging metadata, not capture approval.
  QString productId{};
  // Canonical installation identity computed on the census worker. QML/model
  // rebuilds must not resolve paths on the GUI thread.
  QStringList installation{};
};

// Local installed metadata only. Call on the census worker, never from QML.
class LauncherLibrary {
public:
  struct Discovery {
    QList<LauncherGame> games;
    QVariantList launchers;
    // Resolved on the worker, including parents for missing/replaced metadata.
    QStringList watchPaths;
  };
  static Discovery discover(const QString &home, const QString &config,
                            const QString &data, const QVariantMap &preferences = {},
                            const QString &legendaryConfigPath = {});
  static Discovery discover(const QVariantMap &preferences = {});
  static QList<LauncherGame> installed();
  static QList<LauncherGame> read(const QString &home, const QString &config,
                                  const QString &data);
};
} // namespace TrophyBackend
