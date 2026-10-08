#pragma once

#include <QList>
#include <QObject>
#include <QString>
#include <QVariantMap>

namespace TrophyBackend {

class WaylandWindowMonitor;

/**
 * @brief Service to list available windows for manual recording.
 *
 * Answers from the shared WaylandWindowMonitor backend (KWin first).
 * Window ids are the compositor's window ids (KWin client.windowId).
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

  WaylandWindowMonitor *m_monitor = nullptr;
};

} // namespace TrophyBackend
