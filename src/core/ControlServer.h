/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>

class QLocalServer;

namespace TrophyBackend {

/**
 * @brief Local control socket for native compositor keybinds.
 *
 * Listens on $XDG_RUNTIME_DIR/trophy-control.sock. Native binds that the
 * portal can't reach (GNOME custom shortcuts, Sway/Hyprland bindsym) call
 * `trophy --toggle-record|--save-clip|--add-bookmark|--show-window`, which forwards the
 * command here when an instance is already running. Only single-line
 * ASCII commands are accepted; anything else is ignored. The runtime
 * dir is user-private (0700), so no other user can connect.
 */
class ControlServer : public QObject {
  Q_OBJECT

public:
  static QString defaultSocketPath();

  explicit ControlServer(QObject *parent = nullptr);
  explicit ControlServer(const QString &socketPath, QObject *parent = nullptr);
  ~ControlServer() override;

  bool isListening() const;
  bool listen();
  void close();

  // One-shot client used by second CLI invocations. socketPathOverride
  // exists for hermetic tests; production always uses the default.
  static bool sendCommand(const QString &command, int timeoutMs = 2000);
  static bool sendCommandTo(const QString &socketPath, const QString &command,
                            int timeoutMs = 2000);

signals:
  void toggleRecordingRequested();
  void saveClipRequested();
  void bookmarkRequested();
  void showWindowRequested();

private slots:
  void onNewConnection();

private:
  void handleLine(const QString &line);

  QString m_socketPath;
  QLocalServer *m_server = nullptr;
};

} // namespace TrophyBackend
