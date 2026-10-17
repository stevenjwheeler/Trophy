/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "WindowInfo.h"
#include <QList>
#include <QMutex>
#include <QObject>
#include <atomic>
#include <future>
#include <memory>

namespace TrophyBackend {

// Polls our narrowly scoped GNOME Shell extension, never Shell.Eval. Installation
// and enabling require explicit user consent through start(true). Native work
// runs on the monitor's owned polling worker; shutdown settles it before release.
class GnomeBackend : public QObject {
  Q_OBJECT
public:
  static constexpr const char *kService = "org.gnome.Shell";
  static constexpr const char *kPath = "/org/trophy/WindowHelper";
  static constexpr const char *kInterface = "org.trophy.WindowHelper1";
  static constexpr const char *kUuid = "trophy-monitor@trophy";

  explicit GnomeBackend(QObject *parent = nullptr);
  ~GnomeBackend() override;
  bool start(bool repair = false);
  void stop();
  bool isActive() const { return m_active.load(); }
  bool hasPendingWork() const { return m_setupPending->load(); }
  void refresh();
  QList<WindowInfo> windows() const;
  QString status() const;
  static QString extensionDirectory();
  static bool isInstalled();
  static QList<WindowInfo> parseSnapshot(const QString &result);
  // Compatibility for existing parser fixtures; no Eval calls are made.
  static QList<WindowInfo> parseEvalResult(const QString &result) { return parseSnapshot(result); }
  static QString signatureFor(const QList<WindowInfo> &windows);

signals:
  void windowsUpdated();

private:
  friend class GnomeBackendTest;
  friend class WindowMonitorWorkerTest;
  static QString installHelper(const QString &directory);
  void unavailable(const QString &reason, quint64 generation);
  mutable QMutex m_mutex;
  QList<WindowInfo> m_windows;
  QString m_lastSignature;
  QString m_status;
  std::atomic_bool m_active{false};
  std::atomic_bool m_running{false};
  std::atomic<quint64> m_generation{0};
  std::future<QString> m_setup;
  std::shared_ptr<std::atomic_bool> m_setupPending = std::make_shared<std::atomic_bool>(false);
};
} // namespace TrophyBackend
