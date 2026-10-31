/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include "core/AppRestart.h"

// The script only has to wait for the old process and then start the target,
// but a quoting mistake silently launches the wrong path (or nothing), so the
// generated text is pinned here.
TEST(AppRestartTest, ScriptWaitsForTheOldProcessAndLaunchesTheTarget) {
    const QString text =
        TrophyBackend::AppRestart::script(4321, QStringLiteral("/home/deck/Applications/Trophy.AppImage"));

    EXPECT_TRUE(text.startsWith(QStringLiteral("#!/bin/sh\n"))) << text.toStdString();
    EXPECT_TRUE(text.contains(QStringLiteral("PARENT=4321\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("TARGET='/home/deck/Applications/Trophy.AppImage'\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("while kill -0 \"$PARENT\" 2>/dev/null; do sleep 0.2; done\n")));
    // Launch from the quoted variable so spaces survive, then clean up.
    EXPECT_TRUE(text.contains(QStringLiteral("\"$TARGET\" &\n")));
    EXPECT_TRUE(text.contains(QStringLiteral("rm -f \"$0\"\n")));
    // No shell metacharacters may leak out of the quoting.
    EXPECT_FALSE(text.contains(QStringLiteral("TARGET=/home")));
}

TEST(AppRestartTest, PathsWithSpacesAndQuotesStayOneArgument) {
    const QString path = QStringLiteral("/tmp/My Trophy's Build/Trophy.AppImage");
    const QString text = TrophyBackend::AppRestart::script(7, path);
    // Single quotes are escaped the POSIX way: '\''
    EXPECT_TRUE(text.contains(QStringLiteral("TARGET='/tmp/My Trophy'\\''s Build/Trophy.AppImage'\n")))
        << text.toStdString();
}

TEST(AppRestartTest, MissingOrNonExecutableTargetIsRefused) {
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    EXPECT_FALSE(TrophyBackend::AppRestart::scheduleAfterExit(QString()));
    EXPECT_FALSE(TrophyBackend::AppRestart::scheduleAfterExit(dir.filePath(QStringLiteral("missing"))));

    const QString plain = dir.filePath(QStringLiteral("plain"));
    QFile f(plain);
    ASSERT_TRUE(f.open(QIODevice::WriteOnly));
    f.write("x");
    f.close();
    QFile::setPermissions(plain, QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    EXPECT_FALSE(TrophyBackend::AppRestart::scheduleAfterExit(plain));
}
