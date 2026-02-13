/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GameWhitelist.h"

#include <QDebug>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>

namespace GameRecorder {

GameWhitelist::GameWhitelist() = default;

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
    qInfo() << "No whitelist file found, using empty lists";
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

  // Load whitelist
  m_whitelist.clear();
  QJsonObject whitelistObj = root["whitelist"].toObject();
  for (auto it = whitelistObj.begin(); it != whitelistObj.end(); ++it) {
    m_whitelist[it.key()] = it.value().toString();
  }

  // Load blacklist
  m_blacklist.clear();
  QJsonArray blacklistArr = root["blacklist"].toArray();
  for (const QJsonValue &val : blacklistArr) {
    m_blacklist.append(val.toString());
  }

  qInfo() << "Loaded" << m_whitelist.size() << "whitelisted and"
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
  QString name = displayName.isEmpty() ? windowClass : displayName;
  m_whitelist[windowClass] = name;

  // Remove from blacklist if present
  m_blacklist.removeAll(windowClass);
}

void GameWhitelist::removeFromWhitelist(const QString &windowClass) {
  m_whitelist.remove(windowClass);
}

bool GameWhitelist::isWhitelisted(const QString &windowClass) const {
  return m_whitelist.contains(windowClass);
}

QString GameWhitelist::getWhitelistName(const QString &windowClass) const {
  return m_whitelist.value(windowClass);
}

QStringList GameWhitelist::whitelistEntries() const {
  return m_whitelist.keys();
}

void GameWhitelist::addToBlacklist(const QString &windowClass) {
  if (!m_blacklist.contains(windowClass)) {
    m_blacklist.append(windowClass);
  }

  // Remove from whitelist if present
  m_whitelist.remove(windowClass);
}

void GameWhitelist::removeFromBlacklist(const QString &windowClass) {
  m_blacklist.removeAll(windowClass);
}

bool GameWhitelist::isBlacklisted(const QString &windowClass) const {
  return m_blacklist.contains(windowClass);
}

QStringList GameWhitelist::blacklistEntries() const { return m_blacklist; }

} // namespace GameRecorder
