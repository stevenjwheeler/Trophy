/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2024
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>
#include <QStringList>
#include <optional>

namespace GameRecorder {

/**
 * @brief Heuristics for identifying game processes
 *
 * Uses various indicators to determine if a process is likely a game:
 * - Steam integration (SteamAppId environment variable)
 * - Window class patterns (steam_app_*, wine, proton)
 * - Loaded graphics libraries (Vulkan, OpenGL)
 * - Executable path location
 */
class ProcessHeuristics
{
public:
    struct ProcessInfo {
        qint64 pid = 0;
        QString name;
        QString cmdLine;
        QString cwd;
        QString windowClass;
        bool isWine = false;
        bool isProton = false;
        QString steamAppId;
        QString steamGameName;
    };

    ProcessHeuristics();
    ~ProcessHeuristics();

    /**
     * @brief Get information about a process
     * @param pid Process ID
     * @return ProcessInfo structure
     */
    ProcessInfo getProcessInfo(qint64 pid) const;

    /**
     * @brief Calculate game confidence score
     * @param info Process information
     * @return Score from 0.0 (not a game) to 1.0 (definitely a game)
     */
    float gameConfidence(const ProcessInfo &info) const;

    /**
     * @brief Check if process is probably a game
     * @param info Process information
     * @return true if likely a game
     */
    bool isProbablyGame(const ProcessInfo &info) const;

    /**
     * @brief Get Steam App ID from process
     * @param pid Process ID
     * @return Steam App ID or empty optional
     */
    std::optional<QString> getSteamAppId(qint64 pid) const;

    /**
     * @brief Get Steam game name from App ID
     * @param appId Steam App ID
     * @return Game name or empty optional
     */
    std::optional<QString> getSteamGameName(const QString &appId) const;

private:
    bool checkForGameLibraries(qint64 pid) const;
    bool checkGpuUsage(qint64 pid) const;
    bool isKnownLauncher(const QString &name) const;
    bool isKnownNonGame(const QString &name) const;

    QStringList m_gamePatterns;
    QStringList m_knownLaunchers;
    QStringList m_knownNonGames;
};

} // namespace GameRecorder
