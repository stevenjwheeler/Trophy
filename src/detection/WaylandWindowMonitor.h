/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QTimer>

namespace GameRecorder {

/**
 * @brief Monitors Wayland window/application activity
 *
 * On Wayland, direct window monitoring isn't available, so we use
 * compositor-specific methods or fall back to process monitoring.
 */
class WaylandWindowMonitor : public QObject {
  Q_OBJECT

public:
  explicit WaylandWindowMonitor(QObject *parent = nullptr);
  ~WaylandWindowMonitor() override;

  void startMonitoring();
  void stopMonitoring();

  QString focusedAppId() const;

signals:
  void focusedAppChanged(const QString &appId);

private slots:
  void checkFocusedApp();

private:
  void setupCompositorTracking();
  QString detectCompositor() const;

  QTimer *m_pollTimer = nullptr;
  QString m_focusedAppId;
  bool m_monitoring = false;
};

} // namespace GameRecorder
