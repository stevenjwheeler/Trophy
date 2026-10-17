/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QObject>
#include <QPointer>
#include <QScreen>
#include <QWindow>
#include <QtQml/qqmlregistration.h>

namespace TrophyBackend {

// Own-window presentation only. No compositor queries or process discovery.
class WindowChrome : public QObject {
  Q_OBJECT
  QML_ELEMENT
  Q_PROPERTY(QWindow *targetWindow READ window WRITE setWindow NOTIFY windowChanged)
  Q_PROPERTY(bool squareCorners READ squareCorners NOTIFY squareCornersChanged)

public:
  explicit WindowChrome(QObject *parent = nullptr) : QObject(parent) {}
  QWindow *window() const { return m_window; }
  void setWindow(QWindow *window);
  bool squareCorners() const { return m_squareCorners; }
  static bool shouldSquare(Qt::WindowState state, QSize size,
                           QSize screen, QSize available);

signals:
  void windowChanged();
  void squareCornersChanged();

private:
  void bindScreen(QScreen *screen);
  void refresh();
  QPointer<QWindow> m_window;
  QList<QMetaObject::Connection> m_windowConnections;
  QList<QMetaObject::Connection> m_screenConnections;
  bool m_squareCorners = false;
};

} // namespace TrophyBackend
