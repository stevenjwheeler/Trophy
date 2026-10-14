#pragma once

#include "detection/WindowInfo.h"

#include <QList>
#include <QObject>
#include <QPointer>
#include <QString>
#include <QVariantMap>

namespace TrophyBackend {

class WaylandWindowMonitor;

/**
 * @brief Service to list available windows for manual recording.
 *
 * Answers from the shared WaylandWindowMonitor backend (KWin first).
 * Window ids are the compositor's window ids, except on KWin where the
 * script reports id 0 for every window: the monitor assigns synthetic
 * ids (WaylandWindowMonitor::kSyntheticIdBase + snapshot position) at
 * ingest time, so every listed row still has a distinct id. Treat the
 * id as an opaque token: getVisibleWindows() hands out the maps the
 * pickers list (each map's "id" travels with it through QML
 * search/filter/sort), and the title/pid/class lookups resolve that
 * id — never a positional row number, which filtering would shift.
 * Without a monitor attached, all queries return empty/0 (safe no-op).
 */
class WindowService : public QObject {
  Q_OBJECT

public:
  explicit WindowService(QObject *parent = nullptr);
  ~WindowService() override;

  /** Attach the shared compositor monitor (owned by Application). */
  void setWindowMonitor(WaylandWindowMonitor *monitor);

  /**
   * @brief Get a list of visible windows
   * @return List of maps with keys: id, title, class, pid
   */
  Q_INVOKABLE QVariantList getVisibleWindows();
  Q_INVOKABLE QString getWindowTitle(unsigned long windowId);

  /**
   * @brief Get the owning PID for a compositor window id (0 if unknown).
   */
  unsigned long getPidForWindow(unsigned long windowId);

  /**
   * @brief Get the process name for a window by its process ID
   */
  QString getProcessNameForWindow(unsigned long windowId);

  /**
   * @brief Get the application class for a window (e.g. "Spotify")
   */
  QString getWindowClassForWindow(unsigned long windowId);

private:
  QString fetchWindowTitle(unsigned long windowId);

  /**
   * @brief Resolve an id handed out by getVisibleWindows().
   *
   * Searches the snapshot cache from the last getVisibleWindows() call
   * first, so lookups hit the SAME rows the picker listed even if a new
   * compositor snapshot arrived in between (positional synthetic ids
   * could otherwise remap). Falls back to the live monitor snapshot
   * for ids created after the listing. Returns false when unknown.
   */
  bool findWindow(quint64 id, WindowInfo *out) const;

  QPointer<WaylandWindowMonitor> m_monitor;
  QList<WindowInfo> m_lastWindows;
};

} // namespace TrophyBackend
