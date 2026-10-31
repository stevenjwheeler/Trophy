/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QObject>
#include <QString>
#include <QStringList>

namespace TrophyBackend {

/**
 * @brief Desktop registration for portable (AppImage) builds.
 *
 * An AppImage is not installed into XDG_DATA_DIRS, so xdg-desktop-portal
 * cannot resolve the "trophy" app id. That is functional, not cosmetic: the
 * portal keys ScreenCast restore tokens by token *and* app id, so an
 * unregistered app stores capture approvals under the empty app id shared
 * with every other unregistered app, and loses them once it becomes
 * registered.
 *
 * Integration writes only $XDG_DATA_HOME/applications/trophy.desktop (plus
 * the icon), naming the AppImage where the user keeps it. The file is never
 * copied or moved: the built-in updater replaces $APPIMAGE in place, so the
 * entry keeps working across updates. Registration is checked the way the
 * portal checks it, so entries installed by a distro package,
 * AppImageLauncher, Gear Lever or by hand also count.
 */
class DesktopIntegration : public QObject {
    Q_OBJECT
    Q_PROPERTY(bool isAppImage READ isAppImage CONSTANT)
    Q_PROPERTY(bool isIntegrated READ isIntegrated NOTIFY integrationChanged)
    Q_PROPERTY(bool isRegistered READ isRegistered NOTIFY integrationChanged)
    Q_PROPERTY(bool restartRequired READ restartRequired NOTIFY integrationChanged)
    Q_PROPERTY(QString appImagePath READ appImagePath CONSTANT)
    Q_PROPERTY(QString desktopFilePath READ desktopFilePath CONSTANT)
    // True when our entry is hidden from menus because another AppImage
    // manager already lists Trophy (the entry still supplies the app id).
    Q_PROPERTY(bool entryHidden READ entryHidden NOTIFY integrationChanged)
    // Non-empty when the identity comes from another launcher's entry (Gear
    // Lever / AppImageLauncher) rather than our own trophy.desktop.
    Q_PROPERTY(QString managedEntry READ managedEntry NOTIFY integrationChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(bool statusIsError READ statusIsError NOTIFY statusMessageChanged)

public:
    explicit DesktopIntegration(QObject *parent = nullptr);

    // Test seam: override XDG_DATA_HOME and XDG_DATA_DIRS. An empty dataHome
    // falls back to QStandardPaths; an empty systemDataDirs falls back to the
    // environment (defaulting to /usr/local/share:/usr/share).
    DesktopIntegration(const QString &dataHome, const QStringList &systemDataDirs,
                       QObject *parent = nullptr);

    // True when running from an AppImage bundle.
    bool isAppImage() const;

    // The AppImage file this process was launched from ($APPIMAGE).
    QString appImagePath() const;

    // Our own entry: $XDG_DATA_HOME/applications/trophy.desktop.
    QString desktopFilePath() const;

    // True when our entry exists and the AppImage it names still exists.
    bool isIntegrated() const;

    // True when any XDG application directory has a trophy.desktop, which is
    // what xdg-desktop-portal resolves the app id against.
    bool isRegistered() const;

    // The entry the portal will use, or empty when there is none.
    QString registeredDesktopFile() const;

    // True when this process started unregistered and has been registered
    // since: the portal app id and the platform theme (native file dialogs)
    // are fixed at startup, so only a new process picks them up.
    bool restartRequired() const;

    // Same lookup without an instance, for main() before QGuiApplication
    // exists. Empty arguments use XDG_DATA_HOME / XDG_DATA_DIRS.
    static QString portalDesktopFile(const QString &dataHome = QString(),
                                     const QStringList &systemDataDirs = QStringList());

    // First-launch install: write the entry before QGuiApplication exists so
    // the identity applies to this very process, with no wizard step and no
    // restart. No-op unless an AppImage, no entry yet, not opted out and no
    // AppImage manager already supplies one.
    static bool installEntryAtStartup(const QString &dataHome = QString(),
                                      const QStringList &systemDataDirs = QStringList());

    // The desktop id this process registers as: our stable "trophy" when such
    // an entry exists, otherwise the entry that launched us (a manager's
    // versioned entry), otherwise "trophy".
    static QString effectiveDesktopId(const QString &dataHome = QString(),
                                      const QStringList &systemDataDirs = QStringList());

    bool entryHidden() const;
    QString managedEntry() const;
    QString statusMessage() const;
    bool statusIsError() const;

    // Write/refresh the entry and icon. Idempotent: unchanged files are left
    // alone. Running a moved/renamed AppImage and integrating again repoints
    // the existing entry.
    Q_INVOKABLE bool integrate();
    // Delete our entry and stop recreating it (Settings > Remove).
    Q_INVOKABLE bool removeEntry();

    // Keep NoDisplay in sync with competingLauncher(): called from integrate(),
    // once at startup and when Settings opens, so adding or removing the other
    // manager's entry brings the menu entry in line without a restart.
    Q_INVOKABLE void refreshVisibility();

signals:
    void integrationChanged();
    void statusMessageChanged();
    void integrationFinished(bool ok, const QString &message);

private:
    QString dataHome() const;
    QStringList applicationsDirs() const;
    QString iconTargetPath() const;
    QString desktopEntryContents(const QString &appImage, bool hidden) const;
    // Another .desktop in the user's application dir already launches an
    // AppImage called Trophy (Gear Lever / AppImageLauncher). Returns that
    // entry's path, or empty.
    QString competingLauncher() const;
    bool installIcon();
    void refreshCaches(const QString &applicationsDir, bool iconChanged) const;
    bool applyStatus(const QString &message, bool isError);
    bool writeEntry(const QString &appImage);
    static bool autoInstallEnabled();

    QString m_dataHome;
    QStringList m_systemDataDirs;
    bool m_startedRegistered = true;
    bool m_entryHidden = false;
    // InstallInfo::detect() shells out for native installs; QML reads
    // isAppImage() for every dialog binding, so resolve it once.
    mutable int m_appImageCache = -1;
    QString m_statusMessage;
    bool m_statusIsError = false;
};

} // namespace TrophyBackend
