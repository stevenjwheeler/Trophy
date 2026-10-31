/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "LegacyGlShimCleanup.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSaveFile>
#include <QStandardPaths>
#include <QStringList>

namespace TrophyBackend::LegacyGlShimCleanup {

namespace {

bool isOwnedEntry(const QString &entry) {
  if (entry.isEmpty())
    return false;
  const QString name = QFileInfo(entry).fileName();
  return name == QStringLiteral("libtrophy_gl_shim.so") ||
         entry.contains(QStringLiteral("trophy_gl_shim")) ||
         entry.contains(QStringLiteral("/gl-multilib-v1-"));
}

} // namespace

QString confFilePath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         QStringLiteral("/environment.d/trophy-gl-shim.conf");
}

bool removeOwnedEnrollment() {
  const QString path = confFilePath();
  QFile input(path);
  if (!input.exists())
    return true;
  if (!input.open(QIODevice::ReadOnly | QIODevice::Text))
    return false;
  const QString content = QString::fromUtf8(input.readAll());
  input.close();

  QStringList output;
  bool changed = false;
  for (const QString &line : content.split('\n')) {
    const QString trimmed = line.trimmed();
    if (!trimmed.startsWith(QStringLiteral("LD_PRELOAD="))) {
      output.append(line);
      continue;
    }
    const qsizetype marker = line.indexOf(QStringLiteral("LD_PRELOAD="));
    QStringList kept;
    const QString value = trimmed.mid(int(sizeof("LD_PRELOAD=") - 1));
    // LD_PRELOAD entries are separated by colons and/or whitespace.
    for (const QString &entry :
         value.split(QRegularExpression(QStringLiteral("[\\s:]+")),
                     Qt::SkipEmptyParts)) {
      if (isOwnedEntry(entry)) {
        changed = true;
        continue;
      }
      kept.append(entry);
    }
    if (kept.isEmpty()) {
      changed = true;
      continue; // Drop the assignment entirely.
    }
    output.append(line.left(marker) + QStringLiteral("LD_PRELOAD=") +
                  kept.join(QLatin1Char(':')));
  }
  if (!changed)
    return true;

  while (!output.isEmpty() && output.last().trimmed().isEmpty())
    output.removeLast();
  const QString result =
      output.isEmpty() ? QString() : output.join(QLatin1Char('\n')) + '\n';
  if (result.trimmed().isEmpty())
    return QFile::remove(path);

  QSaveFile file(path);
  if (!file.open(QIODevice::WriteOnly | QIODevice::Text))
    return false;
  file.write(result.toUtf8());
  return file.commit();
}

} // namespace TrophyBackend::LegacyGlShimCleanup
