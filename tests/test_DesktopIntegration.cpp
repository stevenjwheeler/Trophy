/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtest/gtest.h>

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTemporaryDir>

#include "core/DesktopIntegration.h"

namespace {

// Saves/restores the environment values the integration helper reads.
class EnvGuard {
public:
    explicit EnvGuard(std::initializer_list<const char *> names) {
        for (const char *name : names) {
            m_names.append(QByteArray(name));
            m_saved.append(qgetenv(name));
        }
    }
    ~EnvGuard() {
        for (int i = 0; i < m_names.size(); ++i) {
            if (m_saved.at(i).isNull())
                qunsetenv(m_names.at(i).constData());
            else
                qputenv(m_names.at(i).constData(), m_saved.at(i));
        }
    }

private:
    QList<QByteArray> m_names;
    QList<QByteArray> m_saved;
};

QString touchFile(const QString &path, const QByteArray &contents = QByteArray()) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return QString();
    f.write(contents);
    f.close();
    return QFileInfo(path).absoluteFilePath();
}

QString readFile(const QString &path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        return QString();
    return QString::fromUtf8(f.readAll());
}

} // namespace

// The entry must name the AppImage the user launched, keeping the path as one
// Exec argument even when it contains spaces.
TEST(DesktopIntegrationTest, EntryNamesTheRunningAppImage) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir home(QStringLiteral("trophy integration XXXXXX"));
    QTemporaryDir images;
    ASSERT_TRUE(home.isValid() && images.isValid());
    const QString image = touchFile(images.filePath(QStringLiteral("My Trophy.AppImage")), "bytes");
    ASSERT_FALSE(image.isEmpty());
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qputenv("APPIMAGE", image.toLocal8Bit());
    qunsetenv("APPDIR");

    TrophyBackend::DesktopIntegration integration(home.path(), QStringList());
    ASSERT_TRUE(integration.isAppImage());
    ASSERT_TRUE(integration.integrate()) << integration.statusMessage().toStdString();
    EXPECT_FALSE(integration.statusIsError());
    EXPECT_TRUE(integration.isIntegrated());
    EXPECT_TRUE(integration.isRegistered());

    const QString entry = readFile(integration.desktopFilePath());
    ASSERT_FALSE(entry.isEmpty());
    EXPECT_TRUE(entry.contains(QStringLiteral("Exec=\"") + image + QStringLiteral("\"")))
        << entry.toStdString();
    EXPECT_TRUE(entry.contains(QStringLiteral("Type=Application")));
    EXPECT_TRUE(entry.contains(QStringLiteral("Icon=trophy")));
    EXPECT_TRUE(entry.contains(QStringLiteral("Terminal=false")));
}

// The user's AppImage is never copied or relocated: the built-in updater
// replaces $APPIMAGE in place, so a stable path is not needed and a second
// copy would only go stale.
TEST(DesktopIntegrationTest, NeverCopiesOrMovesTheAppImage) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir data;
    QTemporaryDir images;
    ASSERT_TRUE(data.isValid() && images.isValid());
    const QString image = touchFile(images.filePath(QStringLiteral("Trophy-0.9.7-x86_64.AppImage")),
                                    "image bytes");
    ASSERT_FALSE(image.isEmpty());
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qputenv("APPIMAGE", image.toLocal8Bit());
    qunsetenv("APPDIR");

    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    ASSERT_TRUE(integration.integrate());

    // Still the only copy, byte-identical, at the path the user chose.
    EXPECT_TRUE(QFileInfo::exists(image));
    EXPECT_EQ(readFile(image), QStringLiteral("image bytes"));
    // Nothing was installed under the data home except the entry.
    const QStringList topLevel = QDir(data.path()).entryList(QDir::AllEntries | QDir::NoDotAndDotDot |
                                                                 QDir::Hidden);
    EXPECT_EQ(topLevel, QStringList{QStringLiteral("applications")})
        << topLevel.join(',').toStdString();
    EXPECT_EQ(QString::fromLocal8Bit(qgetenv("APPIMAGE")), image);
}

// The dialog gate must mirror the portal's own lookup, so entries installed by
// a distro package or a third-party AppImage manager also count.
TEST(DesktopIntegrationTest, RegistrationLooksInEveryXdgApplicationDir) {
    QTemporaryDir home;
    QTemporaryDir systemShare;
    ASSERT_TRUE(home.isValid() && systemShare.isValid());

    TrophyBackend::DesktopIntegration integration(home.path(),
                                                 QStringList{systemShare.path()});
    EXPECT_FALSE(integration.isRegistered());

    const QString systemEntry =
        touchFile(systemShare.filePath(QStringLiteral("applications/trophy.desktop")),
                  "[Desktop Entry]\nExec=/opt/Trophy.AppImage\n");
    EXPECT_TRUE(integration.isRegistered());
    EXPECT_EQ(integration.registeredDesktopFile(), systemEntry);
    // Registered elsewhere, but not by us, so our own entry does not exist.
    EXPECT_FALSE(integration.isIntegrated());

    QFile::remove(systemEntry);
    touchFile(integration.desktopFilePath(), "[Desktop Entry]\nExec=/opt/Trophy.AppImage\n");
    EXPECT_TRUE(integration.isRegistered());
}

// main() resolves the entry before QGuiApplication exists to decide whether
// to select the bundled portal platform theme (which is what actually makes
// file dialogs native).
TEST(DesktopIntegrationTest, StaticLookupFindsThePortalEntryBeforeTheAppExists) {
    QTemporaryDir home;
    QTemporaryDir systemShare;
    ASSERT_TRUE(home.isValid() && systemShare.isValid());

    EXPECT_TRUE(TrophyBackend::DesktopIntegration::portalDesktopFile(
                    home.path(), QStringList{systemShare.path()})
                    .isEmpty());

    const QString systemEntry =
        touchFile(systemShare.filePath(QStringLiteral("applications/trophy.desktop")),
                  "[Desktop Entry]\nExec=/opt/Trophy.AppImage\n");
    EXPECT_EQ(TrophyBackend::DesktopIntegration::portalDesktopFile(
                  home.path(), QStringList{systemShare.path()}),
              systemEntry);

    QFile::remove(systemEntry);
    const QString userEntry = touchFile(
        QDir(home.path()).filePath(QStringLiteral("applications/trophy.desktop")),
        "[Desktop Entry]\nExec=/opt/Trophy.AppImage\n");
    EXPECT_EQ(TrophyBackend::DesktopIntegration::portalDesktopFile(
                  home.path(), QStringList{systemShare.path()}),
              userEntry);
}

// Idempotency: a matching entry is left byte-identical and the call says so.
TEST(DesktopIntegrationTest, IntegrateLeavesAMatchingEntryUntouched) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir home;
    QTemporaryDir data;
    ASSERT_TRUE(home.isValid() && data.isValid());
    const QString image = touchFile(home.filePath(QStringLiteral("Trophy.AppImage")));
    ASSERT_FALSE(image.isEmpty());
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qputenv("APPIMAGE", image.toLocal8Bit());
    qunsetenv("APPDIR");

    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    ASSERT_TRUE(integration.integrate());
    const QString first = readFile(integration.desktopFilePath());
    const QDateTime firstWrite = QFileInfo(integration.desktopFilePath()).lastModified();

    EXPECT_TRUE(integration.integrate());
    EXPECT_EQ(integration.statusMessage(), QStringLiteral("Already integrated."));
    EXPECT_FALSE(integration.statusIsError());
    EXPECT_EQ(readFile(integration.desktopFilePath()), first);
    EXPECT_EQ(QFileInfo(integration.desktopFilePath()).lastModified(), firstWrite);
}

// Repair: a moved or renamed AppImage breaks the entry, and integrating again
// repoints the same entry file (no second entry, no copied binary).
TEST(DesktopIntegrationTest, MovedAppImageIsRepaired) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir home;
    QTemporaryDir data;
    ASSERT_TRUE(home.isValid() && data.isValid());
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qunsetenv("APPDIR");

    const QString oldImage = touchFile(home.filePath(QStringLiteral("Trophy-old.AppImage")));
    ASSERT_FALSE(oldImage.isEmpty());
    qputenv("APPIMAGE", oldImage.toLocal8Bit());
    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    ASSERT_TRUE(integration.integrate());
    ASSERT_TRUE(integration.isIntegrated());

    QFile::remove(oldImage);
    EXPECT_FALSE(integration.isIntegrated());

    const QString newImage = touchFile(home.filePath(QStringLiteral("Trophy.AppImage")));
    qputenv("APPIMAGE", newImage.toLocal8Bit());
    EXPECT_TRUE(integration.integrate());
    EXPECT_FALSE(integration.statusIsError());
    const QString entry = readFile(integration.desktopFilePath());
    EXPECT_TRUE(entry.contains(newImage));
    EXPECT_FALSE(entry.contains(oldImage));
    EXPECT_TRUE(integration.isIntegrated());
    // The old name is gone from disk, the new one is still the only copy.
    EXPECT_TRUE(QFileInfo::exists(newImage));
}

// A manager that versions its entry (Gear Lever) gives Trophy a different app
// id. Our entry still provides the identity, so keep it installed but out of
// the menu: one icon, and launching from the manager's entry still resolves as
// Trophy.
TEST(DesktopIntegrationTest, ManagerEntryMakesOursHiddenButStillRegistered) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir data;
    QTemporaryDir images;
    ASSERT_TRUE(data.isValid() && images.isValid());
    const QString image = touchFile(images.filePath(QStringLiteral("Trophy-0.9.7-x86_64.AppImage")));
    ASSERT_FALSE(image.isEmpty());
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qputenv("APPIMAGE", image.toLocal8Bit());
    qunsetenv("APPDIR");

    // Gear Lever's entry for its own managed copy of Trophy.
    touchFile(QDir(data.path()).filePath(QStringLiteral("applications/trophy.appimage_0_9_7.desktop")),
              "[Desktop Entry]\nExec=env DESKTOPINTEGRATION=1 /home/deck/Applications/trophy.appimage_0_9_7.appimage\n");

    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    ASSERT_TRUE(integration.integrate()) << integration.statusMessage().toStdString();

    const QString entry = readFile(integration.desktopFilePath());
    EXPECT_TRUE(entry.contains(QStringLiteral("OnlyShowIn=TROPHY-INTERNAL;")));
    EXPECT_TRUE(integration.entryHidden());
    EXPECT_TRUE(integration.isRegistered());   // identity still resolves
    EXPECT_TRUE(integration.isIntegrated());
    EXPECT_TRUE(integration.statusMessage().contains(QStringLiteral("hidden")))
        << integration.statusMessage().toStdString();

    // The manager's entry is removed: ours must come back to the menu rather
    // than leaving the user with nothing to click.
    QFile::remove(QDir(data.path()).filePath(QStringLiteral("applications/trophy.appimage_0_9_7.desktop")));
    integration.refreshVisibility();
    EXPECT_FALSE(integration.entryHidden());
    EXPECT_FALSE(readFile(integration.desktopFilePath()).contains(QStringLiteral("OnlyShowIn=TROPHY-INTERNAL;")));
}

// The refresh only settles visibility: launching from another manager's copy
// must not silently repoint the registered Exec at that file, and the entry is
// repointed only when the registered target has disappeared.
TEST(DesktopIntegrationTest, RefreshKeepsTheRegisteredTargetStable) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir data;
    QTemporaryDir images;
    ASSERT_TRUE(data.isValid() && images.isValid());
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qunsetenv("APPDIR");

    const QString registered = touchFile(images.filePath(QStringLiteral("Trophy-0.9.7-x86_64.AppImage")),
                                         "registered");
    qputenv("APPIMAGE", registered.toLocal8Bit());
    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    ASSERT_TRUE(integration.integrate());
    ASSERT_TRUE(readFile(integration.desktopFilePath()).contains(registered));

    // Another manager's copy is running, and it has its own entry.
    const QString managerCopy =
        touchFile(images.filePath(QStringLiteral("trophy.appimage_0_9_8.appimage")), "manager");
    touchFile(QDir(data.path()).filePath(QStringLiteral("applications/trophy.appimage_0_9_7.desktop")),
              (QStringLiteral("[Desktop Entry]\nExec=env DESKTOPINTEGRATION=1 ") + managerCopy +
               QStringLiteral("\n")).toUtf8());
    qputenv("APPIMAGE", managerCopy.toLocal8Bit());
    integration.refreshVisibility();

    QString entry = readFile(integration.desktopFilePath());
    EXPECT_TRUE(entry.contains(QStringLiteral("OnlyShowIn=TROPHY-INTERNAL;")));
    // Exec still names the registered copy, not the one that launched us.
    EXPECT_TRUE(entry.contains(registered));
    EXPECT_FALSE(entry.contains(managerCopy));

    // The registered copy goes away: now the entry follows the running file.
    QFile::remove(registered);
    integration.refreshVisibility();
    entry = readFile(integration.desktopFilePath());
    EXPECT_TRUE(entry.contains(managerCopy));
    EXPECT_TRUE(entry.contains(QStringLiteral("OnlyShowIn=TROPHY-INTERNAL;")));
}

// A distribution package's entry names /usr/bin/trophy; that is a different
// installation and must not hide our AppImage entry.
TEST(DesktopIntegrationTest, DistroEntryIsNotACompetingLauncher) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir data;
    QTemporaryDir images;
    ASSERT_TRUE(data.isValid() && images.isValid());
    const QString image = touchFile(images.filePath(QStringLiteral("Trophy.AppImage")));
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qputenv("APPIMAGE", image.toLocal8Bit());
    qunsetenv("APPDIR");

    touchFile(QDir(data.path()).filePath(QStringLiteral("applications/trophy.desktop.dist-package")),
              "[Desktop Entry]\nExec=/usr/bin/trophy\n");
    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    ASSERT_TRUE(integration.integrate());
    EXPECT_FALSE(integration.entryHidden());
    EXPECT_FALSE(readFile(integration.desktopFilePath()).contains(QStringLiteral("OnlyShowIn=TROPHY-INTERNAL;")));
}

// A manager's entry is a launcher, not an identity: installing through one
// still gets our hidden entry, so the app id stays stable instead of following
// the manager's versioned filename.
TEST(DesktopIntegrationTest, ManagerInstallStillGetsAStableHiddenEntry) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR",
                  "GIO_LAUNCHED_DESKTOP_FILE", "XDG_CONFIG_HOME"});
    QTemporaryDir data;
    QTemporaryDir config;
    QTemporaryDir images;
    ASSERT_TRUE(data.isValid() && config.isValid() && images.isValid());
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    const QString managed =
        touchFile(images.filePath(QStringLiteral("trophy.appimage_0_9_7.appimage")), "managed");
    const QString entry = touchFile(
        QDir(data.path()).filePath(QStringLiteral("applications/trophy.appimage_0_9_7.desktop")),
        (QStringLiteral("[Desktop Entry]\nExec=env DESKTOPINTEGRATION=1 ") + managed +
         QStringLiteral("\n")).toUtf8());
    qputenv("APPIMAGE", managed.toLocal8Bit());
    qputenv("GIO_LAUNCHED_DESKTOP_FILE", entry.toLocal8Bit());
    qunsetenv("APPDIR");
    qunsetenv("TROPHY_FAKE_INSTALL_KIND");

    ASSERT_TRUE(TrophyBackend::DesktopIntegration::installEntryAtStartup(data.path(), QStringList()));
    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    // Our entry exists, hidden from menus, and wins the id lookup.
    EXPECT_TRUE(QFileInfo::exists(integration.desktopFilePath()));
    EXPECT_TRUE(readFile(integration.desktopFilePath())
                    .contains(QStringLiteral("OnlyShowIn=TROPHY-INTERNAL;")));
    EXPECT_EQ(TrophyBackend::DesktopIntegration::effectiveDesktopId(data.path(), QStringList()),
              QStringLiteral("trophy"));
    EXPECT_TRUE(integration.isRegistered());
    EXPECT_TRUE(integration.entryHidden());
    // The startup install is reported once, so the caches can be refreshed.
    EXPECT_TRUE(TrophyBackend::DesktopIntegration::consumeStartupInstall());
    EXPECT_FALSE(TrophyBackend::DesktopIntegration::consumeStartupInstall());
}

// Our own stable entry wins over a manager's versioned one.
TEST(DesktopIntegrationTest, OurEntryBeatsAManagersVersionedId) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR",
                  "GIO_LAUNCHED_DESKTOP_FILE"});
    QTemporaryDir data;
    QTemporaryDir images;
    ASSERT_TRUE(data.isValid() && images.isValid());
    const QString managed =
        touchFile(images.filePath(QStringLiteral("trophy.appimage_0_9_7.appimage")), "managed");
    const QString managerEntry = touchFile(
        QDir(data.path()).filePath(QStringLiteral("applications/trophy.appimage_0_9_7.desktop")),
        (QStringLiteral("[Desktop Entry]\nExec=env DESKTOPINTEGRATION=1 ") + managed +
         QStringLiteral("\n")).toUtf8());
    touchFile(QDir(data.path()).filePath(QStringLiteral("applications/trophy.desktop")),
              "[Desktop Entry]\nExec=/opt/Trophy.AppImage\n");
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qputenv("APPIMAGE", managed.toLocal8Bit());
    qputenv("GIO_LAUNCHED_DESKTOP_FILE", managerEntry.toLocal8Bit());
    qunsetenv("APPDIR");

    EXPECT_EQ(TrophyBackend::DesktopIntegration::effectiveDesktopId(data.path(), QStringList()),
              QStringLiteral("trophy"));
}

// The entry is installed before QGuiApplication exists, so the first run
// already has the identity and the wizard needs no registration step.
TEST(DesktopIntegrationTest, StartupInstallWritesTheEntry) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR", "XDG_CONFIG_HOME"});
    QTemporaryDir data;
    QTemporaryDir config;
    QTemporaryDir images;
    ASSERT_TRUE(data.isValid() && config.isValid() && images.isValid());
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    const QString image = touchFile(images.filePath(QStringLiteral("Trophy-0.9.7-x86_64.AppImage")));
    ASSERT_FALSE(image.isEmpty());
    qputenv("APPIMAGE", image.toLocal8Bit());
    qunsetenv("APPDIR");
    qunsetenv("TROPHY_FAKE_INSTALL_KIND");

    ASSERT_TRUE(TrophyBackend::DesktopIntegration::installEntryAtStartup(data.path(), QStringList()));
    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    EXPECT_TRUE(QFileInfo::exists(integration.desktopFilePath()));
    EXPECT_TRUE(readFile(integration.desktopFilePath()).contains(image));
    // Idempotent: a second launch leaves it alone.
    EXPECT_FALSE(TrophyBackend::DesktopIntegration::installEntryAtStartup(data.path(), QStringList()));
}

// Removing the entry is also an opt-out, otherwise the next launch would
// silently recreate what the user just deleted.
TEST(DesktopIntegrationTest, RemoveDeletesTheEntryAndStopsReinstall) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR", "XDG_CONFIG_HOME"});
    QTemporaryDir data;
    QTemporaryDir config;
    QTemporaryDir images;
    ASSERT_TRUE(data.isValid() && config.isValid() && images.isValid());
    qputenv("XDG_CONFIG_HOME", config.path().toLocal8Bit());
    const QString image = touchFile(images.filePath(QStringLiteral("Trophy.AppImage")));
    qputenv("APPIMAGE", image.toLocal8Bit());
    qunsetenv("APPDIR");
    qunsetenv("TROPHY_FAKE_INSTALL_KIND");

    ASSERT_TRUE(TrophyBackend::DesktopIntegration::installEntryAtStartup(data.path(), QStringList()));
    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    ASSERT_TRUE(integration.removeEntry());
    EXPECT_FALSE(QFileInfo::exists(integration.desktopFilePath()));
    EXPECT_FALSE(TrophyBackend::DesktopIntegration::installEntryAtStartup(data.path(), QStringList()));

    // Integrating again clears the opt-out.
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    EXPECT_TRUE(integration.integrate());
    EXPECT_TRUE(QFileInfo::exists(integration.desktopFilePath()));
    qunsetenv("TROPHY_FAKE_INSTALL_KIND");
    EXPECT_FALSE(TrophyBackend::DesktopIntegration::installEntryAtStartup(data.path(), QStringList()));
}

TEST(DesktopIntegrationTest, NonAppImageInstallDoesNothing) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir data;
    ASSERT_TRUE(data.isValid());
    qputenv("TROPHY_FAKE_INSTALL_KIND", "portable");
    qunsetenv("APPIMAGE");
    qunsetenv("APPDIR");

    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    ASSERT_FALSE(integration.isAppImage());
    EXPECT_FALSE(integration.integrate());
    EXPECT_TRUE(integration.statusIsError());
    EXPECT_FALSE(integration.statusMessage().isEmpty());
    EXPECT_FALSE(QFileInfo::exists(integration.desktopFilePath()));
}

// A transient mount path must never become the Exec target: it disappears when
// the process exits.
TEST(DesktopIntegrationTest, MountPathIsRejected) {
    EnvGuard env({"TROPHY_FAKE_INSTALL_KIND", "APPIMAGE", "APPDIR"});
    QTemporaryDir mount;
    QTemporaryDir data;
    ASSERT_TRUE(mount.isValid() && data.isValid());
    qputenv("TROPHY_FAKE_INSTALL_KIND", "appimage");
    qputenv("APPDIR", mount.path().toLocal8Bit());
    const QString mountedBinary =
        touchFile(mount.filePath(QStringLiteral("usr/bin/trophy")), "elf");
    ASSERT_FALSE(mountedBinary.isEmpty());
    qputenv("APPIMAGE", mountedBinary.toLocal8Bit());

    TrophyBackend::DesktopIntegration integration(data.path(), QStringList());
    EXPECT_FALSE(integration.integrate());
    EXPECT_TRUE(integration.statusIsError());
    EXPECT_FALSE(QFileInfo::exists(integration.desktopFilePath()));
}
