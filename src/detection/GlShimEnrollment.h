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
 * The shim (trophy_gl_shim.so) loads into dynamically linked session programs
 * via LD_PRELOAD, NOT just games. It is never enabled silently: the setup wizard
 * offers it explicitly and this class persists the choice in
 * $XDG_CONFIG_HOME/environment.d/trophy-gl-shim.conf (takes effect on
 * next login). The conf file is the state — no settings flag needed.
 *
 * TROPHY_GL_SHIM_LIB overrides the native lookup (tests, packaging), and
 * TROPHY_GL_SHIM_LIB32 can supply a matching i386 companion. Explicit enabling
 * with both reporters publishes one architecture-selecting global preload;
 * this is not a per-game setting. Legacy enrollment is never silently migrated.
 */
class GlShimEnrollment {
public:
  /** Shipped path to trophy_gl_shim.so, or empty. Read-only lookup. */
  static QString libraryPath();
  /** Optional same-installation i386 companion; lookup never enrolls it. */
  static QString libraryPath32();
  /** Enrolled helper version, or the available helper when not enrolled. */
  static int helperVersion();
  static int bundledHelperVersion();

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

  /** Write (true) or remove our entries (false). Merges, never
   * clobbers, a foreign LD_PRELOAD entry in our own conf file.
   * Bundle-owned helpers are persisted before atomically updating the conf;
   * explicit re-enrollment refreshes older durable versions. */
  static bool setEnrolled(bool enrolled);
  // Refresh an opted-in architecture pair, or an existing legacy bundle path.
  // A legacy entry stays legacy until explicit re-enrollment.
  static bool refreshEnrolledBundle();

private:
  static bool setEnrolledImpl(bool enrolled, bool allowMultilib, const QString &retained32 = {});
};

} // namespace TrophyBackend
