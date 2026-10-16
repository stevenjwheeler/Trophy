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

namespace TrophyBackend {

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
  // Known default app IDs resolve to the built-in row for veto CRUD only.
  // No fuzzy matching; whitelist/token/stable-identity keys are unchanged.
  void addToBlacklist(const QString &windowClass);
  void removeFromBlacklist(const QString &windowClass);
  bool isBlacklisted(const QString &windowClass) const;
  QStringList blacklistEntries() const;
  // Built-in keys the user explicitly neutralized (kept out of the seed).
  QStringList removedDefaults() const { return m_removedDefaults; }
  // Keys the user explicitly added (Add dialog): they stay visible even
  // with no rule attached, so Prompt never makes a watched game vanish.
  void noteCustomKey(const QString &key);
  void forgetCustomKey(const QString &key);
  QStringList customKeys() const { return m_customKeys; }

  // Normalized match keys (lowercase, no .exe suffix) are compared
  // everywhere, so "Steam", "steam" and "steam.exe" are one entry.
  // Display names are stored untouched.
  static QString normalizeKey(const QString &key);
  // The Vulkan layer's null-name fallback ("Unknown Vulkan Game"): it
  // carries no game identity, so it must never become a whitelist match
  // key — every unidentified reporter would alias to one game.
  static bool isPlaceholderReporterKey(const QString &key);
  // Built-in exclusions (browsers, helpers, test cubes). Shown in their
  // own collapsed section; user entries are never flagged as system.
  static QStringList defaultBlacklistEntries();
  // Friendly label for a built-in key ("steam" -> "Steam"); anything
  // else echoes back unchanged.
  static QString prettySystemName(const QString &canonicalKey);

private:
  QString configFilePath() const;
  // Built-in keys the user explicitly removed (Allow on a system row).
  // Persisted so a restart doesn't resurrect them; re-adding the key
  // clears the marker.
  QStringList m_removedDefaults;
  QStringList m_customKeys;


  // windowClass -> displayName (keys always stored normalized)
  QMap<QString, QString> m_whitelist;
  QStringList m_blacklist;
};

} // namespace TrophyBackend
