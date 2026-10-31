/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ToplevelModel.h"

#include <algorithm>

namespace TrophyBackend {

quintptr ToplevelModel::add() {
  const quintptr handle = m_nextHandle++;
  m_records.insert(handle, Record{});
  return handle;
}

bool ToplevelModel::contains(quintptr handle) const {
  return m_records.contains(handle);
}

int ToplevelModel::size() const { return m_records.size(); }

ToplevelInfo ToplevelModel::info(quintptr handle) const {
  return m_records.value(handle).info;
}

ToplevelModel::Change ToplevelModel::setIdentifier(quintptr handle,
                                                   const QString &identifier) {
  auto it = m_records.find(handle);
  if (it == m_records.end())
    return Change::None;
  if (it->info.identifier == identifier)
    return Change::None;
  it->info.identifier = identifier;
  it->dirty = true;
  return Change::None;
}

ToplevelModel::Change ToplevelModel::setAppId(quintptr handle,
                                              const QString &appId) {
  auto it = m_records.find(handle);
  if (it == m_records.end())
    return Change::None;
  if (it->info.appId == appId)
    return Change::None;
  it->info.appId = appId;
  it->dirty = true;
  return Change::None;
}

ToplevelModel::Change ToplevelModel::setTitle(quintptr handle,
                                              const QString &title) {
  auto it = m_records.find(handle);
  if (it == m_records.end())
    return Change::None;
  if (it->info.title == title)
    return Change::None;
  it->info.title = title;
  it->dirty = true;
  return Change::None;
}

ToplevelModel::Change ToplevelModel::commit(quintptr handle) {
  auto it = m_records.find(handle);
  if (it == m_records.end())
    return Change::None;
  // Identifier is mandatory and always precedes `done` in the protocol.
  // Treat a missing one as "not ready" instead of publishing an anonymous
  // toplevel that could never be matched or closed safely.
  if (it->info.identifier.isEmpty())
    return Change::None;
  if (!it->announced) {
    it->announced = true;
    it->dirty = false;
    return Change::Opened;
  }
  if (!it->dirty)
    return Change::None;
  it->dirty = false;
  return Change::Updated;
}

ToplevelModel::Change ToplevelModel::remove(quintptr handle) {
  auto it = m_records.find(handle);
  if (it == m_records.end())
    return Change::None;
  const bool announced = it->announced;
  m_records.erase(it);
  return announced ? Change::Closed : Change::None;
}

QList<ToplevelInfo> ToplevelModel::snapshot() const {
  QList<ToplevelInfo> result;
  for (auto it = m_records.constBegin(); it != m_records.constEnd(); ++it) {
    if (it->announced)
      result.append(it->info);
  }
  std::sort(result.begin(), result.end(),
            [](const ToplevelInfo &a, const ToplevelInfo &b) {
              return a.identifier < b.identifier;
            });
  return result;
}

} // namespace TrophyBackend
