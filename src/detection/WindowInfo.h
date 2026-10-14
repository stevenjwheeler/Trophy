#pragma once

#include <QJsonObject>
#include <QString>
#include <QtGlobal>

namespace TrophyBackend {

/**
 * @brief One top-level window reported by a compositor backend.
 *
 * Produced by WaylandWindowMonitor backends (KWin first, then GNOME,
 * Hyprland, Sway) and shared with WindowService. All fields are plain
 * data so snapshots cross D-Bus as JSON and stay unit-testable.
 */
struct WindowInfo {
  quint64 id = 0;   // compositor window id (KWin client.windowId)
  // Stable string identity (KWin client.internalId). Preferred over the
  // numeric id when the compositor reports 0 for every window: the
  // monitor derives deterministic synthetic ids from it, so rows track
  // across snapshots even when the backend reorders them.
  QString key;
  QString appId;    // window class / app_id (KWin resourceClass)
  QString title;    // window caption
  qint64 pid = 0;   // owning process id (0 when the compositor hides it)
  bool fullscreen = false;
  bool focused = false;
  bool minimized = false;
  // Window geometry + hosting output size, for borderless-windowed games
  // that never set fullscreen (coverage >= ~90% counts as fullscreen).
  // Zero when the backend could not determine them.
  int x = 0;
  int y = 0;
  int width = 0;
  int height = 0;
  int outputWidth = 0;
  int outputHeight = 0;

  /** Build from a snapshot object; missing keys default safely. */
  static WindowInfo fromJson(const QJsonObject &o);
};

} // namespace TrophyBackend
