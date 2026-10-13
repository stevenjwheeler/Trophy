/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GnomeBackend.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace TrophyBackend {

GnomeBackend::GnomeBackend(QObject *parent) : QObject(parent) {}

bool GnomeBackend::start() {
  if (m_active)
    return true;
  if (!QDBusConnection::sessionBus().isConnected()) {
    qWarning() << "GNOME backend: no D-Bus session bus";
    return false;
  }
  // Activation is lazy: the first refresh() probes Eval. Mark the backend
  // present so the monitor ticks it; refresh() flips m_active on success.
  qInfo() << "GNOME window tracking armed (Shell.Eval polling)";
  return true;
}

QString GnomeBackend::snapshotScript() {
  // Runs inside GNOME Shell. Guards everywhere: API shapes differ across
  // Shell versions, and a throwing script would kill our whole snapshot.
  return QStringLiteral(
      "(() => {"
      "  try {"
      "    var out = [];"
      "    var focus = null;"
      "    try { focus = global.display.focus_window; } catch (e) {}"
      "    var actors = [];"
      "    try { actors = global.get_window_actors(); } catch (e) {}"
      "    for (var i = 0; i < actors.length; i++) {"
      "      try {"
      "        var w = actors[i].get_meta_window();"
      "        if (!w) continue;"
      "        var skip = false;"
      "        try { skip = w.is_skip_taskbar(); } catch (e) {}"
      "        var focused = false;"
      "        try { focused = (w === focus) || w.has_focus(); } catch (e) {}"
      "        if (skip && !focused) continue;"
      "        var id = 0;"
      "        try { id = w.get_stable_sequence(); } catch (e) {}"
      "        if (!id) { try { id = w.get_id(); } catch (e) {} }"
      "        var cls = '';"
      "        try { cls = w.get_wm_class() || ''; } catch (e) {}"
      "        var title = '';"
      "        try { title = w.get_title() || ''; } catch (e) {}"
      "        var pid = 0;"
      "        try { pid = w.get_pid() || 0; } catch (e) {}"
      "        var fs = false;"
      "        try { fs = !!(w.is_fullscreen ? w.is_fullscreen() : w.fullscreen); } catch (e) {}"
      "        var gx = 0, gy = 0, gw = 0, gh = 0, ow = 0, oh = 0;"
      "        try {"
      "          var r = w.get_frame_rect();"
      "          if (r) { gx = r.x || 0; gy = r.y || 0;"
      "                   gw = r.width || 0; gh = r.height || 0; }"
      "          var geo = global.display.get_monitor_geometry(w.get_monitor());"
      "          if (geo) { ow = geo.width || 0; oh = geo.height || 0; }"
      "        } catch (e) { /* geometry optional */ }"
      "        if (!id && !cls && !title) continue;"
      "        out.push({id: id, appId: String(cls), title: String(title),"
      "                  pid: pid, fullscreen: fs, focused: focused,"
      "                  x: gx, y: gy, width: gw, height: gh,"
      "                  outputWidth: ow, outputHeight: oh});"
      "      } catch (e) { /* per-window failure: skip it */ }"
      "    }"
      "    return JSON.stringify(out);"
      "  } catch (e) { return 'null'; }"
      "})()");
}

QList<WindowInfo> GnomeBackend::parseEvalResult(const QString &result) {
  QList<WindowInfo> out;
  const QString trimmed = result.trimmed();
  if (trimmed.isEmpty() || trimmed == "null")
    return out;

  // GNOME may return the string with surrounding quotes when the script
  // result passes through an extra stringification layer. The outer value
  // is JSON, so parse it as a JSON string (handles all escapes: \", \\,
  // \n, \uXXXX — a hand-rolled unescape gets the order wrong and misses
  // the latter two).
  QString json = trimmed;
  if (json.startsWith('"')) {
    // QJsonDocument only holds arrays/objects, so parse the outer
    // string layer via a single-element wrapper.
    QJsonParseError strErr;
    const QJsonDocument strDoc =
        QJsonDocument::fromJson(("[" + json + "]").toUtf8(), &strErr);
    if (strErr.error == QJsonParseError::NoError && strDoc.isArray() &&
        !strDoc.array().isEmpty() && strDoc.array().first().isString())
      json = strDoc.array().first().toString();
  }

  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &err);
  if (err.error != QJsonParseError::NoError || !doc.isArray())
    return out;

  for (const QJsonValue &v : doc.array()) {
    if (!v.isObject())
      continue;
    const WindowInfo w = WindowInfo::fromJson(v.toObject());
    if (w.id == 0 && w.appId.isEmpty() && w.title.isEmpty())
      continue;
    out.append(w);
  }
  return out;
}

QList<WindowInfo> GnomeBackend::windows() const {
  QMutexLocker lg(&m_mutex);
  return m_windows;
}

QString GnomeBackend::signatureFor(const QList<WindowInfo> &windows) {
  QString sig;
  sig.reserve(windows.size() * 48);
  for (const WindowInfo &w : windows) {
    sig += QString::number(w.id);
    sig += '|';
    sig += w.appId;
    sig += '|';
    sig += w.title;
    sig += '|';
    sig += QString::number(w.pid);
    sig += w.fullscreen ? "|F" : "|f";
    sig += w.focused ? "|A|" : "|a|";
    // Geometry matters too: a borderless game growing to >=90% output
    // coverage (no focus/fullscreen change) must still emit so pending
    // candidates confirm via the coverage rule.
    sig += '|';
    sig += QString::number(w.width);
    sig += 'x';
    sig += QString::number(w.height);
    sig += '@';
    sig += QString::number(w.outputWidth);
    sig += 'x';
    sig += QString::number(w.outputHeight);
    sig += ';';
  }
  return sig;
}

void GnomeBackend::refresh() {
  // Eval(script) -> (bool success, QString result).
  auto request = QDBusMessage::createMethodCall(kService, kPath, kInterface, "Eval");
  request.setArguments({snapshotScript()});
  const QDBusMessage reply = QDBusConnection::sessionBus().call(request, QDBus::Block, 2000);
  m_probed = true;
  if (reply.type() == QDBusMessage::ErrorMessage)
    return;

  // Reply packs (success, result) as two arguments.
  const QList<QVariant> args = reply.arguments();
  if (args.size() < 2 || !args.at(0).toBool())
    return;

  const QList<WindowInfo> parsed = parseEvalResult(args.at(1).toString());
  // Snapshots rarely change between ticks; skip the emit (and the QML
  // refresh churn behind it) when nothing did. The 1s cadence itself
  // costs one local D-Bus round-trip — negligible next to a recording.
  const QString sig = signatureFor(parsed);
  {
    QMutexLocker lg(&m_mutex);
    // A successful poll means the backend is alive even when the
    // snapshot is unchanged (and empty-first snapshots must not read
    // inactive): mark active before the dedup early-return.
    m_active = true;
    if (sig == m_lastSignature)
      return;
    m_lastSignature = sig;
    m_windows = parsed;
  }
  emit windowsUpdated();
}

} // namespace TrophyBackend
