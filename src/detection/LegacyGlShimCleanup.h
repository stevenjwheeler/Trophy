/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace TrophyBackend::LegacyGlShimCleanup {

/** Path of the environment.d file the retired GL shim used. */
QString confFilePath();

/**
 * @brief Strip retired GL-shim entries from our environment.d file.
 *
 * Earlier builds could enroll a session-wide `LD_PRELOAD` for the OpenGL
 * shim. The shim is gone now, so a stale entry would make every future login
 * try to load a missing library. Remove only entries Trophy clearly owns
 * (the shim soname, its durable copy, or the multilib token directory),
 * preserve unrelated variables/entries, and delete the file once nothing of
 * ours remains. Never touches a foreign file, and a missing file is a no-op.
 *
 * @return false only when a changed file could not be written/removed.
 */
bool removeOwnedEnrollment();

} // namespace TrophyBackend::LegacyGlShimCleanup
