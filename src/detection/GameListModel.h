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
#include <QVariantMap>
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
  QString section;      // launcher, Custom Games or Default Exclusions
  QString library;      // presentation provenance only; never capture approval
  QString blockKey;     // exact executable veto when metadata lacks a Wine prefix
  bool supportTool = false; // runtime, installer or support helper, never a game
  bool operator==(const GameListEntry &) const = default;
};

/**
 * @brief Dumb QML-facing list model; GameDetectionService owns the
 * instances and rebuilds their content whenever detection state or the
 * user lists change. Stable ordered identities update in place so state-only
 * changes preserve delegates and scrolling. Membership/order changes reset.
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
    LibraryRole,
    BlockKeyRole,
    SupportToolRole,
  };
  Q_ENUM(Roles)

  explicit GameListModel(QObject *parent = nullptr) : QAbstractListModel(parent) {}

  void setEntries(QList<GameListEntry> entries) {
    bool sameKeys = entries.size() == m_entries.size();
    for (qsizetype i = 0; sameKeys && i < entries.size(); ++i)
      sameKeys = entries.at(i).key == m_entries.at(i).key;
    if (sameKeys) {
      QList<QPair<int, QList<int>>> changedRows;
      for (qsizetype i = 0; i < entries.size(); ++i) {
        const auto &before = m_entries.at(i);
        const auto &after = entries.at(i);
        QList<int> roles;
        if (before.display != after.display)
          roles << DisplayRole << Qt::DisplayRole;
        if (before.pid != after.pid) roles << PidRole;
        if (before.running != after.running) roles << RunningRole;
        if (before.steamAppId != after.steamAppId)
          roles << SteamAppIdRole << IconSourceRole;
        if (before.listed != after.listed) roles << ListedRole;
        if (before.isDefault != after.isDefault)
          roles << DefaultRole << IdentifiersRole;
        if (before.hasToken != after.hasToken) roles << HasTokenRole;
        if (before.tokenKey != after.tokenKey) roles << TokenKeyRole;
        if (before.section != after.section) roles << SectionRole;
        if (before.library != after.library) roles << LibraryRole;
        if (before.blockKey != after.blockKey) roles << BlockKeyRole;
        if (before.supportTool != after.supportTool) roles << SupportToolRole;
        if (!roles.isEmpty())
          changedRows.append({static_cast<int>(i), roles});
      }
      m_entries = std::move(entries);
      // Section changes force a ListView relayout/fixup even when its geometry
      // did not change. Do not announce one for ordinary PID/state refreshes.
      for (const auto &[row, roles] : changedRows)
        emit dataChanged(index(row), index(row), roles);
      return;
    }
    beginResetModel();
    m_entries = std::move(entries);
    endResetModel();
  }

  int rowCount(const QModelIndex &parent = QModelIndex()) const override {
    return parent.isValid() ? 0 : static_cast<int>(m_entries.size());
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
    case LibraryRole:
      return e.library;
    case BlockKeyRole:
      return e.blockKey;
    case SupportToolRole:
      return e.supportTool;
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
        {LibraryRole, "library"},
        {BlockKeyRole, "blockKey"},
        {SupportToolRole, "supportTool"},
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
  Q_PROPERTY(QStringList collapsedSections READ collapsedSections WRITE setCollapsedSections
                 NOTIFY collapsedSectionsChanged)
  Q_PROPERTY(QVariantMap sectionCounts READ sectionCounts NOTIFY sectionCountsChanged)
  Q_PROPERTY(int matchingGameCount READ matchingGameCount NOTIFY sectionCountsChanged)

public:
  explicit GameListFilterModel(QObject *parent = nullptr)
      : QSortFilterProxyModel(parent) {
    setFilterCaseSensitivity(Qt::CaseInsensitive);
  }

  void setSourceModel(QAbstractItemModel *model) override {
    for (const auto &connection : m_sourceConnections)
      disconnect(connection);
    m_sourceConnections.clear();
    m_matchesDirty = true;
    QSortFilterProxyModel::setSourceModel(model);
    if (model) {
      const auto refresh = [this] {
        m_matchesDirty = true;
        invalidateFilter();
        emit sectionCountsChanged();
      };
      m_sourceConnections = {
          connect(model, &QAbstractItemModel::dataChanged, this, refresh),
          connect(model, &QAbstractItemModel::modelReset, this, refresh),
          connect(model, &QAbstractItemModel::rowsInserted, this, refresh),
          connect(model, &QAbstractItemModel::rowsRemoved, this, refresh),
          connect(model, &QAbstractItemModel::rowsMoved, this, refresh),
          connect(model, &QAbstractItemModel::layoutChanged, this, refresh)};
    }
    emit sectionCountsChanged();
  }

  QString filterText() const { return m_text; }
  Q_INVOKABLE int sectionCount(const QString &section) const {
    ensureMatches();
    return m_sectionCounts.value(section).toInt();
  }
  QVariantMap sectionCounts() const {
    ensureMatches();
    return m_sectionCounts;
  }
  int matchingGameCount() const {
    ensureMatches();
    return m_matchingGameCount;
  }
  QStringList collapsedSections() const { return m_collapsedSections; }
  void setCollapsedSections(const QStringList &sections) {
    if (sections == m_collapsedSections)
      return;
    m_collapsedSections = sections;
    invalidateFilter();
    emit collapsedSectionsChanged();
  }
  Q_INVOKABLE void toggleSection(const QString &section) {
    auto sections = m_collapsedSections;
    if (!sections.removeAll(section))
      sections.append(section);
    setCollapsedSections(sections);
  }
  void setFilterText(const QString &text) {
    if (m_text == text)
      return;
    m_text = text;
    m_matchesDirty = true;
    invalidateFilter();
    emit filterTextChanged();
    emit sectionCountsChanged();
  }

signals:
  void filterTextChanged();
  void showDefaultEntriesChanged();
  void collapsedSectionsChanged();
  void sectionCountsChanged();

public:
  bool showDefaultEntries() const { return m_showDefault; }
  void setShowDefaultEntries(bool show) {
    if (m_showDefault == show)
      return;
    m_showDefault = show;
    m_matchesDirty = true;
    invalidateFilter();
    emit showDefaultEntriesChanged();
    emit sectionCountsChanged();
  }

protected:
  bool filterAcceptsRow(int row, const QModelIndex &parent) const override {
    if (!matchesRow(row, parent))
      return false;
    const auto section = sourceModel()->index(row, 0, parent)
                             .data(GameListModel::SectionRole).toString();
    if (!m_collapsedSections.contains(section))
      return true;
    // Keep one invisible representative so ListView retains the heading.
    // Filtering out the other rows preserves virtualization for large libraries.
    ensureMatches();
    return m_firstMatches.value(section, -1) == row;
  }
  bool matchesRow(int row, const QModelIndex &parent = {}) const {
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
  void ensureMatches() const {
    if (!m_matchesDirty)
      return;
    m_matchesDirty = false;
    m_sectionCounts.clear();
    m_firstMatches.clear();
    m_matchingGameCount = 0;
    if (!sourceModel())
      return;
    for (int row = 0; row < sourceModel()->rowCount(); ++row) {
      if (!matchesRow(row))
        continue;
      const auto section = sourceModel()->index(row, 0)
                               .data(GameListModel::SectionRole).toString();
      if (!m_firstMatches.contains(section))
        m_firstMatches.insert(section, row);
      m_sectionCounts[section] = m_sectionCounts.value(section).toInt() + 1;
      ++m_matchingGameCount;
    }
  }
  QString m_text;
  bool m_showDefault = true;
  QStringList m_collapsedSections;
  QList<QMetaObject::Connection> m_sourceConnections;
  mutable bool m_matchesDirty = true;
  mutable QVariantMap m_sectionCounts;
  mutable QMap<QString, int> m_firstMatches;
  mutable int m_matchingGameCount = 0;
};

} // namespace TrophyBackend
