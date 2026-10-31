/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AppRestart.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace TrophyBackend {
namespace AppRestart {

namespace {

// Single-quote for /bin/sh: the only character with meaning inside single
// quotes is the quote itself.
QString shellQuote(const QString &value) {
    QString out(QLatin1Char('\''));
    for (const QChar c : value) {
        if (c == QLatin1Char('\''))
            out += QStringLiteral("'\\''");
        else
            out += QString(1, c);
    }
    out += QLatin1Char('\'');
    return out;
}

} // namespace

QString script(qint64 parentPid, const QString &target) {
    QString out;
    out += QStringLiteral("#!/bin/sh\n");
    out += QStringLiteral("# Relaunch Trophy once the old process has exited.\n");
    out += QStringLiteral("PARENT=") + QString::number(parentPid) + QStringLiteral("\n");
    out += QStringLiteral("TARGET=") + shellQuote(target) + QStringLiteral("\n");
    out += QStringLiteral("while kill -0 \"$PARENT\" 2>/dev/null; do sleep 0.2; done\n");
    out += QStringLiteral("\"$TARGET\" &\n");
    out += QStringLiteral("rm -f \"$0\"\n");
    return out;
}

bool scheduleAfterExit(const QString &target) {
    const QFileInfo info(target);
    if (target.isEmpty() || !info.isFile() || !info.isExecutable())
        return false;

    QString dir = QStandardPaths::writableLocation(QStandardPaths::TempLocation);
    if (dir.isEmpty())
        dir = QDir::tempPath();
    const QString path =
        QDir(dir).filePath(QStringLiteral("trophy-restart-%1.sh")
                               .arg(QCoreApplication::applicationPid()));

    QFile helper(path);
    if (!helper.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    const QByteArray contents = script(QCoreApplication::applicationPid(), target).toUtf8();
    const bool written = helper.write(contents) == contents.size();
    helper.close();
    if (!written) {
        QFile::remove(path);
        return false;
    }
    QFile::setPermissions(path, QFileDevice::ReadOwner | QFileDevice::WriteOwner |
                                    QFileDevice::ExeOwner | QFileDevice::ReadGroup |
                                    QFileDevice::ExeGroup | QFileDevice::ReadOther |
                                    QFileDevice::ExeOther);

    return QProcess::startDetached(path, QStringList());
}

} // namespace AppRestart
} // namespace TrophyBackend
