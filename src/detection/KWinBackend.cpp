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
#include <mutex>

#include "detection/ShippedLibDirs.h"

namespace TrophyBackend {

std::atomic<int> KWinBackend::s_enabledCache{-1};
std::function<bool(const QString &, const QStringList &, QString *)> KWinBackend::s_toolOverride;
std::function<void()> KWinBackend::s_restartOverride;

std::shared_future<void> KWinBackend::enqueueOperation(std::function<void()> operation) {
  // One ordered lane for ALL setup/removal side effects, across backend
  // replacements. Off queues after old setup; a later on queues after off.
  static std::mutex mutex;
  static std::shared_future<void> tail;
  std::lock_guard lock(mutex);
  auto previous = tail;
  if (previous.valid() && previous.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    previous = {}; // do not retain completed historical operation chains
  tail = std::async(std::launch::async,
      [previous, operation = std::move(operation)] {
        if (previous.valid()) previous.wait();
        operation();
      }).share();
  return tail;
}

bool KWinBackend::hasPendingWork() const {
  return (m_setupFuture.valid() &&
          m_setupFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready) ||
         (m_statusFuture.valid() &&
          m_statusFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready);
}

std::shared_future<void> KWinBackend::removeInstalledScriptAsync() {
  return enqueueOperation([] { removeInstalledScriptSync(); });
}

KWinBackend::KWinBackend(QObject *parent) : QObject(parent) {}

KWinBackend::~KWinBackend() {
  stop();
  if (m_setupFuture.valid()) m_setupFuture.wait();
  if (m_statusFuture.valid()) m_statusFuture.wait();
}

void KWinBackend::refreshStatus() {
  if (!m_active) return;
  if (m_setupTimer && m_setupTimer->isActive()) return;
  // Do not read midway through the install/enable sequence, or stack queries
  // when kreadconfig is slow. Workers return data only; publication is owned
  // by this backend and a stop/restart supersedes the result.
  if (m_setupFuture.valid() &&
      m_setupFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
    return;
  if (m_statusFuture.valid()) {
    if (m_statusFuture.wait_for(std::chrono::seconds(0)) != std::future_status::ready)
      return;
    bool enabled = false;
    try { enabled = m_statusFuture.get(); }
    catch (...) { return; }
    if (m_statusGeneration != m_setupGeneration) return;
    const int previous = s_enabledCache.exchange(enabled ? 1 : 0);
    if (previous != (enabled ? 1 : 0)) {
      if (!enabled) {
        // Old snapshots must not keep the picker/restore gate looking live,
        // nor prove health after an external disable/re-enable cycle.
        m_hasSnapshot = false;
        QMutexLocker lock(&m_mutex);
        m_windows.clear();
      }
      emit windowsUpdated();
    }
    return;
  }
  m_statusGeneration = m_setupGeneration;
  try {
    m_statusFuture = std::async(std::launch::async, [] {
      return queryScriptEnabledLive();
    });
  } catch (const std::exception &error) {
    qWarning() << "KWin backend: cannot refresh script status:" << error.what();
  }
}

QList<WindowInfo> KWinBackend::parseSnapshot(const QByteArray &json, bool *valid) {
  if (valid) *valid = false;
  QList<WindowInfo> out;
  QJsonParseError err;
  const QJsonDocument doc = QJsonDocument::fromJson(json, &err);
  if (err.error != QJsonParseError::NoError || !doc.isArray())
    return out;
  if (valid) *valid = true;

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
  // The script is semi-trusted local input; bound what we parse so a
  // runaway snapshot cannot balloon the GUI thread.
  static constexpr qsizetype kMaxSnapshotChars = 1024 * 1024; // ~1 MiB
  if (json.size() > kMaxSnapshotChars) {
    qWarning() << "KWin backend: dropping oversized snapshot ("
               << json.size() << "chars)";
    return;
  }
  bool valid = false;
  const QList<WindowInfo> parsed = parseSnapshot(json.toUtf8(), &valid);
  if (!valid)
    return;
  m_hasSnapshot = true;
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

  // 2. Our own install prefix (AppImage included): the bundled copy
  // matches the running code, so it wins over any copy an older
  // install left in the shared datadir.
  const QString bundled =
      findScriptSourceDirForAppDir(QCoreApplication::applicationDirPath());
  if (!bundled.isEmpty())
    return bundled;

  // 3. Installed location: <datadir>/trophy/kwin/trophy-monitor.
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

QString KWinBackend::findScriptSourceDirForAppDir(const QString &appDir) {
  const QString candidate =
      prefixFromBinaryDir(appDir) + "/share/trophy/kwin/trophy-monitor";
  if (QFile::exists(candidate + "/metadata.json"))
    return candidate;
  return QString();
}

bool KWinBackend::runTool(const QString &tool, const QStringList &args,
                          QString *output) {
  if (s_toolOverride) return s_toolOverride(tool, args, output);
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
  // Cache-only: the live query spawns kreadconfig (10 s timeout, plus a
  // second-generation fallback), which used to stall status-label reads on
  // the GUI thread. Unknown until the first setup sequence reports.
  return s_enabledCache.load(std::memory_order_acquire) == 1;
}

bool KWinBackend::queryScriptEnabledLive() {
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
    if (changed)
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
      if (changed)
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
  if (changed)
    *changed = true;
  return isScriptInstalled();
}

bool KWinBackend::ensureScriptInstalled() {
  bool installed = false;
  enqueueOperation([&installed] {
    bool changed = false;
    installed = installScript(&changed);
  }).get();
  return installed;
}

bool KWinBackend::removeInstalledScript() {
  bool removed = false;
  enqueueOperation([&removed] { removed = removeInstalledScriptSync(); }).get();
  return removed;
}

// Worker core. GUI callers enqueue through removeInstalledScriptAsync;
// the direct synchronous wrapper retains the tested on-return contract.
bool KWinBackend::removeInstalledScriptSync() {
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
  // Clear the stale enable key either way (best effort). Both tool
  // generations write the same key in the same file, so the second only
  // runs if the first failed — halving the worst-case block of this
  // synchronous opt-out path.
  //
  // Repeat-removal fast path: with no package present and the cache already
  // reading disabled, our own prior removal already wrote "false", so both
  // kwriteconfig spawns (up to ~20 s worst case when both tool generations
  // are present but hung) are skipped and this degrades to two QFile::exists
  // stats. Sound because a "true" key with no installed package is a KWin
  // no-op, and the status label reads the same cache we consult. Unknown
  // cache (-1) stays conservative and still writes.
  const bool needKeyClear =
      wasPresent || s_enabledCache.load(std::memory_order_acquire) != 0;
  if (needKeyClear) {
    if (!runTool("kwriteconfig6", {"--file", "kwinrc", "--group", "Plugins",
                                   "--key", QString(kScriptId) + "Enabled",
                                   "false"}))
      runTool("kwriteconfig5", {"--file", "kwinrc", "--group", "Plugins",
                                "--key", QString(kScriptId) + "Enabled",
                                "false"});
  }
  if (gone && wasPresent)
    reconfigureKWinStatic();
  if (gone)
    s_enabledCache.store(0, std::memory_order_release);
  return gone;
}

void KWinBackend::reconfigureKWinStatic() {
  if (s_restartOverride) { s_restartOverride(); return; }
  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected())
    return;
  QDBusMessage msg = QDBusMessage::createMethodCall(
      "org.kde.KWin", "/KWin", "org.kde.KWin", "reconfigure");
  bus.call(msg, QDBus::NoBlock);
}

bool KWinBackend::ensureScriptEnabled(bool *changed) {
  // kwinrc: [Plugins] trophy-monitorEnabled=true
  if (queryScriptEnabledLive()) {
    if (changed)
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
  if (changed)
    *changed = true;
  return true;
}

bool KWinBackend::start(bool allowRepair) {
  if (m_active)
    return true;

  QDBusConnection bus = QDBusConnection::sessionBus();
  if (!bus.isConnected()) {
    qWarning() << "KWin backend: no D-Bus session bus";
    return false;
  }

  if (!bus.registerService(kServiceName)) {
    // Best effort: a previously installed script may still deliver (and
    // the object below is still useful to local callers), so fall through
    // instead of aborting start() entirely.
    qWarning() << "KWin backend: could not own" << kServiceName
               << "(another Trophy instance?) - continuing best-effort";
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

  // The install/enable sequence spawns kpackagetool/kreadconfig/
  // kwriteconfig (10 s timeout each, plus second-generation fallbacks), so
  // it runs on a worker; only D-Bus registration above stays on the owner
  // thread, since the script reports back to this object. The worker calls
  // only static helpers that touch the filesystem and spawn tools — never
  // member state. The owned future settles before destruction, and the
  // owner-thread timer generation-checks delivery without worker QObject use.
  queueSetup(allowRepair);
  return true;
}

void KWinBackend::queueSetup(bool allowRepair) {
  quint64 generation = 0;
  {
    QMutexLocker lg(&m_mutex);
    m_setupGeneration++;
    generation = m_setupGeneration;
  }
  auto result = std::make_shared<std::pair<bool, bool>>(false, false);
  m_setupFuture = enqueueOperation([result, allowRepair]() {
    if (!allowRepair) {
      const bool installed = isScriptInstalled();
      const bool enabled = queryScriptEnabledLive();
      s_enabledCache.store(enabled ? 1 : 0, std::memory_order_release);
      if (!installed || !enabled) {
        // Existing setups need an explicit Repair when removed/disabled
        // externally. Do not silently undo that change at the next launch.
        *result = {installed, enabled};
        return;
      }
    }
    bool scriptChanged = false;
    const bool scriptOk = KWinBackend::installScript(&scriptChanged);
    bool enableChanged = false;
    const bool enabledOk = allowRepair
                               ? KWinBackend::ensureScriptEnabled(&enableChanged)
                               : KWinBackend::queryScriptEnabledLive();
    s_enabledCache.store(enabledOk ? 1 : 0, std::memory_order_release);
    if (scriptOk && enabledOk) {
      if (s_restartOverride) s_restartOverride();
      else {
        auto call = [](const QString &path, const QString &interface,
                       const QString &method, const QVariantList &args) {
          auto msg = QDBusMessage::createMethodCall("org.kde.KWin", path, interface, method);
          msg.setArguments(args);
          return QDBusConnection::sessionBus().call(msg, QDBus::Block, 3000);
        };
        const auto unloaded = call("/Scripting", "org.kde.kwin.Scripting", "unloadScript", {QString(kScriptId)});
        if (unloaded.type() != QDBusMessage::ErrorMessage) {
          const auto loaded = call("/Scripting", "org.kde.kwin.Scripting", "loadScript",
              {installedScriptDir() + "/contents/code/main.js", QString(kScriptId)});
          if (loaded.type() != QDBusMessage::ErrorMessage && !loaded.arguments().isEmpty() && loaded.arguments().first().toInt() >= 0)
            call(QString("/Scripting/Script%1").arg(loaded.arguments().first().toInt()),
                 "org.kde.kwin.Script", "run", {});
        }
      }
    }
    *result = {scriptOk, enabledOk};
  });
  if (m_setupTimer) delete m_setupTimer;
  m_setupTimer = new QTimer(this);
  m_setupTimer->setInterval(50);
  connect(m_setupTimer, &QTimer::timeout, this, [this, generation, result] {
    if (hasPendingWork()) return;
    m_setupTimer->stop();
    try { m_setupFuture.get(); } catch (...) { return; }
    if (generation != m_setupGeneration || !m_active) return;
    if (!result->first || !result->second)
      qWarning() << "KWin backend: script setup incomplete";
    emit windowsUpdated();
  });
  m_setupTimer->start();
}

void KWinBackend::stop() {
  m_hasSnapshot = false;
  if (!m_active && !m_dbusRegistered)
    return;
  {
    QMutexLocker lg(&m_mutex);
    // Supersede any setup worker still in flight: its delivery checks this
    // generation and drops its results. Never waited on here — the worker
    // touches no member state, so detaching is safe and stop() never
    // blocks the GUI thread on tool timeouts. Retirement/destruction owns
    // and settles the future; the worker is not detached.
    m_setupGeneration++;
  }
  if (m_dbusRegistered) {
    QDBusConnection::sessionBus().unregisterObject(kObjectPath);
    QDBusConnection::sessionBus().unregisterService(kServiceName);
    m_dbusRegistered = false;
  }
  m_active = false;
}

} // namespace TrophyBackend
