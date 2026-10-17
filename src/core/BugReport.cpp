/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "BugReport.h"

#include <QFile>
#include <QStandardPaths>
#include <QSysInfo>
#include <QUrlQuery>

namespace TrophyBackend {

QString bugReportLogPath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         QStringLiteral("/trophy/trophy.log");
}

QString readLogTail(const QString &logPath, int maxLines, int maxChars) {
  QFile file(logPath);
  if (!file.open(QIODevice::ReadOnly | QIODevice::Text))
    return {};
  const QString content = QString::fromUtf8(file.readAll());
  if (content.isEmpty())
    return {};
  QStringList lines = content.split(u'\n');
  // A trailing newline yields one empty phantom line; drop it so the
  // tail holds real content instead of wasting a line on "".
  while (!lines.isEmpty() && lines.last().isEmpty())
    lines.removeLast();
  if (lines.size() > maxLines)
    lines = lines.mid(lines.size() - maxLines);
  QString tail = lines.join(u'\n');
  if (tail.size() > maxChars)
    tail = tail.right(maxChars);
  return tail;
}

QUrl bugReportUrl(const QString &version, const QString &logTail) {
  const QString tail =
      logTail.isEmpty() ? QStringLiteral("(log unavailable or empty)") : logTail;
  const QString body =
      QStringLiteral("**Describe the bug**\n<!-- Please describe what happened -->\n\n"
                     "**Version:** %1\n**OS:** %2 (%3 %4)\n\n"
                     "**Log (last lines of trophy.log):**\n```text\n%5\n```\n"
                     "Full log: %6 — attach it if the tail above was truncated.\n")
          .arg(version.isEmpty() ? QStringLiteral("unknown") : version,
               QSysInfo::prettyProductName(),
               QSysInfo::kernelVersion(), QSysInfo::currentCpuArchitecture(),
               tail, bugReportLogPath());
  QUrl url(kBugReportRepo + QStringLiteral("/issues/new"));
  QUrlQuery query;
  query.addQueryItem(QStringLiteral("labels"), QStringLiteral("bug"));
  query.addQueryItem(QStringLiteral("title"), QStringLiteral("[Bug]: "));
  query.addQueryItem(QStringLiteral("body"), body);
  url.setQuery(query);
  return url;
}

} // namespace TrophyBackend
