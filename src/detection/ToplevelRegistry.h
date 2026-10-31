/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include "ToplevelModel.h"

#include <QList>
#include <QObject>
#include <QString>
#include <QMetaType>
#include <memory>

namespace TrophyBackend {

/**
 * @brief One cross-compositor window list via ext-foreign-toplevel-list-v1.
 *
 * This is the portable half of window tracking: the protocol exposes every
 * mapped toplevel with app_id/title/identifier on KWin, Mutter, wlroots
 * compositors and others. It deliberately does not expose PID, focus or
 * fullscreen; those stay optional enrichment rather than a detection
 * dependency.
 *
 * Runs on the GUI thread through QSocketNotifier; callbacks only mutate the
 * ToplevelModel and emit signals, so no compositor round-trip happens on a
 * hot path after start().
 */
class ToplevelRegistry : public QObject {
  Q_OBJECT

public:
  class Private;

  explicit ToplevelRegistry(QObject *parent = nullptr);
  ~ToplevelRegistry() override;

  /** Compiled support and a usable Wayland session in the environment. */
  static bool isAvailable();

  /**
   * @brief Connect and bind the global.
   * @return false when the compositor does not expose the protocol; callers
   *         should keep their existing backend fallback in that case.
   */
  bool start(QString *error = nullptr);
  void stop();
  bool isActive() const;

  QList<ToplevelInfo> toplevels() const;

signals:
  void toplevelOpened(const TrophyBackend::ToplevelInfo &info);
  void toplevelUpdated(const TrophyBackend::ToplevelInfo &info);
  void toplevelClosed(const QString &identifier);
  void toplevelsChanged();
  void connectionLost(const QString &reason);

private:
  std::unique_ptr<Private> d;
};

} // namespace TrophyBackend

Q_DECLARE_METATYPE(TrophyBackend::ToplevelInfo)
