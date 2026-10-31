/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace TrophyBackend {

/**
 * Relaunch Trophy after this process exits.
 *
 * Some changes (desktop registration, the portal app id, the platform theme
 * that provides native file dialogs) are fixed when QGuiApplication is
 * created, so they only take effect in a new process. The helper script waits
 * for the current PID to disappear before starting the target, which lets the
 * current instance shut down normally instead of being killed.
 */
namespace AppRestart {

// The script that waits for `parentPid` and then starts `target`. Exposed for
// testing (quoting and the wait loop are the fragile parts).
QString script(qint64 parentPid, const QString &target);

// Write the script and start it detached. False when the target is missing or
// the script cannot be launched; the caller must not quit in that case.
bool scheduleAfterExit(const QString &target);

} // namespace AppRestart

} // namespace TrophyBackend
