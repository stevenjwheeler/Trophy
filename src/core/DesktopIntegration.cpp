/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DesktopIntegration.h"
#include "InstallInfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QSaveFile>
#include <QStandardPaths>

namespace TrophyBackend {

namespace {

QString desktopFileName() { return QStringLiteral("trophy.desktop"); }

// Quote one Exec argument per the Desktop Entry spec: the value stays a single
// argument even when the path contains spaces.
QString quoteExecArg(const QString &path) {
    QString out = path;
    out.replace(QLatin1Char('\\'), QStringLiteral("\\\\"));
    out.replace(QLatin1Char('"'), QStringLiteral("\\\""));
    out.replace(QLatin1Char('`'), QStringLiteral("\\`"));
    out.replace(QLatin1Char('$'), QStringLiteral("\\$"));
    return QLatin1Char('"') + out + QLatin1Char('"');
}

QString unescapeDesktopString(const QString &in) {
    QString out;
    out.reserve(in.size());
    for (int i = 0; i < in.size(); ++i) {
        if (in.at(i) == QLatin1Char('\\') && i + 1 < in.size()) {
            out += in.at(++i);
            continue;
        }
        out += in.at(i);
    }
    return out;
}

// Split a desktop-entry Exec value into arguments, honouring double quotes and
// backslash escapes. Enough for the entries app launchers write.
QStringList splitExec(const QString &value) {
    QStringList out;
    QString current;
    bool quoted = false;
    bool have = false;
    for (int i = 0; i < value.size(); ++i) {
        const QChar c = value.at(i);
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            have = true;
            continue;
        }
        if (c == QLatin1Char('\\') && i + 1 < value.size()) {
            current += value.at(++i);
            have = true;
            continue;
        }
        if (c.isSpace() && !quoted) {
            if (have) {
                out << current;
                current.clear();
                have = false;
            }
            continue;
        }
        current += c;
        have = true;
    }
    if (have)
        out << current;
    return out;
}

bool looksLikeAssignment(const QString &token) {
    const int eq = token.indexOf(QLatin1Char('='));
    if (eq <= 0)
        return false;
    for (int i = 0; i < eq; ++i) {
        const QChar c = token.at(i);
        if (!(c.isLetterOrNumber() || c == QLatin1Char('_')))
            return false;
    }
    return true;
}

// The program an entry launches. Handles the "env VAR=value program" form that
// AppImage managers write, which a naive first-token read reports as "env".
QString execProgram(const QString &desktopPath) {
    QFile f(desktopPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.startsWith(QStringLiteral("Exec=")))
            continue;
        QStringList tokens = splitExec(line.mid(5).trimmed());
        int i = 0;
        if (i < tokens.size() && QFileInfo(tokens.at(i)).fileName() == QStringLiteral("env"))
            ++i;
        while (i < tokens.size() && looksLikeAssignment(tokens.at(i)))
            ++i;
        return i < tokens.size() ? tokens.at(i) : QString();
    }
    return QString();
}

// Run a cache-refresh helper without letting a stuck one freeze the GUI.
bool runBounded(const QString &program, const QStringList &args, int timeoutMs) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForStarted(timeoutMs))
        return false;
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(1000);
        return false;
    }
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}

} // namespace

DesktopIntegration::DesktopIntegration(QObject *parent)
    : DesktopIntegration(QString(), QStringList(), parent) {}

DesktopIntegration::DesktopIntegration(const QString &dataHome,
                                       const QStringList &systemDataDirs,
                                       QObject *parent)
    : QObject(parent), m_dataHome(dataHome), m_systemDataDirs(systemDataDirs) {
    m_startedRegistered = isRegistered();
}

bool DesktopIntegration::isAppImage() const {
    if (m_appImageCache < 0)
        m_appImageCache = InstallInfo::detect() == InstallKind::AppImage ? 1 : 0;
    return m_appImageCache == 1;
}

QString DesktopIntegration::dataHome() const {
    if (!m_dataHome.isEmpty())
        return m_dataHome;
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.local/share");
    return dir;
}

namespace {

QStringList applicationsDirsFor(const QString &dataHome, const QStringList &systemDataDirs) {
    QStringList dirs;
    QString home = dataHome;
    if (home.isEmpty()) {
        home = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
        if (home.isEmpty())
            home = QDir::homePath() + QStringLiteral("/.local/share");
    }
    dirs << home + QStringLiteral("/applications");
    QStringList system = systemDataDirs;
    if (system.isEmpty()) {
        const QByteArray env = qgetenv("XDG_DATA_DIRS");
        system = env.isEmpty()
                     ? QStringList{QStringLiteral("/usr/local/share"), QStringLiteral("/usr/share")}
                     : QString::fromLocal8Bit(env).split(QLatin1Char(':'), Qt::SkipEmptyParts);
    }
    for (const QString &dir : system) {
        const QString trimmed = dir.trimmed();
        if (!trimmed.isEmpty())
            dirs << trimmed + QStringLiteral("/applications");
    }
    dirs.removeDuplicates();
    return dirs;
}

} // namespace

QString DesktopIntegration::portalDesktopFile(const QString &dataHome,
                                             const QStringList &systemDataDirs) {
    const QStringList dirs = applicationsDirsFor(dataHome, systemDataDirs);
    for (const QString &dir : dirs) {
        const QString candidate = QDir(dir).filePath(desktopFileName());
        if (QFileInfo::exists(candidate))
            return candidate;
    }
    return QString();
}

QStringList DesktopIntegration::applicationsDirs() const {
    return applicationsDirsFor(dataHome(), m_systemDataDirs);
}

QString DesktopIntegration::appImagePath() const {
    const QByteArray env = qgetenv("APPIMAGE");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QCoreApplication::applicationFilePath();
}

QString DesktopIntegration::desktopFilePath() const {
    return QDir(dataHome()).filePath(QStringLiteral("applications/") + desktopFileName());
}

QString DesktopIntegration::iconTargetPath() const {
    return QDir(dataHome()).filePath(QStringLiteral("icons/hicolor/scalable/apps/trophy.svg"));
}

QString DesktopIntegration::registeredDesktopFile() const {
    return portalDesktopFile(dataHome(), m_systemDataDirs);
}

bool DesktopIntegration::isIntegrated() const {
    const QString desktop = desktopFilePath();
    if (!QFileInfo::exists(desktop))
        return false;
    const QString target = execProgram(desktop);
    return !target.isEmpty() && QFileInfo::exists(target);
}

bool DesktopIntegration::restartRequired() const {
    return !m_startedRegistered && isRegistered();
}

bool DesktopIntegration::isRegistered() const {
    return !registeredDesktopFile().isEmpty();
}

QString DesktopIntegration::statusMessage() const { return m_statusMessage; }

bool DesktopIntegration::statusIsError() const { return m_statusIsError; }

QString DesktopIntegration::competingLauncher() const {
    const QDir dir(QDir(dataHome()).filePath(QStringLiteral("applications")));
    if (!dir.exists())
        return QString();
    const QStringList entries =
        dir.entryList(QStringList{QStringLiteral("*.desktop")}, QDir::Files);
    for (const QString &name : entries) {
        const QString path = dir.filePath(name);
        if (path == desktopFilePath())
            continue;
        // Only portable installs count: a distribution package's entry names
        // /usr/bin/trophy and must not hide our AppImage entry.
        const QString exec = execProgram(path);
        if (exec.isEmpty())
            continue;
        const QString base = QFileInfo(exec).fileName();
        if (base.contains(QStringLiteral("trophy"), Qt::CaseInsensitive) &&
            base.contains(QStringLiteral("appimage"), Qt::CaseInsensitive))
            return path;
    }
    return QString();
}

bool DesktopIntegration::entryHidden() const { return m_entryHidden; }

QString DesktopIntegration::desktopEntryContents(const QString &appImage, bool hidden) const {
    QString out;
    out += QStringLiteral("[Desktop Entry]\n");
    out += QStringLiteral("Name=Trophy\n");
    out += QStringLiteral("GenericName=Game recording and clipping\n");
    out += QStringLiteral("Comment=Game recording and clipping\n");
    out += QStringLiteral("Exec=") + quoteExecArg(appImage) + QStringLiteral("\n");
    out += QStringLiteral("Icon=trophy\n");
    out += QStringLiteral("Terminal=false\n");
    out += QStringLiteral("Type=Application\n");
    out += QStringLiteral("Categories=AudioVideo;Video;Game;\n");
    out += QStringLiteral("Keywords=game;recording;capture;video;gameplay;\n");
    out += QStringLiteral("StartupNotify=true\n");
    out += QStringLiteral("StartupWMClass=trophy\n");
    // Still resolvable by the portal (which is all the app id needs), but not
    // a second icon when another manager already lists Trophy.
    if (hidden)
        out += QStringLiteral("NoDisplay=true\n");
    return out;
}

bool DesktopIntegration::applyStatus(const QString &message, bool isError) {
    m_statusMessage = message;
    m_statusIsError = isError;
    emit statusMessageChanged();
    emit integrationFinished(!isError, message);
    return !isError;
}

bool DesktopIntegration::installIcon() {
    const QString appDir = QString::fromLocal8Bit(qgetenv("APPDIR"));
    if (appDir.isEmpty())
        return false;
    const QStringList candidates{
        appDir + QStringLiteral("/usr/share/icons/hicolor/scalable/apps/trophy.svg"),
        appDir + QStringLiteral("/usr/share/icons/trophy.svg")};
    QString source;
    for (const QString &candidate : candidates) {
        if (QFileInfo::exists(candidate)) {
            source = candidate;
            break;
        }
    }
    if (source.isEmpty())
        return false;

    const QString target = iconTargetPath();
    const QFileInfo targetInfo(target);
    if (targetInfo.exists() && targetInfo.size() == QFileInfo(source).size())
        return false;
    QDir().mkpath(QFileInfo(target).absolutePath());
    QFile::remove(target);
    return QFile::copy(source, target);
}

void DesktopIntegration::refreshCaches(const QString &applicationsDir, bool iconChanged) const {
    runBounded(QStringLiteral("update-desktop-database"),
               QStringList() << applicationsDir, 2000);
    if (iconChanged) {
        const QString iconRoot =
            QFileInfo(iconTargetPath()).absolutePath().section(QStringLiteral("/icons/"), 0, 0) +
            QStringLiteral("/icons");
        runBounded(QStringLiteral("gtk-update-icon-cache"),
                   QStringList() << QStringLiteral("-q") << QStringLiteral("-t") << iconRoot,
                   3000);
    }
}

void DesktopIntegration::refreshVisibility() {
    // Never touch an entry we did not write: a distribution package owns its
    // own Exec line and must not be rewritten to the running binary's path.
    if (!isAppImage())
        return;
    const QString desktop = desktopFilePath();
    if (!QFileInfo::exists(desktop))
        return;
    const bool hidden = !competingLauncher().isEmpty();
    m_entryHidden = hidden;
    // Only the visibility flag is refreshed here. Repointing Exec at the
    // currently running file would silently move the registration to another
    // manager's copy just because that entry launched this process; the
    // registered target is kept while it still exists, and integrate() is what
    // deliberately repoints it.
    QString target = execProgram(desktop);
    if (target.isEmpty() || !QFileInfo(target).isFile())
        target = appImagePath();
    if (target.isEmpty() || !QFileInfo(target).isFile())
        return;
    const QByteArray wanted = desktopEntryContents(target, hidden).toUtf8();
    QFile existing(desktop);
    if (existing.open(QIODevice::ReadOnly) && existing.readAll() == wanted) {
        existing.close();
        return;
    }
    existing.close();
    QSaveFile entry(desktop);
    if (!entry.open(QIODevice::WriteOnly | QIODevice::Text))
        return;
    entry.write(wanted);
    if (entry.commit())
        emit integrationChanged();
}

bool DesktopIntegration::integrate() {
    if (!isAppImage())
        return applyStatus(QStringLiteral("Not running as an AppImage."), true);

    const QString appImage = appImagePath();
    if (appImage.isEmpty() || !QFileInfo(appImage).isFile())
        return applyStatus(QStringLiteral("Could not locate the AppImage to register."), true);

    // The mounted AppImage runtime path is not a durable Exec target; the
    // entry must name the .AppImage file the user launched.
    const QByteArray appDirEnv = qgetenv("APPDIR");
    if (!appDirEnv.isEmpty()) {
        const QString appDir = QFileInfo(QString::fromLocal8Bit(appDirEnv)).canonicalFilePath();
        const QString canonicalImage = QFileInfo(appImage).canonicalFilePath();
        if (!appDir.isEmpty() && !canonicalImage.isEmpty() &&
            canonicalImage.startsWith(appDir + QLatin1Char('/'))) {
            return applyStatus(
                QStringLiteral("Run the .AppImage file directly so it can be registered."), true);
        }
    }

    const QString desktop = desktopFilePath();
    const QString competing = competingLauncher();
    m_entryHidden = !competing.isEmpty();
    const QByteArray wanted = desktopEntryContents(appImage, m_entryHidden).toUtf8();
    bool entryChanged = true;
    QFile existing(desktop);
    if (existing.open(QIODevice::ReadOnly)) {
        entryChanged = existing.readAll() != wanted;
        existing.close();
    }
    if (entryChanged) {
        QDir().mkpath(QFileInfo(desktop).absolutePath());
        QSaveFile entry(desktop);
        if (!entry.open(QIODevice::WriteOnly | QIODevice::Text))
            return applyStatus(QStringLiteral("Could not write the desktop entry."), true);
        entry.write(wanted);
        if (!entry.commit())
            return applyStatus(QStringLiteral("Could not write the desktop entry."), true);
    }

    const bool iconChanged = installIcon();
    if (entryChanged || iconChanged) {
        refreshCaches(QFileInfo(desktop).absolutePath(), iconChanged);
        emit integrationChanged();
    }

    if (entryChanged || iconChanged) {
        if (m_entryHidden) {
            return applyStatus(
                QStringLiteral("Registered. The menu entry is hidden because %1 already "
                               "lists Trophy; launch it from there.")
                    .arg(QFileInfo(competing).fileName()),
                false);
        }
        return applyStatus(QStringLiteral("Desktop entry installed."), false);
    }
    return applyStatus(QStringLiteral("Already integrated."), false);
}

} // namespace TrophyBackend
