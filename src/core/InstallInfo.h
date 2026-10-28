/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace TrophyBackend {

enum class InstallKind {
    AppImage = 0,
    DebianPackage = 1,
    RpmPackage = 2,
    PortablePrefix = 3,
    Unknown = 4
};

class InstallInfo {
public:
    // Detect how the running binary was installed. Safe to call at startup.
    static InstallKind detect();

    static QString name(InstallKind kind);

    // Suggested local-install command for a downloaded update asset.
    // Empty for kinds that cannot be installed from a downloaded file.
    static QString manualCommand(InstallKind kind, const QString &assetFileName);
};

} // namespace TrophyBackend