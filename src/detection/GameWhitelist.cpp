/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GameWhitelist.h"
#include "DefaultAppCatalog.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QSet>

namespace TrophyBackend {

namespace {
bool retiredDesktopDefault(const QString &key) {
  static const QStringList retired{
      "steam", "discord", "obs", "unknown vulkan game", "vkcube",
      "firefox", "chrome", "chromium", "brave", "msedge", "opera", "vivaldi",
      "librewolf", "thunderbird", "slack", "telegram", "signal", "element",
      "zoom", "spotify", "vlc", "mpv", "gimp", "krita", "inkscape", "blender",
      "code", "konsole", "gnome-terminal", "alacritty", "kitty", "foot",
      "wezterm", "dolphin", "nautilus", "thunar"};
  return retired.contains(key);
}
}

QStringList GameWhitelist::defaultBlacklistEntries() {
  // Only prerequisite/accessibility helpers remain seeded here. Desktop apps
  // require installation evidence or explicit rules, not blanket vetoes.
  // Runtime tools are classified from Steam IDs/manifests on the census worker.
  return {
      "vc_redist",
      "dxsetup",
      "xalia",
  };
}

QString GameWhitelist::normalizeKey(const QString &key) {
  QString n = key.trimmed().toLower();
  if (n.endsWith(QStringLiteral(".exe")))
    n.chop(4);
  return n;
}

bool GameWhitelist::isPlaceholderReporterKey(const QString &key) {
  return normalizeKey(key) == QStringLiteral("unknown vulkan game");
}

QString GameWhitelist::prettySystemName(const QString &canonicalKey) {
  static const QMap<QString, QString> pretty{
      {"xalia", "Xalia"},
      {"steam", "Steam"},
      {"discord", "Discord"},
      {"obs", "OBS"},
      {"unknown vulkan game", "Unknown Vulkan Game"},
      {"vkcube", "Vulkan Cube (test app)"},
      {"firefox", "Firefox"},
      {"chrome", "Chrome"},
      {"chromium", "Chromium"},
      {"brave", "Brave"},
      {"msedge", "Edge"},
      {"opera", "Opera"},
      {"vivaldi", "Vivaldi"},
      {"librewolf", "LibreWolf"},
      {"thunderbird", "Thunderbird"},
      {"slack", "Slack"},
      {"telegram", "Telegram"},
      {"signal", "Signal"},
      {"element", "Element"},
      {"zoom", "Zoom"},
      {"spotify", "Spotify"},
      {"vlc", "VLC"},
      {"mpv", "mpv"},
      {"gimp", "GIMP"},
      {"krita", "Krita"},
      {"inkscape", "Inkscape"},
      {"blender", "Blender"},
      {"code", "VS Code"},
      {"konsole", "Konsole"},
      {"gnome-terminal", "Terminal"},
      {"alacritty", "Alacritty"},
      {"kitty", "kitty"},
      {"foot", "foot"},
      {"wezterm", "WezTerm"},
      {"dolphin", "Dolphin"},
      {"nautilus", "Files"},
      {"thunar", "Thunar"},
  };
  return pretty.value(defaultExclusionKey(normalizeKey(canonicalKey)), canonicalKey);
}

GameWhitelist::GameWhitelist() {
  m_blacklist = defaultBlacklistEntries();
}

GameWhitelist::~GameWhitelist() = default;

QString GameWhitelist::configFilePath() const {
  QString configPath =
      QStandardPaths::writableLocation(QStandardPaths::ConfigLocation);
  return configPath + "/trophy/game-whitelist.json";
}

void GameWhitelist::load() {
  QString filePath = configFilePath();
  QFile file(filePath);

  if (!file.exists()) {
    qDebug() << "No whitelist file found, using empty lists";
    return;
  }

  if (!file.open(QIODevice::ReadOnly)) {
    qWarning() << "Failed to open whitelist file:" << file.errorString();
    return;
  }

  QByteArray data = file.readAll();
  file.close();

  QJsonParseError parseError;
  QJsonDocument doc = QJsonDocument::fromJson(data, &parseError);

  if (parseError.error != QJsonParseError::NoError) {
    qWarning() << "Failed to parse whitelist JSON:" << parseError.errorString();
    return;
  }

  QJsonObject root = doc.object();

  // Load whitelist (normalize: legacy files hold "Steam"/"steam" dupes)
  m_whitelist.clear();
  QJsonObject whitelistObj = root["whitelist"].toObject();
  for (auto it = whitelistObj.begin(); it != whitelistObj.end(); ++it) {
    const QString key = normalizeKey(it.key());
    if (!key.isEmpty() && !m_whitelist.contains(key))
      m_whitelist[key] = it.value().toString();
  }

  QJsonArray customArr = root["customKeys"].toArray();
  for (const QJsonValue &val : customArr) {
    const QString entry = normalizeKey(val.toString());
    if (!entry.isEmpty() && !m_customKeys.contains(entry))
      m_customKeys.append(entry);
  }

  // Retire the old seeded desktop-app list once. Recorded custom identities
  // and definitions are explicit user choices; subsequent Deny edits are
  // distinguished by the policy version written on every save.
  const bool migrateDefaults = root["defaultPolicyVersion"].toInt(1) < 2;
  QSet<QString> explicitKeys;
  for (const auto &key : m_customKeys + m_whitelist.keys())
    explicitKeys.insert(defaultExclusionKey(key));
  QJsonArray blacklistArr = root["blacklist"].toArray();
  for (const QJsonValue &val : blacklistArr) {
    const auto entry = defaultExclusionKey(normalizeKey(val.toString()));
    if (migrateDefaults && retiredDesktopDefault(entry) && !explicitKeys.contains(entry)) {
      m_blacklist.removeAll(entry);
      continue;
    }
    if (!entry.isEmpty() && !m_blacklist.contains(entry))
      m_blacklist.append(entry);
  }

  // Honor explicit removals of built-ins: the constructor seeded them,
  // the file simply omits them, so subtract the recorded markers.
  // Legacy split rows (obs64, signal-desktop, etc.) map to their app here.
  // If either former row was unblocked, keep the consolidated app unblocked.
  QJsonArray removedArr = root["removedDefaults"].toArray();
  for (const QJsonValue &val : removedArr) {
    const QString entry = defaultExclusionKey(normalizeKey(val.toString()));
    if (!entry.isEmpty()) {
      if (retiredDesktopDefault(entry))
        m_removedDefaults.removeAll(entry);
      else if (!m_removedDefaults.contains(entry))
        m_removedDefaults.append(entry);
      m_blacklist.removeAll(entry);
    }
  }

  // Retired reporter placeholders have no process identity. Remove their
  // legacy bookkeeping rather than resurrecting a configurable exclusion.
  if (m_whitelist.contains(QStringLiteral("unknown vulkan game"))) {
    qInfo() << "Dropping placeholder whitelist entry for 'unknown vulkan "
               "game' - null-name reporters carry no game identity";
    m_whitelist.remove(QStringLiteral("unknown vulkan game"));
  }
  m_blacklist.removeAll(QStringLiteral("unknown vulkan game"));
  m_removedDefaults.removeAll(QStringLiteral("unknown vulkan game"));
  m_customKeys.removeAll(QStringLiteral("unknown vulkan game"));

  qDebug() << "Loaded" << m_whitelist.size() << "whitelisted and"
          << m_blacklist.size() << "blacklisted entries";
}

void GameWhitelist::save() const {
  QString filePath = configFilePath();

  // Ensure directory exists
  QDir dir = QFileInfo(filePath).dir();
  if (!dir.exists()) {
    dir.mkpath(".");
  }

  QJsonObject root;
  root["defaultPolicyVersion"] = 2;

  // Save whitelist
  QJsonObject whitelistObj;
  for (auto it = m_whitelist.begin(); it != m_whitelist.end(); ++it) {
    whitelistObj[it.key()] = it.value();
  }
  root["whitelist"] = whitelistObj;

  // Save blacklist
  QJsonArray blacklistArr;
  for (const QString &entry : m_blacklist) {
    blacklistArr.append(entry);
  }
  root["blacklist"] = blacklistArr;
  QJsonArray removedArr;
  for (const QString &entry : m_removedDefaults) {
    removedArr.append(entry);
  }
  root["removedDefaults"] = removedArr;
  QJsonArray customArr;
  for (const QString &entry : m_customKeys) {
    customArr.append(entry);
  }
  root["customKeys"] = customArr;

  QJsonDocument doc(root);

  QFile file(filePath);
  if (!file.open(QIODevice::WriteOnly)) {
    qWarning() << "Failed to save whitelist:" << file.errorString();
    return;
  }

  file.write(doc.toJson(QJsonDocument::Indented));
  file.close();

  qInfo() << "Saved whitelist to" << filePath;
}

void GameWhitelist::addToWhitelist(const QString &windowClass,
                                   const QString &displayName) {
  const QString key = normalizeKey(windowClass);
  if (key.isEmpty() || isPlaceholderReporterKey(key))
    return;
  QString name = displayName.trimmed().isEmpty() ? key : displayName.trimmed();
  m_whitelist[key] = name;
  // Lists are independent now: a block veto and a listing coexist, the
  // veto winning wherever detection reads them. Poles only add/remove
  // their own side; the x button clears both.
}

void GameWhitelist::removeFromWhitelist(const QString &windowClass) {
  m_whitelist.remove(normalizeKey(windowClass));
}

bool GameWhitelist::isWhitelisted(const QString &windowClass) const {
  return m_whitelist.contains(normalizeKey(windowClass));
}

QString GameWhitelist::getWhitelistName(const QString &windowClass) const {
  return m_whitelist.value(normalizeKey(windowClass));
}

QStringList GameWhitelist::whitelistEntries() const {
  return m_whitelist.keys();
}

void GameWhitelist::addToBlacklist(const QString &windowClass) {
  const QString key = defaultExclusionKey(normalizeKey(windowClass));
  if (key.isEmpty())
    return;
  if (!m_blacklist.contains(key)) {
    m_blacklist.append(key);
  }
  // Re-listing a built-in clears its removal marker.
  m_removedDefaults.removeAll(key);
}

void GameWhitelist::noteCustomKey(const QString &key) {
  const QString k = normalizeKey(key);
  if (!k.isEmpty() && !m_customKeys.contains(k))
    m_customKeys.append(k);
}

void GameWhitelist::forgetCustomKey(const QString &key) {
  m_customKeys.removeAll(normalizeKey(key));
}

void GameWhitelist::removeFromBlacklist(const QString &windowClass) {
  const QString key = defaultExclusionKey(normalizeKey(windowClass));
  m_blacklist.removeAll(key);
  // Explicitly dropping a built-in must survive restarts (the
  // constructor re-seeds defaults every launch).
  if (defaultBlacklistEntries().contains(key) &&
      !m_removedDefaults.contains(key)) {
    m_removedDefaults.append(key);
  }
}

bool GameWhitelist::isBlacklisted(const QString &windowClass) const {
  return m_blacklist.contains(defaultExclusionKey(normalizeKey(windowClass)));
}

QStringList GameWhitelist::blacklistEntries() const { return m_blacklist; }

} // namespace TrophyBackend
