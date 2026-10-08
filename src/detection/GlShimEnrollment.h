/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QString>

namespace TrophyBackend {

/**
 * @brief Opt-in enrollment for the OpenGL detection shim.
 *
 * The shim (trophy_gl_shim.so) only loads into games via LD_PRELOAD, which
 * is session-wide and therefore never enabled silently: the setup wizard
 * offers it explicitly and this class persists the choice in
 * $XDG_CONFIG_HOME/environment.d/trophy-gl-shim.conf (takes effect on
 * next login). The conf file is the state — no settings flag needed.
 *
 * TROPHY_GL_SHIM_LIB overrides the library lookup (tests, packaging).
 */
class GlShimEnrollment {
public:
  /** Full path to trophy_gl_shim.so, or empty when not installed. */
  static QString libraryPath();

  /**
   * Lookup relative to a given application directory (the dev build tree
   * keeps the lib in <appDir>/gl_shim/; installs resolve it under the
   * prefix derived from <appDir>, incl. multilib layouts). Separated
   * from libraryPath() so tests can exercise the layout without
   * touching the real install.
   * NOTE: takes a directory — findShippedLib appends the file name, so a
   * full file path here silently matches nothing.
   */
  static QString libraryPathForAppDir(const QString &appDir);

  /** Full path of our environment.d conf file. */
  static QString confFilePath();

  /** True when the conf file enrolls an existing library. */
  static bool isEnrolled();

  /** True when this very process runs with the shim preloaded. */
  static bool isActiveInCurrentSession();

  /** Write (true) or remove (false) the enrollment. Merges, never
   * clobbers, a foreign LD_PRELOAD entry in our own conf file. */
  static bool setEnrolled(bool enrolled);
};

} // namespace TrophyBackend
