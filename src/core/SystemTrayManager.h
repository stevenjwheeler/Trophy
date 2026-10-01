/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

class QSystemTrayIcon;
class QMenu;
class QAction;

namespace TrophyBackend {

/**
 * @brief System tray icon with recording-state icon and context menu.
 *
 * Idle shows the Trophy icon; while recording it switches to a record
 * indicator and the tooltip names the game. All actions are signals so
 * Application wires them to the recording pipeline and main window.
 * When no tray host exists (isAvailable() == false) the icon stays
 * hidden and callers fall back to plain window behaviour.
 */
class SystemTrayManager : public QObject {
  Q_OBJECT

public:
  explicit SystemTrayManager(QObject *parent = nullptr);
  ~SystemTrayManager() override;

  bool isAvailable() const;

  /** Show or hide the tray icon (hidden when no host exists). */
  void setTrayVisible(bool visible);

  // Observable state (used by tests and future shortcuts)
  QString recordActionText() const;
  QString toggleWindowActionText() const;
  QString toolTipText() const;
  void triggerRecordAction();
  void triggerReplayAction();
  void triggerToggleWindowAction();
  void triggerQuitAction();

public slots:
  void setRecording(bool recording, const QString &gameName = QString());
  void setWindowVisible(bool visible);
  // Live counters for the recording tooltip (duration in ms, size in bytes).
  void setProgress(qint64 durationMs, qint64 bytes);

signals:
  void showWindowRequested();
  void hideWindowRequested();
  void startRecordingRequested();
  void stopRecordingRequested();
  void saveReplayRequested();
  void openRecordingsRequested();
  void quitRequested();

private slots:
  void onTrayActivated();
  void onToggleWindow();
  void onToggleRecording();

private:
  void refresh();

  QSystemTrayIcon *m_tray = nullptr;
  QMenu *m_menu = nullptr;
  QAction *m_toggleWindowAction = nullptr;
  QAction *m_recordAction = nullptr;
  QAction *m_replayAction = nullptr;
  QAction *m_openFolderAction = nullptr;
  QAction *m_quitAction = nullptr;

  bool m_isRecording = false;
  QString m_gameName;
  bool m_windowVisible = true;
  qint64 m_durationMs = 0;
  qint64 m_bytes = 0;
};

} // namespace TrophyBackend
