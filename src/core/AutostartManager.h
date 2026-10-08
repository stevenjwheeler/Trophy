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
 * @brief Manages the XDG autostart entry (~/.config/autostart/trophy.desktop).
 *
 * Only files carrying our marker comment are claimed: isEnabled() ignores
 * foreign entries and disable() never deletes them. configHomeOverride
 * exists for hermetic unit tests (defaults to the real config location).
 */
class AutostartManager : public QObject {
  Q_OBJECT

public:
  explicit AutostartManager(QObject *parent = nullptr);
  explicit AutostartManager(const QString &configHomeOverride,
                            QObject *parent = nullptr);

  QString filePath() const;
  bool isEnabled() const;
  bool setEnabled(bool enabled, const QString &execPath);

private:
  QString m_configHomeOverride;
};

} // namespace TrophyBackend
