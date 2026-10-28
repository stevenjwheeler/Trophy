/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "InstallInfo.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStringList>

namespace TrophyBackend {

namespace {
bool runQuery(const QString &program, const QStringList &args) {
    QProcess p;
    p.start(program, args);
    if (!p.waitForFinished(3000))
        return false;
    return p.exitStatus() == QProcess::NormalExit && p.exitCode() == 0;
}
} // namespace

InstallKind InstallInfo::detect() {
    // Testing hook: override the detected install kind so the update
    // UI can be exercised for every supported format without
    // reinstalling. Values: appimage, deb, rpm, tar/portable, unknown.
    const QByteArray overrideKind = qgetenv("TROPHY_FAKE_INSTALL_KIND");
    if (!overrideKind.isEmpty()) {
        const QString k = QString::fromUtf8(overrideKind).trimmed().toLower();
        if (k == "appimage")
            return InstallKind::AppImage;
        if (k == "deb" || k == "debian")
            return InstallKind::DebianPackage;
        if (k == "rpm")
            return InstallKind::RpmPackage;
        if (k == "tar" || k == "portable" || k == "tar.xz")
            return InstallKind::PortablePrefix;
        if (k == "unknown")
            return InstallKind::Unknown;
    }

    const QString appPath = QCoreApplication::applicationFilePath();
    const QFileInfo appInfo(appPath);
    const QString appDir = appInfo.canonicalFilePath().section('/', 0, -2);

    // AppImage: the runtime sets APPIMAGE to the original file path and
    // APPDIR to the mount root. Require the executable to actually live
    // under APPDIR so an inherited env var cannot misclassify a native
    // install.
    const QByteArray appImageEnv = qgetenv("APPIMAGE");
    const QByteArray appDirEnv = qgetenv("APPDIR");
    if (!appImageEnv.isEmpty() && !appDirEnv.isEmpty()) {
        const QString appDirCanonical =
            QFileInfo(QString::fromLocal8Bit(appDirEnv)).canonicalFilePath();
        if (!appDirCanonical.isEmpty() && appPath.startsWith(appDirCanonical + '/'))
            return InstallKind::AppImage;
    }

    // Package-manager ownership.
    if (runQuery("dpkg", QStringList() << "-S" << appPath))
        return InstallKind::DebianPackage;
    if (runQuery("rpm", QStringList() << "-qf" << appPath))
        return InstallKind::RpmPackage;

    // Not owned by a package manager and not an AppImage: a manually
    // extracted prefix (tar.xz or ~/.local install).
    return InstallKind::PortablePrefix;
}

QString InstallInfo::name(InstallKind kind) {
    switch (kind) {
    case InstallKind::AppImage:
        return QStringLiteral("AppImage");
    case InstallKind::DebianPackage:
        return QStringLiteral("Debian package");
    case InstallKind::RpmPackage:
        return QStringLiteral("RPM package");
    case InstallKind::PortablePrefix:
        return QStringLiteral("Portable install");
    case InstallKind::Unknown:
    default:
        return QStringLiteral("Unknown");
    }
}

QString InstallInfo::manualCommand(InstallKind kind, const QString &assetFileName) {
    if (assetFileName.isEmpty())
        return QString();
    switch (kind) {
    case InstallKind::DebianPackage:
        return QStringLiteral("sudo apt install ./%1").arg(assetFileName);
    case InstallKind::RpmPackage:
        return QStringLiteral("sudo dnf install ./%1").arg(assetFileName);
    case InstallKind::PortablePrefix:
        return QStringLiteral("tar -xf %1 -C <prefix>").arg(assetFileName);
    default:
        return QString();
    }
}

} // namespace TrophyBackend