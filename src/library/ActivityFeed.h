/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDateTime>
#include <QList>
#include <QObject>
#include <QString>
#include <QVariantList>
#include <QVariantMap>

namespace TrophyBackend {

/**
 * @brief Session-persistent timeline events for the dashboard activity feed.
 *
 * Library-derived rows (completed recordings) rebuild from disk every
 * launch, but transient events — clip saves, deletions, storage warnings —
 * would otherwise vanish on restart. This store keeps them in
 * `<ConfigLocation>/trophy/activity.json` (next to settings.json) as a
 * plain JSON array, newest first.
 *
 * Deliberately uncapped: entries are ~150 bytes each, so even 10k events
 * stay around 1.5 MB. If that ever becomes a problem, prune by age here
 * rather than reintroducing a count cap in QML.
 */
class ActivityFeed : public QObject {
  Q_OBJECT

  Q_PROPERTY(int count READ count NOTIFY countChanged)
  Q_PROPERTY(QVariantList events READ events NOTIFY eventsChanged)

public:
  struct Event {
    QDateTime at;
    QString kind;  // "clip" | "deleted" | "storage"
    QString dot;   // "red" | "green" | "blue" | "orange"
    QString title;
    QString detail;
    // Library file the event refers to (clip saves). Empty for the rest;
    // lets the feed enrich the row from the live library entry.
    QString path;
  };

  /**
   * @param filePath Override for tests. Empty means the default location
   * next to settings.json.
   */
  explicit ActivityFeed(QObject *parent = nullptr,
                        const QString &filePath = QString());
  ~ActivityFeed() override = default;

  static QString defaultFilePath();

  int count() const { return m_events.size(); }
  QVariantList events() const;
  Q_INVOKABLE QVariantMap get(int index) const;

  /** Stamp now, prepend, persist. */
  Q_INVOKABLE void addEvent(const QString &kind, const QString &dot,
                            const QString &title, const QString &detail,
                            const QString &path = QString());
  /** Explicit timestamp (tests / imports). */
  Q_INVOKABLE void addEventAt(const QDateTime &at, const QString &kind,
                              const QString &dot, const QString &title,
                              const QString &detail,
                              const QString &path = QString());

  Q_INVOKABLE void clear();

signals:
  void countChanged();
  void eventsChanged();

private:
  void load();
  void save() const;
  static QVariantMap toMap(const Event &e);
  static bool fromMap(const QVariantMap &m, Event *out);

  QString m_filePath;
  QList<Event> m_events; // newest first
};

} // namespace TrophyBackend
