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
#include <QSettings>
#include <QStandardPaths>
#include <QTextStream>

namespace TrophyBackend {

namespace {
QString settingsKey(const QString &key) { return QStringLiteral("desktopIntegration/") + key; }
}

DesktopIntegration::DesktopIntegration(QObject *parent) : QObject(parent) {}

bool DesktopIntegration::isAppImage() const {
    return InstallInfo::detect() == InstallKind::AppImage;
}

QString DesktopIntegration::appImagePath() const {
    const QByteArray env = qgetenv("APPIMAGE");
    if (!env.isEmpty())
        return QString::fromLocal8Bit(env);
    return QCoreApplication::applicationFilePath();
}

QString DesktopIntegration::stablePath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.local/share");
    return QDir(dir).filePath(QStringLiteral("trophy/Trophy.AppImage"));
}

QString DesktopIntegration::desktopFilePath() const {
    QString dir = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
    if (dir.isEmpty())
        dir = QDir::homePath() + QStringLiteral("/.local/share/applications");
    return QDir(dir).filePath(QStringLiteral("trophy.desktop"));
}

QString DesktopIntegration::iconTargetPath() const {
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
    if (base.isEmpty())
        base = QDir::homePath() + QStringLiteral("/.local/share");
    return QDir(base).filePath(QStringLiteral("icons/hicolor/scalable/apps/trophy.svg"));
}

bool DesktopIntegration::isIntegrated() const {
    const QString stable = stablePath();
    const QString desktop = desktopFilePath();
    if (!QFile::exists(stable) || !QFile::exists(desktop))
        return false;
    // The .desktop must point at the stable binary.
    QFile f(desktop);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return false;
    const QString contents = QString::fromUtf8(f.readAll());
    f.close();
    const QString execLine = QStringLiteral("Exec=") + stable;
    return contents.contains(execLine);
}

bool DesktopIntegration::declined() const {
    QSettings s(QStringLiteral("Trophy"), QStringLiteral("Trophy"));
    return s.value(settingsKey(QStringLiteral("declined")), false).toBool();
}

void DesktopIntegration::setDeclined(bool declined) {
    QSettings s(QStringLiteral("Trophy"), QStringLiteral("Trophy"));
    s.setValue(settingsKey(QStringLiteral("declined")), declined);
    s.sync();
    emit declinedChanged();
}

bool DesktopIntegration::integrate() {
    if (!isAppImage()) {
        emit integrationFinished(false, QStringLiteral("Not running as an AppImage."));
        return false;
    }
    if (isIntegrated()) {
        emit integrationFinished(true, QStringLiteral("Already integrated."));
        return true;
    }

    const QString src = appImagePath();
    if (src.isEmpty() || !QFile::exists(src)) {
        emit integrationFinished(false, QStringLiteral("AppImage path not found."));
        return false;
    }

    // 1. Copy to stable path (if not already there / size mismatch).
    const QString stable = stablePath();
    QDir().mkpath(QFileInfo(stable).absolutePath());
    const QFileInfo srcInfo(src);
    const QFileInfo stableInfo(stable);
    if (!stableInfo.exists() || stableInfo.size() != srcInfo.size()) {
        QFile::remove(stable);
        if (!QFile::copy(src, stable)) {
            emit integrationFinished(false, QStringLiteral("Could not copy AppImage to stable path."));
            return false;
        }
        QFile::setPermissions(stable, QFile::ReadOwner | QFile::WriteOwner | QFile::ExeOwner |
                                          QFile::ReadGroup | QFile::ExeGroup |
                                          QFile::ReadOther | QFile::ExeOther);
    }

    // 2. Write .desktop.
    const QString desktop = desktopFilePath();
    QDir().mkpath(QFileInfo(desktop).absolutePath());
    QFile df(desktop);
    if (!df.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        emit integrationFinished(false, QStringLiteral("Could not write .desktop file."));
        return false;
    }
    QTextStream ts(&df);
    ts << "[Desktop Entry]\n";
    ts << "Name=Trophy\n";
    ts << "GenericName=Game recording and clipping\n";
    ts << "Comment=Game recording and clipping\n";
    ts << "Exec=" << stable << "\n";
    ts << "Icon=trophy\n";
    ts << "Terminal=false\n";
    ts << "Type=Application\n";
    ts << "Categories=AudioVideo;Video;Game;\n";
    ts << "Keywords=game;recording;capture;video;gameplay;\n";
    ts << "StartupNotify=true\n";
    ts << "StartupWMClass=trophy\n";
    df.close();

    // 3. Install icon (best effort, from the AppImage's bundled icon).
    const QString appDir = QString::fromLocal8Bit(qgetenv("APPDIR"));
    QString srcIcon = appDir + QStringLiteral("/usr/share/icons/hicolor/scalable/apps/trophy.svg");
    if (!QFile::exists(srcIcon))
        srcIcon = appDir + QStringLiteral("/usr/share/icons/trophy.svg");
    if (QFile::exists(srcIcon)) {
        const QString iconTarget = iconTargetPath();
        QDir().mkpath(QFileInfo(iconTarget).absolutePath());
        QFile::remove(iconTarget);
        QFile::copy(srcIcon, iconTarget);
    }

    // 4. Refresh caches (best effort).
    refreshCaches(desktop);

    emit integrationChanged();
    emit integrationFinished(true, QStringLiteral("Desktop integration complete. Restart Trophy to use portal features."));
    return true;
}

bool DesktopIntegration::refreshCaches(const QString &desktopPath) {
    const QString dir = QFileInfo(desktopPath).absolutePath();
    QProcess::execute(QStringLiteral("update-desktop-database"), QStringList() << dir);
    // Icon cache (optional).
    const QString iconBase = QFileInfo(iconTargetPath()).absolutePath().section(QStringLiteral("/icons/"), 0, 0) + QStringLiteral("/icons");
    QProcess::execute(QStringLiteral("gtk-update-icon-cache"), QStringList() << QStringLiteral("-f") << iconBase);
    return true;
}

} // namespace TrophyBackend