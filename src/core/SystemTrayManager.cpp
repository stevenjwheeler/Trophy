/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SystemTrayManager.h"

#include <QAction>
#include <QIcon>
#include <QMenu>
#include <QSystemTrayIcon>

namespace TrophyBackend {

namespace {
const char *kIdleIcon = ":/icons/trophy.svg";
const char *kRecordingIcon = ":/icons/record.svg";

QString formatDuration(qint64 ms) {
  if (ms <= 0) {
    return QStringLiteral("0:00");
  }
  const qint64 totalSecs = ms / 1000;
  const qint64 h = totalSecs / 3600;
  const qint64 m = (totalSecs % 3600) / 60;
  const qint64 s = totalSecs % 60;
  if (h > 0) {
    return QStringLiteral("%1:%2:%3")
        .arg(h)
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'));
  }
  return QStringLiteral("%1:%2").arg(m).arg(s, 2, 10, QLatin1Char('0'));
}

QString formatSize(qint64 bytes) {
  if (bytes <= 0) {
    return QStringLiteral("0 B");
  }
  constexpr qint64 KB = 1024;
  constexpr qint64 MB = KB * 1024;
  constexpr qint64 GB = MB * 1024;
  if (bytes >= GB) {
    return QStringLiteral("%1 GB").arg(bytes / double(GB), 0, 'f', 1);
  }
  if (bytes >= MB) {
    return QStringLiteral("%1 MB").arg(bytes / double(MB), 0, 'f', 1);
  }
  if (bytes >= KB) {
    return QStringLiteral("%1 KB").arg(bytes / double(KB), 0, 'f', 0);
  }
  return QStringLiteral("%1 B").arg(bytes);
}
} // namespace

SystemTrayManager::SystemTrayManager(QObject *parent) : QObject(parent) {
  m_tray = new QSystemTrayIcon(QIcon(QString::fromLatin1(kIdleIcon)), this);
  m_menu = new QMenu(nullptr);

  m_toggleWindowAction = m_menu->addAction(tr("Hide Trophy"), this,
                                           &SystemTrayManager::onToggleWindow);
  m_recordAction = m_menu->addAction(tr("Start Recording"), this,
                                     &SystemTrayManager::onToggleRecording);
  m_replayAction =
      m_menu->addAction(tr("Save Clip"), this, [this]() {
        emit saveReplayRequested();
      });
  m_menu->addSeparator();
  m_quitAction = m_menu->addAction(tr("Quit"), this, [this]() {
    emit quitRequested();
  });

  m_tray->setContextMenu(m_menu);
  m_tray->setToolTip(tr("Trophy"));

  connect(m_tray, &QSystemTrayIcon::activated, this,
          &SystemTrayManager::onTrayActivated);
  refresh();
}

SystemTrayManager::~SystemTrayManager() {
  if (m_tray) {
    m_tray->hide();
  }
  delete m_menu;
  m_menu = nullptr;
}

bool SystemTrayManager::isAvailable() const {
  return QSystemTrayIcon::isSystemTrayAvailable();
}

void SystemTrayManager::setTrayVisible(bool visible) {
  if (!m_tray) {
    return;
  }
  if (visible && !isAvailable()) {
    return;
  }
  m_tray->setVisible(visible);
}

void SystemTrayManager::setRecording(bool recording, const QString &gameName) {
  if (m_isRecording == recording && m_gameName == gameName) {
    return;
  }
  m_isRecording = recording;
  m_gameName = gameName;
  if (!recording) {
    m_durationMs = 0;
    m_bytes = 0;
  }
  refresh();
}

void SystemTrayManager::setWindowVisible(bool visible) {
  if (m_windowVisible == visible) {
    return;
  }
  m_windowVisible = visible;
  refresh();
}

void SystemTrayManager::setProgress(qint64 durationMs, qint64 bytes) {
  if (!m_isRecording) {
    return;
  }
  if (m_durationMs == durationMs && m_bytes == bytes) {
    return;
  }
  m_durationMs = durationMs;
  m_bytes = bytes;
  refresh();
}

QString SystemTrayManager::recordActionText() const {
  return m_recordAction ? m_recordAction->text() : QString();
}

QString SystemTrayManager::toggleWindowActionText() const {
  return m_toggleWindowAction ? m_toggleWindowAction->text() : QString();
}

QString SystemTrayManager::toolTipText() const {
  return m_tray ? m_tray->toolTip() : QString();
}

void SystemTrayManager::triggerRecordAction() {
  if (m_recordAction) {
    m_recordAction->trigger();
  }
}

void SystemTrayManager::triggerReplayAction() {
  if (m_replayAction) {
    m_replayAction->trigger();
  }
}

void SystemTrayManager::triggerToggleWindowAction() {
  if (m_toggleWindowAction) {
    m_toggleWindowAction->trigger();
  }
}

void SystemTrayManager::triggerQuitAction() {
  if (m_quitAction) {
    m_quitAction->trigger();
  }
}

void SystemTrayManager::onTrayActivated() {
  // Left-click toggles the main window; right-click opens the menu
  // natively via the tray host (KDE/GNOME extension).
  emit showWindowRequested();
}

void SystemTrayManager::onToggleWindow() {
  if (m_windowVisible) {
    emit hideWindowRequested();
  } else {
    emit showWindowRequested();
  }
}

void SystemTrayManager::onToggleRecording() {
  if (m_isRecording) {
    emit stopRecordingRequested();
  } else {
    emit startRecordingRequested();
  }
}

void SystemTrayManager::refresh() {
  if (!m_tray) {
    return;
  }
  m_tray->setIcon(QIcon(QString::fromLatin1(m_isRecording ? kRecordingIcon
                                                          : kIdleIcon)));
  if (m_isRecording) {
    const QString base = m_gameName.isEmpty()
                             ? tr("Trophy — Recording")
                             : tr("Trophy — Recording %1").arg(m_gameName);
    if (m_durationMs > 0 || m_bytes > 0) {
      m_tray->setToolTip(QStringLiteral("%1 • %2 • %3")
                             .arg(base, formatDuration(m_durationMs),
                                  formatSize(m_bytes)));
    } else {
      m_tray->setToolTip(base);
    }
  } else {
    m_tray->setToolTip(tr("Trophy"));
  }
  if (m_toggleWindowAction) {
    m_toggleWindowAction->setText(m_windowVisible ? tr("Hide Trophy")
                                                  : tr("Show Trophy"));
  }
  if (m_recordAction) {
    m_recordAction->setText(m_isRecording ? tr("Stop Recording")
                                          : tr("Start Recording"));
  }
}

} // namespace TrophyBackend
