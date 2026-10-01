/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "SwayBackend.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QStandardPaths>

namespace TrophyBackend {

SwayBackend::SwayBackend(QObject *parent) : QObject(parent) {}

bool SwayBackend::start() {
  if (m_active)
    return true;
  if (QStandardPaths::findExecutable("swaymsg").isEmpty()) {
    qWarning() << "Sway backend: swaymsg not found";
    return false;
  }
  if (qgetenv("SWAYSOCK").isEmpty())
    qWarning() << "Sway backend: SWAYSOCK unset, swaymsg may fail";
  qInfo() << "Sway window tracking armed (swaymsg polling)";
  return true;
}

void SwayBackend::collectWindows(const QJsonObject &node,
                                 QList<WindowInfo> &out, int outputWidth,
                                 int outputHeight) {
  // Output nodes carry the display size for everything beneath them.
  if (node.value("type").toString() == "output") {
    const QJsonObject rect = node.value("rect").toObject();
    const int ow = rect.value("width").toInt();
    const int oh = rect.value("height").toInt();
    if (ow > 0 && oh > 0) {
      outputWidth = ow;
      outputHeight = oh;
    }
  }

  // A window node carries a pid; plain containers do not (sway-ipc(7)).
  // Recurse through both tiled and floating children regardless.
  if (node.contains("pid")) {
    WindowInfo w;
    w.id = (quint64)node.value("id").toDouble();
    w.appId = node.value("app_id").toString();
    if (w.appId.isEmpty()) {
      // XWayland fallback: window class from window_properties.
      const QJsonObject props = node.value("window_properties").toObject();
      w.appId = props.value("class").toString();
    }
    w.title = node.value("name").toString();
    if (w.title.isEmpty()) {
      const QJsonObject props = node.value("window_properties").toObject();
      w.title = props.value("title").toString();
    }
    w.pid = (qint64)node.value("pid").toDouble();
    w.fullscreen = node.value("fullscreen_mode").toInt(0) != 0;
    w.focused = node.value("focused").toBool();
    const QJsonObject rect = node.value("rect").toObject();
    w.x = rect.value("x").toInt();
    w.y = rect.value("y").toInt();
    w.width = rect.value("width").toInt();
    w.height = rect.value("height").toInt();
    w.outputWidth = outputWidth;
    w.outputHeight = outputHeight;
    if (w.id != 0 || !w.appId.isEmpty() || !w.title.isEmpty())
      out.append(w);
  }

  for (const QJsonValue &child : node.value("nodes").toArray()) {
    if (child.isObject())
      collectWindows(child.toObject(), out, outputWidth, outputHeight);
  }
  for (const QJsonValue &child : node.value("floating_nodes").toArray()) {
    if (child.isObject())
      collectWindows(child.toObject(), out, outputWidth, outputHeight);
  }
}

QList<WindowInfo> SwayBackend::parseTree(const QByteArray &json) {
  QList<WindowInfo> out;
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError)
    return out;
  if (doc.isArray()) {
    // Defensive: some wrappers emit a one-element array.
    for (const QJsonValue &v : doc.array()) {
      if (v.isObject())
        collectWindows(v.toObject(), out, 0, 0);
    }
  } else if (doc.isObject()) {
    collectWindows(doc.object(), out, 0, 0);
  }
  return out;
}

QList<WindowInfo> SwayBackend::windows() const {
  QMutexLocker lg(&m_mutex);
  return m_windows;
}

void SwayBackend::refresh() {
  const QString exe = QStandardPaths::findExecutable("swaymsg");
  if (exe.isEmpty())
    return; // silent no-op off-compositor
  QProcess proc;
  proc.start(exe, {"-t", "get_tree"});
  if (!proc.waitForFinished(2000)) {
    proc.kill();
    return;
  }
  if (proc.exitCode() != 0)
    return;

  const QList<WindowInfo> parsed =
      parseTree(proc.readAllStandardOutput());
  {
    QMutexLocker lg(&m_mutex);
    m_windows = parsed;
  }
  m_active = true;
  emit windowsUpdated();
}

} // namespace TrophyBackend
