/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

namespace TrophyBackend {

/**
 * @brief Manages the XDG autostart entry
 * (~/.config/autostart/io.github.stevenjwheeler.Trophy.desktop).
 *
 * Only files carrying our marker comment are claimed: isEnabled() ignores
 * foreign entries and setEnabled() never overwrites or deletes them.
 * configHomeOverride exists for hermetic unit tests (defaults to the real
 * config location).
 */
class AutostartManager : public QObject {
  Q_OBJECT

public:
  explicit AutostartManager(QObject *parent = nullptr);
  explicit AutostartManager(const QString &configHomeOverride,
                            QObject *parent = nullptr);

  QString filePath() const;
  bool isEnabled() const;
  // Pure Desktop Entry Exec resolver; explicit inputs keep packaging tests
  // independent of the environment the test runner was launched in.
  static QString launchCommand(const QString &execPath,
                               const QString &appImagePath = QString());
  bool setEnabled(bool enabled, const QString &execPath,
                  const QString &appImagePath = QString());

private:
  QString autostartDir() const;
  QString legacyFilePath() const;
  QString m_configHomeOverride;
};

} // namespace TrophyBackend
