/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WaylandWindowMonitor.h"
#include "GnomeBackend.h"
#include "HyprlandBackend.h"
#include "KWinBackend.h"
#include "SwayBackend.h"

#include <QDebug>
#include <QMutexLocker>
#include <QProcessEnvironment>
#include <QSet>
#include <future>

namespace TrophyBackend {

namespace {

// FNV-1a 64-bit over the key's UTF-8 bytes. Deterministic across
// processes and restarts (unlike qHash(QString), which is per-process
// seeded), so a key-derived synthetic id is stable for the window's
// lifetime and beyond.
quint64 stableKeyHash(const QString &key) {
  quint64 h = 14695981039346656037ULL;
  const QByteArray bytes = key.toUtf8();
  for (char b : bytes) {
    h ^= quint64(quint8(b));
    h *= 1099511628211ULL;
  }
  return h;
}

} // namespace

WaylandWindowMonitor::WaylandWindowMonitor(QObject *parent) : QObject(parent) {
  // Detect eagerly so UI can name the compositor before tracking starts.
  m_compositor = detectCompositor();
  m_pollTimer = new QTimer(this);
  m_healthTimer.setSingleShot(true);
  m_healthTimer.setInterval(30000);
  connect(&m_healthTimer, &QTimer::timeout, this, &WaylandWindowMonitor::windowsChanged);
  m_pollTimer->setInterval(1000);
  connect(m_pollTimer, &QTimer::timeout, this,
          &WaylandWindowMonitor::checkFocusedApp);
}

WaylandWindowMonitor::~WaylandWindowMonitor() {
  stopMonitoring();
  // Shutdown, unlike a settings toggle, settles all owned work before
  // releasing backend objects or input files.
  for (auto &retired : m_retired)
    if (retired.poll.valid()) retired.poll.wait();
  for (auto &removal : m_removals) removal.wait();
  m_retired.clear();
}

void WaylandWindowMonitor::startMonitoring(bool repairBackend) {
  if (m_monitoring) {
    return;
  }

  m_compositor = detectCompositor();
  ++m_generation;
  qDebug() << "Wayland compositor detected:" << m_compositor;

  setupCompositorTracking(repairBackend);

  m_monitoring = true;
  m_healthTimer.start();
  m_pollTimer->start();

  qDebug() << "Wayland window monitoring started (" << m_compositor << ")";
  // The KWin installed/enabled properties notify on windowsChanged, which
  // only fires on snapshots — re-read them now so status labels shown
  // right after a toggle reflect the install that just ran, not the
  // pre-toggle state.
  emit windowsChanged();
}

void WaylandWindowMonitor::stopMonitoring() {
  if (!m_monitoring && !m_kwin && !m_gnome && !m_hyprland && !m_sway) {
    return;
  }

  m_monitoring = false;
  m_healthTimer.stop();
  m_lastTrackingReady = false;
  ++m_generation;
  std::shared_ptr<QObject> polling;
  if (m_gnome) polling = m_gnome;
  if (m_hyprland) polling = m_hyprland;
  if (m_sway) polling = m_sway;
  auto retire = [this](auto &backend) {
    if (!backend) return;
    disconnect(backend.get(), nullptr, this, nullptr);
    backend->stop();
    m_retired.push_back({backend, {}});
    backend.reset();
  };
  retire(m_kwin);
  retire(m_gnome);
  retire(m_hyprland);
  retire(m_sway);
  if (m_pollFuture.valid())
    m_retired.push_back({std::move(polling), std::move(m_pollFuture)});
  // Retired workers must not leave a frozen, apparently visible window in
  // the picker or restore-token startup gate while tracking is disabled.
  ingestSnapshot({});
  collectRetired();
  // Keep the housekeeping timer alive while retired operations settle.
  m_pollTimer->start();

  qInfo() << "Wayland window monitoring stopped";
}

QString WaylandWindowMonitor::focusedAppId() const {
  QMutexLocker lg(&m_mutex);
  return m_focusedAppId;
}

QList<WindowInfo> WaylandWindowMonitor::windows() const {
  QMutexLocker lg(&m_mutex);
  return m_windows;
}

WindowInfo WaylandWindowMonitor::windowForPid(qint64 pid) const {
  QMutexLocker lg(&m_mutex);
  for (const WindowInfo &w : m_windows) {
    if (w.pid == pid)
      return w;
  }
  return WindowInfo{};
}

bool WaylandWindowMonitor::kwinScriptInstalled() const {
  return KWinBackend::isScriptInstalled();
}

bool WaylandWindowMonitor::kwinScriptEnabled() const {
  return KWinBackend::isScriptEnabled();
}

bool WaylandWindowMonitor::kwinActive() const {
  return m_kwin && m_kwin->isActive();
}

QString WaylandWindowMonitor::gnomeHelperStatus() const {
  return m_gnome ? m_gnome->status() : QString();
}

bool WaylandWindowMonitor::gnomeHelperInstalled() const {
  return GnomeBackend::isInstalled();
}

bool WaylandWindowMonitor::trackingReady() const {
  if (!m_monitoring) return false;
  if (m_kwin)
    return m_kwin->isActive() && m_kwin->hasSnapshot() &&
           kwinScriptInstalled() && kwinScriptEnabled();
  return (m_gnome && m_gnome->isActive()) ||
         (m_hyprland && m_hyprland->isActive()) ||
         (m_sway && m_sway->isActive());
}

void WaylandWindowMonitor::repairTracking() {
  stopMonitoring();
  startMonitoring(true);
}

void WaylandWindowMonitor::uninstallBackend() {
  stopMonitoring();
  m_removals.push_back(KWinBackend::removeInstalledScriptAsync());
  m_pollTimer->start();
  // Same staleness reason as in startMonitoring: refresh status bindings.
  emit windowsChanged();
}

void WaylandWindowMonitor::ingestSnapshot(const QList<WindowInfo> &windows) {
  // The KWin script reports client.windowId, which is 0 for every Wayland
  // window, so without remapping all KWin rows share id 0 and any
  // id-based lookup matches the first row. Remap chain per row:
  //   real nonzero id -> kept as-is;
  //   zero id + stable string key (KWin internalId) -> deterministic id
  //     derived from the key, so the row tracks across snapshots even
  //     when the backend reorders them (and across restarts);
  //   zero id + no key -> kSyntheticIdBase + snapshot position (old
  //     behavior; stable only while the order holds).
  // Real nonzero ids from any backend are never rewritten.
  QList<WindowInfo> normalized = windows;
  QSet<quint64> used;
  for (const WindowInfo &w : normalized) {
    if (w.id != 0)
      used.insert(w.id);
  }
  for (qsizetype i = 0; i < normalized.size(); ++i) {
    if (normalized[i].id != 0 || normalized[i].key.isEmpty())
      continue;
    quint64 fresh = kSyntheticIdBase |
                    (stableKeyHash(normalized[i].key) & 0x7FFFFFFFULL);
    // Assign keyed ids first so a positional fallback can never steal a
    // keyed id when the snapshot order changes.
    while (used.contains(fresh))
      fresh = kSyntheticIdBase | ((fresh + 1) & 0x7FFFFFFFULL);
    normalized[i].id = fresh;
    used.insert(fresh);
  }
  for (qsizetype i = 0; i < normalized.size(); ++i) {
    if (normalized[i].id != 0)
      continue;
    quint64 fresh = kSyntheticIdBase + quint64(i);
    while (used.contains(fresh))
      ++fresh;
    normalized[i].id = fresh;
    used.insert(fresh);
  }
  QString newFocus;
  for (const WindowInfo &w : normalized) {
    if (w.focused && !w.appId.isEmpty()) {
      newFocus = w.appId;
      break;
    }
  }
  // Fall back to the focused window's title when the class is hidden.
  if (newFocus.isEmpty()) {
    for (const WindowInfo &w : normalized) {
      if (w.focused && !w.title.isEmpty()) {
        newFocus = w.title;
        break;
      }
    }
  }

  bool focusChanged = false;
  {
    QMutexLocker lg(&m_mutex);
    m_windows = normalized;
    if (newFocus != m_focusedAppId) {
      m_focusedAppId = newFocus;
      focusChanged = true;
    }
  }
  emit windowsChanged();
  if (focusChanged)
    emit focusedAppChanged(newFocus);
}

void WaylandWindowMonitor::checkFocusedApp() {
  collectRetired();
  // Watchdog tick. The KWin backend is event-driven (the script pushes on
  // window changes); CLI polling backends refresh here.
  if (!m_monitoring)
    return;
  if (m_kwin) m_kwin->refreshStatus();
  // Polling backends may accept an unchanged/empty first snapshot without
  // emitting windowsUpdated. Still settle readiness (and notice a removed
  // KWin package) without rebuilding window consumers on every timer tick.
  const bool ready = trackingReady();
  if (ready != m_lastTrackingReady) {
    m_lastTrackingReady = ready;
    emit windowsChanged();
  }
  if (!m_gnome && !m_hyprland && !m_sway && !m_pollOverride)
    return; // KWin is push-only; housekeeping needs no polling worker.
  if (m_pollFuture.valid()) {
    if (m_pollFuture.wait_for(std::chrono::seconds(0)) !=
        std::future_status::ready)
      return;
    try {
      m_pollFuture.get();
    } catch (const std::exception &e) {
      qWarning() << "Compositor poll failed:" << e.what();
    }
  }
  try {
    m_pollFuture = std::async(std::launch::async,
        [gnome = m_gnome, hyprland = m_hyprland, sway = m_sway,
         override = m_pollOverride]() {
      if (override) { override(); return; }
      if (gnome) gnome->refresh();
      if (hyprland) hyprland->refresh();
      if (sway) sway->refresh();
    });
  } catch (const std::exception &e) {
    qWarning() << "Unable to start compositor poll:" << e.what();
  }
}

void WaylandWindowMonitor::collectRetired() {
  std::erase_if(m_retired, [](const Retired &retired) {
    if (retired.poll.valid() && retired.poll.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return false;
    const auto kwin = std::dynamic_pointer_cast<KWinBackend>(retired.backend);
    const auto gnome = std::dynamic_pointer_cast<GnomeBackend>(retired.backend);
    return (!kwin || !kwin->hasPendingWork()) && (!gnome || !gnome->hasPendingWork());
  });
  const auto before = m_removals.size();
  std::erase_if(m_removals, [](const auto &future) {
    return future.wait_for(std::chrono::seconds(0)) == std::future_status::ready;
  });
  if (before != m_removals.size()) emit windowsChanged();
  if (!m_monitoring && m_retired.empty() && m_removals.empty()) m_pollTimer->stop();
}

void WaylandWindowMonitor::onBackendWindowsUpdated() {
  if (!m_monitoring) return;
  QObject *src = sender();
  if (m_kwin && src == m_kwin.get())
    ingestSnapshot(m_kwin->windows());
  else if (m_gnome && src == m_gnome.get())
    ingestSnapshot(m_gnome->windows());
  else if (m_hyprland && src == m_hyprland.get())
    ingestSnapshot(m_hyprland->windows());
  else if (m_sway && src == m_sway.get())
    ingestSnapshot(m_sway->windows());
}

void WaylandWindowMonitor::setupCompositorTracking(bool repairBackend) {
  if (m_compositor.contains("kwin", Qt::CaseInsensitive)) {
    if (!m_kwin) {
      m_kwin = std::make_shared<KWinBackend>();
      const auto generation = m_generation;
      connect(m_kwin.get(), &KWinBackend::windowsUpdated, this,
              [this, generation] { if (generation == m_generation) onBackendWindowsUpdated(); });
    }
    if (!m_kwin->start(repairBackend))
      qWarning() << "KWin backend failed to start; window data unavailable";
  } else if (m_compositor.contains("gnome", Qt::CaseInsensitive) ||
             m_compositor.contains("mutter", Qt::CaseInsensitive)) {
    if (!m_gnome) {
      m_gnome = std::make_shared<GnomeBackend>();
      const auto generation = m_generation;
      connect(m_gnome.get(), &GnomeBackend::windowsUpdated, this,
              [this, generation] { if (generation == m_generation) onBackendWindowsUpdated(); });
    }
    if (!m_gnome->start(repairBackend))
      qWarning() << "GNOME backend failed to start; window data unavailable";
  } else if (m_compositor.contains("hyprland", Qt::CaseInsensitive)) {
    if (!m_hyprland) {
      m_hyprland = std::make_shared<HyprlandBackend>();
      const auto generation = m_generation;
      connect(m_hyprland.get(), &HyprlandBackend::windowsUpdated, this,
              [this, generation] { if (generation == m_generation) onBackendWindowsUpdated(); });
    }
    if (!m_hyprland->start())
      qWarning() << "Hyprland backend failed to start; window data unavailable";
  } else if (m_compositor.contains("sway", Qt::CaseInsensitive)) {
    if (!m_sway) {
      m_sway = std::make_shared<SwayBackend>();
      const auto generation = m_generation;
      connect(m_sway.get(), &SwayBackend::windowsUpdated, this,
              [this, generation] { if (generation == m_generation) onBackendWindowsUpdated(); });
    }
    if (!m_sway->start())
      qWarning() << "Sway backend failed to start; window data unavailable";
  } else {
    qWarning() << "Unknown compositor, window tracking unavailable:"
               << m_compositor;
  }
}

QString WaylandWindowMonitor::detectCompositor() {
  QProcessEnvironment env = QProcessEnvironment::systemEnvironment();

  // Canonicalize to the keys setupCompositorTracking understands. Notably
  // KDE sessions report XDG_CURRENT_DESKTOP=KDE (not KWin).
  auto canonical = [](const QString &raw) -> QString {
    const QString v = raw.toLower();
    if (v.isEmpty())
      return QString();
    if (v.contains("kwin") || v.contains("kde") || v.contains("plasma"))
      return "kwin";
    if (v.contains("gnome") || v.contains("mutter"))
      return "gnome";
    if (v.contains("hyprland"))
      return "hyprland";
    if (v.contains("sway"))
      return "sway";
    return v;
  };

  const QString desktop = canonical(env.value("XDG_CURRENT_DESKTOP", ""));
  if (!desktop.isEmpty())
    return desktop;

  const QString session = canonical(env.value("DESKTOP_SESSION", ""));
  if (!session.isEmpty())
    return session;

  // Check for specific compositor env vars
  if (!env.value("HYPRLAND_INSTANCE_SIGNATURE", "").isEmpty()) {
    return "hyprland";
  }
  if (!env.value("SWAYSOCK", "").isEmpty()) {
    return "sway";
  }
  if (!env.value("KDE_SESSION_VERSION", "").isEmpty()) {
    return "kwin";
  }

  return "unknown";
}

} // namespace TrophyBackend
