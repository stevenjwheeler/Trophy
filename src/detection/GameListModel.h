/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QAbstractListModel>
#include <QList>
#include <QSortFilterProxyModel>
#include <QString>
#include <QUrl>
#include "DefaultAppCatalog.h"
#include "GameIconSource.h"

namespace TrophyBackend {

/**
 * @brief One row in a game list (detected / whitelist / blacklist tabs).
 */
struct GameListEntry {
  QString key;      // match key: reporter class or steam_app_<id>
  QString display;  // user-facing name
  qint64 pid = 0;   // live PID when running, else 0
  bool running = false;
  uint steamAppId = 0; // parsed from steam_app_<id> keys, else 0
  int listed = 0;       // 0 = none, 1 = whitelisted, 2 = blacklisted
  bool isDefault = false; // built-in exclusion, hidden unless opted in
  bool hasToken = false; // portal restore token held: silent capture, no
                         // prompt. This is the only "approved" state.
  QString tokenKey;      // exact settings-map key for withdrawal
  QString section;      // list grouping: Now playing / Ready to capture
                        // / Apps
};

/**
 * @brief Dumb QML-facing list model; GameDetectionService owns the
 * instances and rebuilds their content whenever detection state or the
 * user lists change. Lists are tiny (tens of rows), so full resets are
 * cheaper than diffing and keep QML bindings trivial.
 */
class GameListModel : public QAbstractListModel {
  Q_OBJECT

public:
  enum Roles {
    KeyRole = Qt::UserRole + 1,
    DisplayRole,
    PidRole,
    RunningRole,
    SteamAppIdRole,
    ListedRole,
    DefaultRole,
    HasTokenRole,
    TokenKeyRole,
    SectionRole,
    IconSourceRole,
    IdentifiersRole,
  };
  Q_ENUM(Roles)

  explicit GameListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

  void setEntries(QList<GameListEntry> entries) {
    beginResetModel();
    m_entries = std::move(entries);
    endResetModel();
  }

  int rowCount(const QModelIndex &parent = QModelIndex()) const override {
    return parent.isValid() ? 0 : m_entries.size();
  }

  QVariant data(const QModelIndex &index, int role) const override {
    if (!index.isValid() || index.row() < 0 ||
        index.row() >= m_entries.size())
      return {};
    const GameListEntry &e = m_entries.at(index.row());
    switch (role) {
    case KeyRole:
      return e.key;
    case DisplayRole:
    case Qt::DisplayRole:
      return e.display;
    case PidRole:
      return e.pid;
    case RunningRole:
      return e.running;
    case SteamAppIdRole:
      return e.steamAppId;
    case ListedRole:
      return e.listed;
    case DefaultRole:
      return e.isDefault;
    case HasTokenRole:
      return e.hasToken;
    case TokenKeyRole:
      return e.tokenKey;
    case SectionRole:
      return e.section;
    case IconSourceRole:
      return gameIconSource(e.key, e.steamAppId);
    case IdentifiersRole:
      if (e.isDefault) {
        if (const auto *app = defaultAppInfo(e.key))
          return QStringList{app->key} + app->aliases;
      }
      return QStringList{};
    default:
      return {};
    }
  }

  QHash<int, QByteArray> roleNames() const override {
    return {
        {KeyRole, "key"},
        {DisplayRole, "display"},
        {PidRole, "pid"},
        {RunningRole, "running"},
        {SteamAppIdRole, "steamAppId"},
        {ListedRole, "listed"},
        {DefaultRole, "isDefault"},
        {HasTokenRole, "hasToken"},
        {TokenKeyRole, "tokenKey"},
        {SectionRole, "section"},
        {IconSourceRole, "iconSource"},
        {IdentifiersRole, "identifiers"},
    };
  }

private:
  QList<GameListEntry> m_entries;
};

/**
 * @brief Case-insensitive substring filter over display, key and default aliases.
 * One instance fronts each GameListModel so the page search field filters
 * all tabs through a single service-level filter string.
 */
class GameListFilterModel : public QSortFilterProxyModel {
  Q_OBJECT
  Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY
                 filterTextChanged)
  Q_PROPERTY(bool showDefaultEntries READ showDefaultEntries WRITE
                 setShowDefaultEntries NOTIFY showDefaultEntriesChanged)

public:
  explicit GameListFilterModel(QObject *parent = nullptr)
      : QSortFilterProxyModel(parent) {
    setFilterCaseSensitivity(Qt::CaseInsensitive);
  }

  QString filterText() const { return m_text; }
  void setFilterText(const QString &text) {
    if (m_text == text)
      return;
    m_text = text;
    invalidateFilter();
    emit filterTextChanged();
  }

signals:
  void filterTextChanged();
  void showDefaultEntriesChanged();

public:
  bool showDefaultEntries() const { return m_showDefault; }
  void setShowDefaultEntries(bool show) {
    if (m_showDefault == show)
      return;
    m_showDefault = show;
    invalidateFilter();
    emit showDefaultEntriesChanged();
  }

protected:
  bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
    const QModelIndex idx = sourceModel()->index(row, 0, parent);
    if (!m_showDefault &&
        sourceModel()->data(idx, GameListModel::DefaultRole).toBool())
      return false;
    if (m_text.isEmpty())
      return true;
    const QString display =
        sourceModel()->data(idx, GameListModel::DisplayRole).toString();
    const QString key =
        sourceModel()->data(idx, GameListModel::KeyRole).toString();
    const QString aliases = sourceModel()
                                ->data(idx, GameListModel::IdentifiersRole)
                                .toStringList().join(' ');
    return display.contains(m_text, Qt::CaseInsensitive) ||
           key.contains(m_text, Qt::CaseInsensitive) ||
           aliases.contains(m_text, Qt::CaseInsensitive);
  }

private:
  QString m_text;
  bool m_showDefault = true;
};

} // namespace TrophyBackend
