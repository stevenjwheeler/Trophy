/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "KWinBackend.h"

#include <QCoreApplication>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace TrophyBackend {

KWinBackend::KWinBackend(QObject *parent) : QObject(parent) {}

KWinBackend::~KWinBackend() { stop(); }

QList<WindowInfo> KWinBackend::parseSnapshot(const QByteArray &json) {
  QList<WindowInfo> out;
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError || !doc.isArray())
    return out;

  for (const QJsonValue &v : doc.array()) {
    if (!v.isObject())
      continue;
    const WindowInfo w = WindowInfo::fromJson(v.toObject());
    // Skip entries carrying no information at all.
    if (w.id == 0 && w.appId.isEmpty() && w.title.isEmpty())
      continue;
    out.append(w);
  }
  return out;
}

QList<WindowInfo> KWinBackend::windows() const {
  QMutexLocker lg(&m_mutex);
  return m_windows;
}

void KWinBackend::ReportWindows(const QString &json) {
  const QList<WindowInfo> parsed =
      parseSnapshot(json.toUtf8());
  {
    QMutexLocker lg(&m_mutex);
    m_windows = parsed;
  }
  emit windowsUpdated();
}

QString KWinBackend::findScriptSourceDir() {
  // 1. Explicit override (dev / packaging).
  const QString env = QString::fromLocal8Bit(qgetenv("TROPHY_KWIN_SCRIPT_DIR"));
  if (!env.isEmpty()) {
    if (QFile::exists(env + "/metadata.json"))
      return env;
    qWarning() << "TROPHY_KWIN_SCRIPT_DIR set but has no metadata.json:" << env;
  }

  // 2. Installed location: <datadir>/trophy/kwin/trophy-monitor.
  const QString installed = QStandardPaths::locate(
      QStandardPaths::GenericDataLocation,
      "trophy/kwin/trophy-monitor/metadata.json");
  if (!installed.isEmpty())
    return QFileInfo(installed).absolutePath();

  // 3. CMake build-tree copy next to the binaries (dev runs).
  const QString appDir = QCoreApplication::applicationDirPath();
  const QStringList devCandidates = {
      appDir + "/kwin-scripts/trophy-monitor",
  };
  for (const QString &dir : devCandidates) {
    if (QFile::exists(dir + "/metadata.json"))
      return dir;
  }
  return QString();
}

bool KWinBackend::runTool(const QString &tool, const QStringList &args,
                          QString *output) {
  const QString exe =
      QStandardPaths::findExecutable(tool);
  if (exe.isEmpty()) {
    // Missing tools are normal (older Plasma generations, best-effort
    // cleanup paths). Callers warn with context when it matters.
    qDebug() << "KWin backend: tool not found:" << tool;
    return false;
  }
  QProcess proc;
  proc.start(exe, args);
  if (!proc.waitForFinished(10000)) {
    qWarning() << "KWin backend: tool timed out:" << tool;
    proc.kill();
    return false;
  }
  if (output)
    *output = QString::fromLocal8Bit(proc.readAllStandardOutput()).trimmed();
  return proc.exitCode() == 0;
}

static bool fileContentsEqual(const QString &a, const QString &b) {
  QFile fa(a), fb(b);
  if (!fa.open(QIODevice::ReadOnly) || !fb.open(QIODevice::ReadOnly))
    return false;
  return fa.readAll() == fb.readAll();
}

QString KWinBackend::installedScriptDir() {
  return QStandardPaths::writableLocation(
             QStandardPaths::GenericDataLocation) +
         "/kwin/scripts/" + kScriptId;
}

bool KWinBackend::isScriptInstalled() {
  return QFile::exists(installedScriptDir() + "/metadata.json");
}

bool KWinBackend::isScriptEnabled() {
  QString current;
  if (!runTool("kreadconfig6", {"--file", "kwinrc", "--group", "Plugins",
                                "--key", QString(kScriptId) + "Enabled"},
               &current)) {
    // Older Plasma still ships the Qt5 tool names.
    runTool("kreadconfig5", {"--file", "kwinrc", "--group", "Plugins",
                             "--key", QString(kScriptId) + "Enabled"},
            &current);
  }
  return current.compare("true", Qt::CaseInsensitive) == 0;
}

bool KWinBackend::installScript(bool *changed) {
  const QString srcDir = findScriptSourceDir();
  if (srcDir.isEmpty()) {
    qWarning() << "KWin backend: bundled trophy-monitor script not found "
                  "(checked TROPHY_KWIN_SCRIPT_DIR, datadir and build tree)";
    return false;
  }

  const QString destDir = installedScriptDir();
  const QStringList files = {"metadata.json", "contents/code/main.js"};

  // Already installed and identical: nothing to do (and no KWin reload).
  bool identical = QFile::exists(destDir + "/metadata.json");
  for (const QString &rel : files) {
    if (!fileContentsEqual(srcDir + "/" + rel, destDir + "/" + rel)) {
      identical = false;
      break;
    }
  }
  if (identical) {
    *changed = false;
    return true;
  }

  // Official installer first: this registers the package so it shows up
  // in System Settings > KWin Scripts, where the user can inspect,
  // disable or remove it themselves.
  const QString tool =
      QStandardPaths::findExecutable("kpackagetool6");
  if (!tool.isEmpty()) {
    const QString op = isScriptInstalled() ? "-u" : "-i";
    if (runTool("kpackagetool6",
                {"--type", "KWin/Script", op, srcDir})) {
      *changed = true;
      return isScriptInstalled();
    }
    qWarning() << "KWin backend: kpackagetool6 failed, falling back to copy";
  } else {
    qInfo() << "KWin backend: kpackagetool6 not found, copying package "
               "directly (still visible in System Settings)";
  }

  // Fallback: plain copy into the per-user script dir (KWin scans it too).
  QDir().mkpath(destDir + "/contents/code");
  for (const QString &rel : files) {
    const QString src = srcDir + "/" + rel;
    const QString dst = destDir + "/" + rel;
    if (QFile::exists(dst) && !QFile::remove(dst)) {
      qWarning() << "KWin backend: cannot replace" << dst;
      return false;
    }
    if (!QFile::copy(src, dst)) {
      qWarning() << "KWin backend: cannot install" << dst;
      return false;
    }
  }
  *changed = true;
  return isScriptInstalled();
}

bool KWinBackend::ensureScriptInstalled() {
  bool changed = false;
  return installScript(&changed);
}

bool KWinBackend::removeInstalledScript() {
  const bool wasPresent = isScriptInstalled();
  bool gone = !wasPresent;
  if (!gone) {
    if (!runTool("kpackagetool6",
                 {"--type", "KWin/Script", "-r", kScriptId})) {
      // Fallback: delete our own package dir (id is namespaced to us).
      gone = QDir(installedScriptDir()).removeRecursively();
    } else {
      gone = !isScriptInstalled();
    }
  }
  // Clear the stale enable key either way (best effort, both tool generations).
  runTool("kwriteconfig6", {"--file", "kwinrc", "--group", "Plugins", "--key",
                            QString(kScriptId) + "Enabled", "false"});
  runTool("kwriteconfig5", {"--file", "kwinrc", "--group", "Plugins", "--key",
                            QString(kScriptId) + "Enabled", "false"});
  if (gone && wasPresent)
    reconfigureKWinStatic();
  return gone;
}

void KWinBackend::reconfigureKWinStatic() {
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected())
    return;
  QDBusMessage msg = QDBusMessage::createMethodCall(
      "org.kde.KWin", "/KWin", "org.kde.KWin", "reconfigure");
  bus.call(msg, QDBus::NoBlock);
}

bool KWinBackend::ensureScriptEnabled(bool *changed) {
  // kwinrc: [Plugins] trophy-monitorEnabled=true
  if (isScriptEnabled()) {
    *changed = false;
    return true;
  }
  const bool enabled = runTool(
      "kwriteconfig6", {"--file", "kwinrc", "--group", "Plugins", "--key",
                        QString(kScriptId) + "Enabled", "true"});
  if (!enabled &&
      !runTool("kwriteconfig5", {"--file", "kwinrc", "--group", "Plugins",
                                 "--key", QString(kScriptId) + "Enabled",
                                 "true"})) {
    qWarning() << "KWin backend: cannot enable script (no kwriteconfig)";
    return false;
  }
  *changed = true;
  return true;
}

void KWinBackend::reconfigureKWin() { reconfigureKWinStatic(); }

bool KWinBackend::start() {
  if (m_active)
    return true;

  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    qWarning() << "KWin backend: no D-Bus session bus";
    return false;
  }

  bool scriptChanged = false;
  const bool scriptOk = installScript(&scriptChanged);
  bool enableChanged = false;
  const bool enabledOk = ensureScriptEnabled(&enableChanged);
  if (scriptOk && enabledOk && (scriptChanged || enableChanged)) {
    // A fresh/updated/newly-enabled script only loads after reconfigure.
    reconfigureKWin();
  }
  if (!scriptOk || !enabledOk) {
    qWarning() << "KWin backend: script setup incomplete, "
                  "window data may be unavailable";
    // Continue anyway: a previously installed script may still report.
  }

  if (!bus.registerService(kServiceName)) {
    qWarning() << "KWin backend: could not own" << kServiceName
               << "(another Trophy instance?)";
    return false;
  }
  if (!bus.registerObject(kObjectPath, this,
                          QDBusConnection::ExportAllSlots)) {
    qWarning() << "KWin backend: could not register" << kObjectPath;
    bus.unregisterService(kServiceName);
    return false;
  }
  m_dbusRegistered = true;
  m_active = true;
  qDebug() << "KWin window tracking active (trophy-monitor script)";
  return true;
}

void KWinBackend::stop() {
  if (!m_active && !m_dbusRegistered)
    return;
  if (m_dbusRegistered) {
    QDBusConnection::sessionBus().unregisterObject(kObjectPath);
    QDBusConnection::sessionBus().unregisterService(kServiceName);
    m_dbusRegistered = false;
  }
  m_active = false;
}

} // namespace TrophyBackend
