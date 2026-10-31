/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QList>
#include <QMap>
#include <QString>
#include <QtGlobal>

namespace TrophyBackend {

/** One mapped toplevel as exposed by ext-foreign-toplevel-list-v1. */
struct ToplevelInfo {
  QString identifier;
  QString appId;
  QString title;

  bool operator==(const ToplevelInfo &other) const = default;
};

/**
 * @brief Event-ordered toplevel state, independent of Wayland objects.
 *
 * The protocol sends identifier/app_id/title followed by `done`. This model
 * owns that ordering (and the handle->info map) so the Wayland wrapper stays
 * a thin adapter and the tricky cases are unit-testable without a compositor.
 */
class ToplevelModel {
public:
  enum class Change { None, Opened, Updated, Closed };

  /** Register a new toplevel handle; returns the opaque handle id. */
  quintptr add();

  bool contains(quintptr handle) const;
  int size() const;
  ToplevelInfo info(quintptr handle) const;

  Change setIdentifier(quintptr handle, const QString &identifier);
  Change setAppId(quintptr handle, const QString &appId);
  Change setTitle(quintptr handle, const QString &title);

  /** Apply the atomic `done` batch. Ignored until an identifier exists. */
  Change commit(quintptr handle);

  /** Apply `closed`; reports whether the toplevel had been announced. */
  Change remove(quintptr handle);

  /** Announced toplevels, sorted by identifier for stable UI/test output. */
  QList<ToplevelInfo> snapshot() const;

private:
  struct Record {
    ToplevelInfo info;
    bool announced = false;
    bool dirty = false;
  };
  QMap<quintptr, Record> m_records;
  quintptr m_nextHandle = 1;
};

} // namespace TrophyBackend
