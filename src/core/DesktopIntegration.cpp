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

// First Exec= argument of a desktop entry, unquoted. Empty when missing.
QString execTarget(const QString &desktopPath) {
    QFile f(desktopPath);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    while (!f.atEnd()) {
        const QString line = QString::fromUtf8(f.readLine()).trimmed();
        if (!line.startsWith(QStringLiteral("Exec=")))
            continue;
        QString value = line.mid(5).trimmed();
        if (value.startsWith(QLatin1Char('"'))) {
            value.remove(0, 1);
            const int end = value.lastIndexOf(QLatin1Char('"'));
            if (end >= 0)
                value.truncate(end);
            return unescapeDesktopString(value);
        }
        const int space = value.indexOf(QLatin1Char(' '));
        return space >= 0 ? value.left(space) : value;
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
    : QObject(parent), m_dataHome(dataHome), m_systemDataDirs(systemDataDirs) {}

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
    const QString target = execTarget(desktop);
    return !target.isEmpty() && QFileInfo::exists(target);
}

bool DesktopIntegration::isRegistered() const {
    return !registeredDesktopFile().isEmpty();
}

QString DesktopIntegration::statusMessage() const { return m_statusMessage; }

bool DesktopIntegration::statusIsError() const { return m_statusIsError; }

QString DesktopIntegration::desktopEntryContents(const QString &appImage) const {
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
    const QByteArray wanted = desktopEntryContents(appImage).toUtf8();
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

    return applyStatus(entryChanged || iconChanged
                           ? QStringLiteral("Desktop entry installed. Restart Trophy so the "
                                            "portal uses it.")
                           : QStringLiteral("Already integrated."),
                       false);
}

} // namespace TrophyBackend
