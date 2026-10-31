/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "DetectionPolicy.h"

namespace TrophyBackend {

DetectionDecision DetectionPolicy::decide(const DetectionEvidence &evidence) {
  // A veto is authoritative even when a whitelist rule also matches; the
  // service checks blacklists before building evidence, this is the same
  // ordering expressed once.
  if (evidence.excluded)
    return DetectionDecision::Ignore;

  // Explicit user rule, Proton/Steam marker, or the executable living in
  // a Steam library: detect without waiting for a window.
  if (evidence.whitelisted || evidence.steam)
    return DetectionDecision::Detect;

  // Without a reporter, a window is only context, never independent game
  // discovery: a focused fullscreen player is not a game.
  if (!evidence.reporter)
    return DetectionDecision::Ignore;

  // The reporter proves a process initialized a graphics stack; the
  // window still has to look like gameplay (or the user has to whitelist).
  if (!evidence.minimized && evidence.focused &&
      (evidence.fullscreen || evidence.coversOutput))
    return DetectionDecision::Detect;

  return DetectionDecision::Candidate;
}

} // namespace TrophyBackend
