/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "WindowChrome.h"

namespace TrophyBackend {

bool WindowChrome::shouldSquare(Qt::WindowState state, QSize size,
                                QSize screen, QSize available) {
  if (state == Qt::WindowMaximized || state == Qt::WindowFullScreen)
    return true;
  if (!size.isValid() || size.isEmpty())
    return false;
  // Qt's public Wayland API has no portable tiled-state notification.
  // Treat full-height/full-width windows as edge-constrained. This is a
  // size heuristic, not proof of snapping; a manually full-span window also
  // becomes square. Quarter tiles/custom layouts are intentionally not guessed.
  const auto spans = [size](QSize extent) {
    return extent.width() > 2 && extent.height() > 2 &&
           (size.width() >= extent.width() - 2 ||
            size.height() >= extent.height() - 2);
  };
  return spans(available) || spans(screen);
}

void WindowChrome::setWindow(QWindow *window) {
  if (m_window == window)
    return;
  for (const auto &connection : m_windowConnections)
    disconnect(connection);
  m_windowConnections.clear();
  m_window = window;
  if (window) {
    m_windowConnections.append(connect(window, &QWindow::widthChanged, this, &WindowChrome::refresh));
    m_windowConnections.append(connect(window, &QWindow::heightChanged, this, &WindowChrome::refresh));
    m_windowConnections.append(connect(window, &QWindow::windowStateChanged, this, &WindowChrome::refresh));
    m_windowConnections.append(connect(window, &QWindow::screenChanged, this, [this](QScreen *screen) {
      bindScreen(screen);
      refresh();
    }));
    m_windowConnections.append(connect(window, &QObject::destroyed, this, [this] {
      bindScreen(nullptr);
      emit windowChanged();
      refresh();
    }));
  }
  bindScreen(window ? window->screen() : nullptr);
  emit windowChanged();
  refresh();
}

void WindowChrome::bindScreen(QScreen *screen) {
  for (const auto &connection : m_screenConnections)
    disconnect(connection);
  m_screenConnections.clear();
  if (screen) {
    m_screenConnections.append(connect(screen, &QScreen::geometryChanged, this, &WindowChrome::refresh));
    m_screenConnections.append(connect(screen, &QScreen::availableGeometryChanged, this, &WindowChrome::refresh));
  }
}

void WindowChrome::refresh() {
  auto *screen = m_window ? m_window->screen() : nullptr;
  const bool square = m_window && shouldSquare(m_window->windowState(), m_window->size(),
      screen ? screen->geometry().size() : QSize(),
      screen ? screen->availableGeometry().size() : QSize());
  if (m_squareCorners != square) {
    m_squareCorners = square;
    emit squareCornersChanged();
  }
}

} // namespace TrophyBackend
