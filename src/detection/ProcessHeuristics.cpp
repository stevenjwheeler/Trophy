/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ProcessHeuristics.h"

#include <QFile>
#include <QDir>
#include <QRegularExpression>
#include <QDebug>

namespace GameRecorder {

ProcessHeuristics::ProcessHeuristics()
{
    // Patterns that indicate a game
    m_gamePatterns = {
        "steam_app_*",
        "proton",
        "wine",
        "wine64",
        "lutris",
        "retroarch",
        "dolphin-emu",
        "pcsx2",
        "rpcs3",
        "yuzu",
        "ryujinx",
        "cemu",
        "desmume"
    };

    // Launchers (games might be children of these)
    m_knownLaunchers = {
        "steam",
        "steamwebhelper",
        "lutris",
        "heroic",
        "bottles",
        "gamescope"
    };

    // Known non-games
    m_knownNonGames = {
        "firefox",
        "chromium",
        "chrome",
        "code",
        "discord",
        "slack",
        "thunderbird",
        "nautilus",
        "dolphin",
        "konsole",
        "gnome-terminal",
        "kitty",
        "alacritty",
        "spotify",
        "vlc",
        "mpv"
    };
}

ProcessHeuristics::~ProcessHeuristics() = default;

ProcessHeuristics::ProcessInfo ProcessHeuristics::getProcessInfo(qint64 pid) const
{
    ProcessInfo info;
    info.pid = pid;

    QString procPath = QString("/proc/%1").arg(pid);

    // Read process name from comm
    QFile commFile(procPath + "/comm");
    if (commFile.open(QIODevice::ReadOnly)) {
        info.name = QString::fromUtf8(commFile.readAll()).trimmed();
        commFile.close();
    }

    // Read command line
    QFile cmdlineFile(procPath + "/cmdline");
    if (cmdlineFile.open(QIODevice::ReadOnly)) {
        QByteArray cmdline = cmdlineFile.readAll();
        cmdline.replace('\0', ' ');
        info.cmdLine = QString::fromUtf8(cmdline).trimmed();
        cmdlineFile.close();
    }

    // Read current working directory
    QFileInfo cwdLink(procPath + "/cwd");
    if (cwdLink.exists()) {
        info.cwd = cwdLink.symLinkTarget();
    }

    // Read environment for Steam App ID
    QFile environFile(procPath + "/environ");
    if (environFile.open(QIODevice::ReadOnly)) {
        QByteArray environ = environFile.readAll();
        QList<QByteArray> vars = environ.split('\0');

        for (const QByteArray &var : vars) {
            if (var.startsWith("SteamAppId=")) {
                info.steamAppId = QString::fromUtf8(var.mid(11));
            } else if (var.startsWith("SteamGameId=")) {
                // Alternative Steam identifier
                if (info.steamAppId.isEmpty()) {
                    info.steamAppId = QString::fromUtf8(var.mid(12));
                }
            } else if (var.startsWith("STEAM_COMPAT_DATA_PATH=")) {
                info.isProton = true;
            }
        }
        environFile.close();
    }

    // Check if Wine/Proton
    if (info.name.contains("wine", Qt::CaseInsensitive) ||
        info.cmdLine.contains("wine", Qt::CaseInsensitive)) {
        info.isWine = true;
    }

    // Get Steam game name if we have App ID
    if (!info.steamAppId.isEmpty()) {
        auto gameName = getSteamGameName(info.steamAppId);
        if (gameName) {
            info.steamGameName = *gameName;
        }
    }

    return info;
}

float ProcessHeuristics::gameConfidence(const ProcessInfo &info) const
{
    float score = 0.0f;

    // Check if it's a known non-game
    if (isKnownNonGame(info.name)) {
        return 0.0f;
    }

    // Steam game (very reliable)
    if (!info.steamAppId.isEmpty()) {
        score += 0.9f;
    }

    // Wine/Proton (likely a game)
    if (info.isWine || info.isProton) {
        score += 0.3f;
    }

    // Pattern matching
    for (const QString &pattern : m_gamePatterns) {
        QRegularExpression re(QRegularExpression::wildcardToRegularExpression(pattern),
                             QRegularExpression::CaseInsensitiveOption);
        if (re.match(info.name).hasMatch() || re.match(info.windowClass).hasMatch()) {
            score += 0.5f;
            break;
        }
    }

    // Path-based detection
    if (info.cwd.contains("steamapps/common", Qt::CaseInsensitive) ||
        info.cwd.contains("/Games/", Qt::CaseInsensitive) ||
        info.cwd.contains(".wine", Qt::CaseInsensitive) ||
        info.cwd.contains("lutris", Qt::CaseInsensitive)) {
        score += 0.3f;
    }

    // Graphics library detection
    if (checkForGameLibraries(info.pid)) {
        score += 0.2f;
    }

    return std::min(score, 1.0f);
}

bool ProcessHeuristics::isProbablyGame(const ProcessInfo &info) const
{
    return gameConfidence(info) >= 0.5f;
}

std::optional<QString> ProcessHeuristics::getSteamAppId(qint64 pid) const
{
    ProcessInfo info = getProcessInfo(pid);
    if (!info.steamAppId.isEmpty()) {
        return info.steamAppId;
    }
    return std::nullopt;
}

std::optional<QString> ProcessHeuristics::getSteamGameName(const QString &appId) const
{
    // Check Steam library folders for game name
    QStringList steamPaths = {
        QDir::homePath() + "/.steam/steam",
        QDir::homePath() + "/.local/share/Steam"
    };

    for (const QString &steamPath : steamPaths) {
        // Try to find appmanifest file
        QString manifestPath = steamPath + "/steamapps/appmanifest_" + appId + ".acf";
        QFile manifest(manifestPath);

        if (manifest.open(QIODevice::ReadOnly)) {
            QString content = QString::fromUtf8(manifest.readAll());
            manifest.close();

            // Parse "name" field from ACF format
            QRegularExpression nameRe("\"name\"\\s+\"([^\"]+)\"");
            QRegularExpressionMatch match = nameRe.match(content);
            if (match.hasMatch()) {
                return match.captured(1);
            }
        }
    }

    return std::nullopt;
}

bool ProcessHeuristics::checkForGameLibraries(qint64 pid) const
{
    QString mapsPath = QString("/proc/%1/maps").arg(pid);
    QFile mapsFile(mapsPath);

    if (!mapsFile.open(QIODevice::ReadOnly)) {
        return false;
    }

    QByteArray maps = mapsFile.readAll();
    mapsFile.close();

    // Check for common game/graphics libraries
    QList<QByteArray> gameLibs = {
        "libvulkan",
        "libGL.so",
        "libGLX",
        "libEGL",
        "dxvk",
        "vkd3d",
        "libSDL",
        "libopenal",
        "libsteam_api"
    };

    for (const QByteArray &lib : gameLibs) {
        if (maps.contains(lib)) {
            return true;
        }
    }

    return false;
}

bool ProcessHeuristics::checkGpuUsage(qint64 pid) const
{
    // TODO: Check nvidia-smi or similar for GPU usage
    Q_UNUSED(pid)
    return false;
}

bool ProcessHeuristics::isKnownLauncher(const QString &name) const
{
    return m_knownLaunchers.contains(name, Qt::CaseInsensitive);
}

bool ProcessHeuristics::isKnownNonGame(const QString &name) const
{
    return m_knownNonGames.contains(name, Qt::CaseInsensitive);
}

} // namespace GameRecorder
