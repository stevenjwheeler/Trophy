/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "WindowInfo.h"

#include <QList>
#include <QMutex>
#include <QObject>

namespace TrophyBackend {

/**
 * @brief GNOME Shell window-tracking backend for WaylandWindowMonitor.
 *
 * Polls `org.gnome.Shell.Eval` once per second with a defensive script
 * that snapshots all windows as JSON: [{id, appId, title, pid,
 * fullscreen, focused}, ...]. Window ids are MetaWindow stable sequence
 * numbers (unique per session). No extension, no installation, nothing
 * written anywhere — pure D-Bus reads.
 *
 * GNOME only grants Eval to the session user; if the shell restricts it,
 * refresh() fails softly and the backend stays inactive. The static
 * parseEvalResult() is pure and unit-tested with canned replies.
 */
class GnomeBackend : public QObject {
  Q_OBJECT

public:
  static constexpr const char *kService = "org.gnome.Shell";
  static constexpr const char *kPath = "/org/gnome/Shell";
  static constexpr const char *kInterface = "org.gnome.Shell";

  explicit GnomeBackend(QObject *parent = nullptr);
  ~GnomeBackend() override = default;

  /** True when the shell answers Eval at all (checked on first refresh). */
  bool start();
  void stop() { m_active = false; }
  bool isActive() const { return m_active; }

  /** Single Eval round-trip; called by the monitor's 1s watchdog tick. */
  void refresh();

  QList<WindowInfo> windows() const;

  /** Parse an Eval result string into window infos. Never throws. */
  static QList<WindowInfo> parseEvalResult(const QString &result);

  /** Stable content signature used to skip unchanged snapshots. */
  static QString signatureFor(const QList<WindowInfo> &windows);

signals:
  void windowsUpdated();

private:
  // Defensive script: every accessor guarded, invisible helper surfaces
  // skipped, always returns a JSON array string (or "null" on failure).
  static QString snapshotScript();

  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  QString m_lastSignature; // content hash of the last emitted snapshot
  bool m_active = false;
  bool m_probed = false;
};

} // namespace TrophyBackend
