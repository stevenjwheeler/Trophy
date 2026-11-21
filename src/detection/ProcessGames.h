/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ProcessCensus.h"
#include "LauncherLibrary.h"
#include "GameLaunchContext.h"
#include <QList>
#include <QMutex>

namespace TrophyBackend {
struct GameInstance {
  QString id, key, name;
  qint64 primaryPid = 0;
  ProcessCensus::Snapshot processes;
  GameLaunchContext launchContext{};
  QStringList detectionSources{};
};

// Pure identification plus a small thread-safe publication boundary for the
// PipeWire capture workers. All rules are exact identities, never substrings.
class ProcessGames {
public:
  static QList<GameInstance> identify(const ProcessCensus::Snapshot &processes,
                                      const QList<LauncherGame> &catalogue,
                                      const QMap<QString, QString> &definitions,
                                      const QStringList &blocked,
                                      const QList<GameInstance> &previous = {});
  static bool excluded(const QString &payload);
  static bool bootstrapCandidate(const QString &payload);
  static QString verifiedBootstrapDirectory(
      const ProcessInfo &process, const QList<LauncherGame> &catalogue);
  static QString verifiedInstallationDirectory(
      const ProcessInfo &process, const QList<LauncherGame> &catalogue);
  static void publish(const QList<GameInstance> &games);
  static QList<GameInstance> snapshot();
  // Returns an instance ID or empty. A present but unowned PID is a veto.
  // No PID: exact executable evidence must resolve to precisely one instance.
  static QString associate(qint64 pid, const QString &binary,
                           const QString &application = {});
  static QString instanceForPid(qint64 pid);
  static QString pinProcess(qint64 pid);
  static bool belongsTo(const QString &instance, qint64 pid,
                        const QString &binary, const QString &application = {});
};
} // namespace TrophyBackend
