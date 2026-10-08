/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "HyprlandBackend.h"

#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMap>
#include <QProcess>
#include <QSize>
#include <QStandardPaths>

namespace TrophyBackend {

HyprlandBackend::HyprlandBackend(QObject *parent) : QObject(parent) {}

bool HyprlandBackend::start() {
  if (m_active)
    return true;
  if (QStandardPaths::findExecutable("hyprctl").isEmpty()) {
    qWarning() << "Hyprland backend: hyprctl not found";
    return false;
  }
  qInfo() << "Hyprland window tracking armed (hyprctl polling)";
  return true;
}

QString HyprlandBackend::runHyprctl(const QStringList &args) {
  const QString exe = QStandardPaths::findExecutable("hyprctl");
  if (exe.isEmpty())
    return QString();
  QProcess proc;
  proc.start(exe, args);
  if (!proc.waitForFinished(2000)) {
    proc.kill();
    return QString();
  }
  if (proc.exitCode() != 0)
    return QString();
  return QString::fromLocal8Bit(proc.readAllStandardOutput());
}

QList<WindowInfo> HyprlandBackend::parseClients(const QByteArray &json,
                                               const QString &activeAddress,
                                               const QByteArray &monitorsJson) {
  QList<WindowInfo> out;
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError || !doc.isArray())
    return out;

  // Output sizes by workspace id, for borderless-coverage checks.
  QMap<int, QSize> outputByWorkspace;
  if (!monitorsJson.isEmpty()) {
    QJsonParseError merr;
    const QJsonDocument mdoc =
        QJsonDocument::fromJson(monitorsJson, &merr);
    if (merr.error == QJsonParseError::NoError && mdoc.isArray()) {
      for (const QJsonValue &mv : mdoc.array()) {
        if (!mv.isObject())
          continue;
        const QJsonObject mo = mv.toObject();
        const int wsId =
            mo.value("activeWorkspace").toObject().value("id").toInt(-1);
        const int mw = mo.value("width").toInt();
        const int mh = mo.value("height").toInt();
        if (wsId >= 0 && mw > 0 && mh > 0)
          outputByWorkspace[wsId] = QSize(mw, mh);
      }
    }
  }

  // Normalize the active address for comparison ("0x…" vs plain hex).
  quint64 activeId = 0;
  if (!activeAddress.isEmpty()) {
    bool ok = false;
    activeId = activeAddress.trimmed().toULongLong(&ok, 16);
    if (!ok)
      activeId = 0;
  }

  for (const QJsonValue &v : doc.array()) {
    if (!v.isObject())
      continue;
    const QJsonObject o = v.toObject();
    WindowInfo w = WindowInfo::fromJson(o); // title/pid passthrough
    bool idOk = false;
    w.id = o.value("address").toString().trimmed().toULongLong(&idOk, 16);
    w.appId = o.value("class").toString();
    if (w.appId.isEmpty())
      w.appId = o.value("initialClass").toString();
    if (w.title.isEmpty())
      w.title = o.value("initialTitle").toString();
    // Hyprland fullscreen modes: 0 none, 1 maximized, 2 fullscreen,
    // 3 maximized+fullscreen.
    w.fullscreen = o.value("fullscreen").toInt(0) >= 2;
    if (activeId != 0)
      w.focused = (idOk && w.id == activeId);
    else
      w.focused = (o.value("focusHistoryID").toInt(-1) == 0);
    const QJsonArray at = o.value("at").toArray();
    if (at.size() >= 2) {
      w.x = at.at(0).toInt();
      w.y = at.at(1).toInt();
    }
    const QJsonArray size = o.value("size").toArray();
    if (size.size() >= 2) {
      w.width = size.at(0).toInt();
      w.height = size.at(1).toInt();
    }
    const int wsId = o.value("workspace").toObject().value("id").toInt(-1);
    if (outputByWorkspace.contains(wsId)) {
      w.outputWidth = outputByWorkspace[wsId].width();
      w.outputHeight = outputByWorkspace[wsId].height();
    }
    if ((!idOk || w.id == 0) && w.appId.isEmpty() && w.title.isEmpty())
      continue;
    out.append(w);
  }
  return out;
}

QList<WindowInfo> HyprlandBackend::windows() const {
  QMutexLocker lg(&m_mutex);
  return m_windows;
}

void HyprlandBackend::refresh() {
  const QString clients = runHyprctl({"clients", "-j"});
  if (clients.isEmpty())
    return; // no compositor / tool failed: silent no-op

  // Focused address, best effort (empty on no focus or old hyprctl).
  QString activeAddress;
  const QString activeJson = runHyprctl({"activewindow", "-j"});
  if (!activeJson.isEmpty()) {
    QJsonParseError err;
    const QJsonDocument doc =
        QJsonDocument::fromJson(activeJson.toUtf8(), &err);
    if (err.error == QJsonParseError::NoError && doc.isObject())
      activeAddress = doc.object().value("address").toString();
  }

  const QList<WindowInfo> parsed =
      parseClients(clients.toUtf8(), activeAddress,
                   runHyprctl({"monitors", "-j"}).toUtf8());
  {
    QMutexLocker lg(&m_mutex);
    m_windows = parsed;
  }
  m_active = true;
  emit windowsUpdated();
}

} // namespace TrophyBackend
