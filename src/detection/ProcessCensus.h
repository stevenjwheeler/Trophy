/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QMap>
#include <QStringList>

namespace TrophyBackend {

struct ProcessInfo {
  qint64 pid = 0;
  qint64 parent = 0;
  quint64 start = 0;
  QString executable;
  QString directory;
  QStringList arguments;
  uint steamId = 0;
  QString winePrefix;
  // Relevant drive mapping, resolved on the census worker. An empty value
  // means the mapping exists but is unusable; payload() never performs I/O.
  QMap<QChar, QString> wineDrives;
  QString identity() const {
    return QString::number(pid) + ":" + QString::number(start);
  }
  QString payload() const;
};

// No connector privileges, ptrace, graphics hooks or compositor connection.
// A scan is a best-effort snapshot, not a promise to observe short-lived tasks.
class ProcessCensus {
public:
  using Snapshot = QMap<qint64, ProcessInfo>;
  static Snapshot scan(const QString &root = QStringLiteral("/proc"));
  static ProcessInfo inspect(qint64 pid,
                             const QString &root = QStringLiteral("/proc"));
  static bool parseStat(const QByteArray &stat, ProcessInfo &process);
};
} // namespace TrophyBackend
