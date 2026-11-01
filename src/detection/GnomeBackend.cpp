/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "GnomeBackend.h"

#include <QDBusConnection>
#include <QDBusMessage>
#include <QDebug>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QSaveFile>
#include <QScopeGuard>
#include <QStandardPaths>
#include <mutex>

namespace TrophyBackend {
namespace {
// Serializes explicit repairs across monitor stop/restart generations.
std::mutex setupMutex;
constexpr qsizetype maxSnapshotBytes = 1024 * 1024;
}

GnomeBackend::GnomeBackend(QObject *parent) : QObject(parent) {}

GnomeBackend::~GnomeBackend() {
  stop();
  if (m_setup.valid()) m_setup.wait();
}

void GnomeBackend::stop() {
  QMutexLocker lock(&m_mutex);
  ++m_generation;
  m_running = false;
  m_active = false;
  m_windows.clear();
  m_lastSignature.clear();
  m_status.clear();
}

QString GnomeBackend::extensionDirectory() {
  return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) +
         "/gnome-shell/extensions/" + kUuid;
}

bool GnomeBackend::isInstalled() {
  const QDir dir(extensionDirectory());
  return QFileInfo::exists(dir.filePath("metadata.json")) &&
         QFileInfo::exists(dir.filePath("extension.js"));
}

QString GnomeBackend::installHelper(const QString &directory) {
  std::lock_guard lock(setupMutex);
  const QFileInfo destination(directory);
  if (destination.isSymLink() || (destination.exists() && !destination.isDir()))
    return tr("Cannot install the GNOME helper: destination is not a regular directory.");
  // Never overwrite an unrelated extension or follow a replaced file symlink.
  if (destination.exists() && !QDir(directory).isEmpty()) {
    QFile metadata(QDir(directory).filePath("metadata.json"));
    if (!metadata.open(QIODevice::ReadOnly) ||
        QJsonDocument::fromJson(metadata.readAll()).object().value("uuid").toString() != kUuid)
      return tr("Cannot install the GNOME helper: an unrecognized extension occupies its directory.");
  }
  if (!QDir().mkpath(directory))
    return tr("Cannot create the GNOME helper directory.");
  // Metadata last: a new incomplete directory is not advertised to the shell.
  for (const QString &name : {QStringLiteral("extension.js"), QStringLiteral("metadata.json")}) {
    QFile source(":/gnome/" + name);
    const QString path = QDir(directory).filePath(name);
    if (QFileInfo(path).isSymLink() || !source.open(QIODevice::ReadOnly))
      return tr("Cannot read or safely install the bundled GNOME helper.");
    const QByteArray bytes = source.readAll();
    QSaveFile output(path);
    if (bytes.isEmpty() || !output.open(QIODevice::WriteOnly) ||
        output.write(bytes) != bytes.size() || !output.commit())
      return tr("Cannot write the GNOME helper files.");
  }

  // Create an autostart entry that enables the extension on next login, since
  // GNOME Shell only discovers new extensions at startup. This avoids requiring
  // the user to manually run `gnome-extensions enable` after logging back in.
  const QString autostartDir = QStandardPaths::writableLocation(QStandardPaths::ConfigLocation) + "/autostart";
  const QString autostartPath = autostartDir + "/trophy-gnome-helper-enable.desktop";
  QDir().mkpath(autostartDir);
  QFile autostartFile(autostartPath);
  if (autostartFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
    const QByteArray autostartContent =
        ("[Desktop Entry]\n"
         "Type=Application\n"
         "Name=Trophy GNOME Helper Enable\n"
         "Comment=One-time automatic enable of the Trophy Window Helper GNOME extension installed by Trophy\n"
         "Exec=sh -c \"gnome-extensions enable trophy-monitor@trophy; rm -f '" + autostartPath.toUtf8() + "'\"\n"
         "Terminal=false\n"
         "NoDisplay=true\n"
         "X-GNOME-Autostart-enabled=true\n");
    autostartFile.write(autostartContent);
    autostartFile.close();
  }

  const QString exe = QStandardPaths::findExecutable("gnome-extensions");
  if (exe.isEmpty())
    return tr("Helper installed. Log out and back in; it will be enabled automatically on your next login.");
  QProcess process;
  process.start(exe, {"enable", kUuid});
  if (!process.waitForFinished(3000)) {
    process.kill();
    process.waitForFinished(1000);
    return tr("Helper installed. Log out and back in; it will be enabled automatically on your next login. (Enabling timed out this session.)");
  }
  if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0)
    return tr("Helper installed. Log out and back in; it will be enabled automatically on your next login. Check that your GNOME version is supported.");
  // Enable succeeded (shell already knew about it): remove the autostart entry.
  QFile::remove(autostartPath);
  return tr("Helper enabled. Waiting for GNOME to report windows. If it does not, check GNOME Extensions for errors or log out and back in.");
}

bool GnomeBackend::start(bool repair) {
  if (m_running.exchange(true)) return true;
  ++m_generation;
  if (!QDBusConnection::sessionBus().isConnected()) {
    m_running = false;
    return false;
  }
  if (repair) {
    const QString directory = extensionDirectory();
    m_setupPending->store(true);
    try {
      m_setup = std::async(std::launch::async, [directory, pending = m_setupPending] {
        const auto settled = qScopeGuard([&] { pending->store(false); });
        return installHelper(directory);
      });
    } catch (const std::exception &e) {
      m_setupPending->store(false);
      qWarning() << "Cannot start GNOME helper setup:" << e.what();
      QMutexLocker lock(&m_mutex);
      m_status = tr("Cannot start GNOME helper installation. Please retry Repair.");
    }
  }
  qInfo() << "GNOME window tracking armed (Trophy Shell extension)";
  return true;
}

QList<WindowInfo> GnomeBackend::parseSnapshot(const QString &result) {
  QList<WindowInfo> out;
  if (result.size() > maxSnapshotBytes) return out;
  QString json = result.trimmed();
  // Retain compatibility with old, double-stringified parser fixtures.
  if (json.startsWith('"')) {
    const auto wrapper = QJsonDocument::fromJson(("[" + json + "]").toUtf8());
    if (wrapper.isArray() && wrapper.array().size() == 1 && wrapper.array().first().isString())
      json = wrapper.array().first().toString();
  }
  const QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8());
  if (!doc.isArray()) return out;
  for (const auto &value : doc.array()) {
    if (!value.isObject()) continue;
    const WindowInfo w = WindowInfo::fromJson(value.toObject());
    if (w.id || !w.appId.isEmpty() || !w.title.isEmpty()) out.append(w);
  }
  return out;
}

QList<WindowInfo> GnomeBackend::windows() const {
  QMutexLocker lock(&m_mutex);
  return m_windows;
}

QString GnomeBackend::status() const {
  QMutexLocker lock(&m_mutex);
  return m_status;
}

QString GnomeBackend::signatureFor(const QList<WindowInfo> &windows) {
  if (windows.isEmpty()) return {};
  // Use structured serialization: titles/app ids can themselves contain separators.
  QJsonArray rows;
  for (const auto &w : windows)
    rows.append(QJsonArray{QString::number(w.id), w.appId, w.title,
        QString::number(w.pid), w.fullscreen, w.focused, w.minimized,
        w.x, w.y, w.width, w.height, w.outputWidth, w.outputHeight});
  return QString::fromUtf8(QJsonDocument(rows).toJson(QJsonDocument::Compact));
}

void GnomeBackend::unavailable(const QString &reason, quint64 generation) {
  bool changed;
  {
    QMutexLocker lock(&m_mutex);
    if (!m_running || generation != m_generation) return;
    changed = m_active.exchange(false) || !m_windows.isEmpty() || m_status != reason;
    m_windows.clear();
    m_lastSignature.clear();
    m_status = reason;
  }
  if (changed) emit windowsUpdated();
}

void GnomeBackend::refresh() {
  if (!m_running) return;
  const quint64 generation = m_generation.load();
  if (m_setup.valid()) {
    const QString result = m_setup.get();
    if (!m_running || generation != m_generation) return;
    {
      QMutexLocker lock(&m_mutex);
      m_status = result;
    }
    emit windowsUpdated();
  }
  auto request = QDBusMessage::createMethodCall(kService, kPath, kInterface, "GetWindows");
  const QDBusMessage reply = QDBusConnection::sessionBus().call(request, QDBus::Block, 2000);
  if (!m_running || generation != m_generation) return;
  const auto args = reply.arguments();
  const QString json = args.size() == 1 && args.first().metaType().id() == QMetaType::QString
      ? args.first().toString() : QString();
  const QJsonDocument document = json.toUtf8().size() <= maxSnapshotBytes
      ? QJsonDocument::fromJson(json.toUtf8()) : QJsonDocument();
  bool valid = document.isArray();
  if (valid) {
    for (const auto &entry : document.array()) {
      if (!entry.isObject() || entry.toObject().isEmpty()) {
        valid = false;
        break;
      }
    }
  }
  if (reply.type() == QDBusMessage::ErrorMessage || json.toUtf8().size() > maxSnapshotBytes ||
      !valid) {
    QString reason = status();
    if (reason.isEmpty() || m_active)
      reason = isInstalled()
          ? tr("Helper installed. Log out and back in; it will be enabled automatically on your next login.")
          : tr("GNOME helper is not installed. Choose Install helper. A logout/login may be required after installation.");
    unavailable(reason, generation);
    return;
  }
  const auto parsed = parseSnapshot(json);
  const QString signature = signatureFor(parsed);
  bool changed;
  {
    QMutexLocker lock(&m_mutex);
    if (!m_running || generation != m_generation) return;
    changed = !m_active.exchange(true) || signature != m_lastSignature;
    m_windows = parsed;
    m_lastSignature = signature;
    m_status = tr("Working");
  }
  if (changed) emit windowsUpdated();
}
} // namespace TrophyBackend
