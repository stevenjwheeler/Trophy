/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QUrl>

namespace TrophyBackend {

// Canonical repo for bug reports (matches InfoPage + UpdateChecker).
inline const QString kBugReportRepo =
    QStringLiteral("https://github.com/OasisLab-Software/Trophy");

// Browsers cannot receive a file attachment via URL, so the report
// prefills the issue body with the version, OS, and the tail of the
// log instead. The full log path is named in the body for manual attach.
QString bugReportLogPath();
QString readLogTail(const QString &logPath, int maxLines = 50,
                    int maxChars = 4000);
QUrl bugReportUrl(const QString &version, const QString &logTail);

} // namespace TrophyBackend
