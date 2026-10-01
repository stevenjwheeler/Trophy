/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "library/ActivityFeed.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSaveFile>
#include <QStandardPaths>

namespace TrophyBackend {

ActivityFeed::ActivityFeed(QObject *parent, const QString &filePath)
    : QObject(parent),
      m_filePath(filePath.isEmpty() ? defaultFilePath() : filePath) {
  load();
}

QString ActivityFeed::defaultFilePath() {
  return QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) +
         "/trophy/activity.json";
}

QVariantList ActivityFeed::events() const {
  QVariantList out;
  out.reserve(m_events.size());
  for (const Event &e : m_events)
    out.append(toMap(e));
  return out;
}

QVariantMap ActivityFeed::get(int index) const {
  if (index < 0 || index >= m_events.size())
    return {};
  return toMap(m_events.at(index));
}

void ActivityFeed::addEvent(const QString &kind, const QString &dot,
                            const QString &title, const QString &detail,
                            const QString &path) {
  addEventAt(QDateTime::currentDateTime(), kind, dot, title, detail, path);
}

void ActivityFeed::addEventAt(const QDateTime &at, const QString &kind,
                              const QString &dot, const QString &title,
                              const QString &detail, const QString &path) {
  Event e{at.isValid() ? at : QDateTime::currentDateTime(), kind, dot, title,
          detail, path};
  m_events.prepend(e);
  save();
  emit countChanged();
  emit eventsChanged();
}

void ActivityFeed::clear() {
  if (m_events.isEmpty())
    return;
  m_events.clear();
  save();
  emit countChanged();
  emit eventsChanged();
}

void ActivityFeed::load() {
  QFile file(m_filePath);
  if (!file.exists())
    return;
  if (!file.open(QIODevice::ReadOnly))
    return;
  const QByteArray data = file.readAll();
  file.close();

  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
  if (err.error != QJsonParseError::NoError || !doc.isObject())
    return; // corrupt file: start empty rather than crashing
  const QJsonArray arr = doc.object()["events"].toArray();
  for (const QJsonValue &v : arr) {
    if (!v.isObject())
      continue;
    Event e;
    if (fromMap(v.toObject().toVariantMap(), &e))
      m_events.append(e);
  }
}

void ActivityFeed::save() const {
  QDir().mkpath(QFileInfo(m_filePath).absolutePath());
  QSaveFile file(m_filePath);
  if (!file.open(QIODevice::WriteOnly))
    return;
  QJsonArray arr;
  for (const Event &e : m_events)
    arr.append(QJsonObject::fromVariantMap(toMap(e)));
  QJsonObject root;
  root["version"] = 1;
  root["events"] = arr;
  file.write(QJsonDocument(root).toJson(QJsonDocument::Compact));
  file.commit();
}

QVariantMap ActivityFeed::toMap(const Event &e) {
  return {{"at", e.at.toString(Qt::ISODateWithMs)},
          {"kind", e.kind},
          {"dot", e.dot},
          {"title", e.title},
          {"detail", e.detail},
          {"path", e.path}};
}

bool ActivityFeed::fromMap(const QVariantMap &m, Event *out) {
  const QDateTime at =
      QDateTime::fromString(m.value("at").toString(), Qt::ISODateWithMs);
  if (!at.isValid() || !out)
    return false;
  out->at = at;
  out->kind = m.value("kind").toString();
  out->dot = m.value("dot").toString();
  out->title = m.value("title").toString();
  out->detail = m.value("detail").toString();
  // Pre-path entries simply carry no path.
  out->path = m.value("path").toString();
  return true;
}

} // namespace TrophyBackend
