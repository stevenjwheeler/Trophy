/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QStringList>
#include <QtGlobal>

namespace TrophyBackend {

/**
 * @brief Resolve who is behind a reporter event.
 *
 * Everything here is derived from the one PID named in a START event (or
 * the reporter-provided executable/argv). It never scans /proc: the only
 * cross-process reads are targeted /proc/<pid> lookups for that PID.
 *
 * Split out of GameDetectionService so the socket handler, the window
 * reconciler and future detection sources share one identity policy.
 */
class GameIdentity {
public:
  /**
   * @brief Stable identity of the process named in a reporter START event.
   *
   * `key` is the canonical match key (steam_app_<id>, exe:<path>, wine
   * prefix + game path, or a reporter class when no executable is known).
   * `name` is the display label the reporter supplied (or the executable
   * basename when the reporter is generic). `ignored` marks
   * helpers/installers/launchers that must never become a game.
   */
  struct ReporterIdentity {
    QString key;
    QString name;
    QString executable;
    bool ignored = false;
  };

  /**
   * @brief Pure resolver.
   * @param reporter  Reporter-supplied application class (may be empty).
   * @param exe       Absolute executable path (or Wine loader path).
   * @param argv      Process arguments (Wine rewrites these; used carefully).
   * @param steamAppId App id from the wire/environment, 0 when unknown.
   * @param winePrefix WINEPREFIX for Wine identities, defaults to ~/.wine.
   */
  static ReporterIdentity resolve(const QString &reporter,
                                  const QString &exe,
                                  const QStringList &argv,
                                  uint steamAppId,
                                  const QString &winePrefix = {});

  /** Targeted /proc/<pid> read: exe, argv, WINEPREFIX, then resolve(). */
  static ReporterIdentity forPid(qint64 pid, const QString &reporter,
                                 uint steamAppId);

  /** SteamAppId from a process environment; 0 when absent/unreadable. */
  static uint steamAppIdForPid(qint64 pid);

  /** "steam_app_1422450" -> 1422450, anything else -> 0. */
  static uint steamAppIdFromAppId(const QString &appId);

  /**
   * @brief Single-PID Steam evidence: SteamAppId env, or an executable or
   * command line under a steamapps library. Never a process scan.
   */
  static bool looksLikeSteamGame(qint64 pid);

  /** Wine loader names (wine, wine64, *-preloader, wineserver, proton). */
  static bool isWineLoader(const QString &name);

  /** Helper/installer/crash-handler names that must never be games. */
  static bool isHelperName(const QString &name);
};

} // namespace TrophyBackend
