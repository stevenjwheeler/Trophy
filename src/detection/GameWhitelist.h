/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QMap>
#include <QString>
#include <QStringList>

namespace GameRecorder {

/**
 * @brief User-defined game whitelist and blacklist
 *
 * Stores user preferences for which applications should be
 * recognized as games (whitelist) or ignored (blacklist).
 */
class GameWhitelist {
public:
  GameWhitelist();
  ~GameWhitelist();

  /**
   * @brief Load whitelist/blacklist from disk
   */
  void load();

  /**
   * @brief Save whitelist/blacklist to disk
   */
  void save() const;

  // Whitelist operations
  void addToWhitelist(const QString &windowClass,
                      const QString &displayName = QString());
  void removeFromWhitelist(const QString &windowClass);
  bool isWhitelisted(const QString &windowClass) const;
  QString getWhitelistName(const QString &windowClass) const;
  QStringList whitelistEntries() const;

  // Blacklist operations
  void addToBlacklist(const QString &windowClass);
  void removeFromBlacklist(const QString &windowClass);
  bool isBlacklisted(const QString &windowClass) const;
  QStringList blacklistEntries() const;

private:
  QString configFilePath() const;

  // windowClass -> displayName
  QMap<QString, QString> m_whitelist;
  QStringList m_blacklist;
};

} // namespace GameRecorder
