/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

namespace TrophyBackend {

/**
 * @brief Everything the detection decision is allowed to know.
 *
 * Built once per event from an identity plus at most one window snapshot.
 * Keeping the decision a pure function of this struct means the reporter
 * path, the window reconciler and the pending-candidate recheck cannot
 * drift apart (they used to encode the same rules separately).
 */
struct DetectionEvidence {
  bool excluded = false;   // blacklist/veto applies to key or display name
  bool whitelisted = false; // explicit user rule matches
  bool steam = false;       // SteamAppId (wire/env) or a steamapps path
  bool reporter = false;    // a graphics reporter reported this PID
  bool focused = false;     // window state for the PID (when known)
  bool fullscreen = false;  // compositor fullscreen flag
  bool coversOutput = false; // borderless window covering >= ~90% output
  bool minimized = false;
};

enum class DetectionDecision {
  Ignore,   // not a game, or not enough evidence to even ask the user
  Candidate, // reporter evidence only: wait for focus/fullscreen
  Detect,   // confident enough to report/auto-capture
};

/**
 * @brief One evidence threshold for every detection source.
 *
 * Rules, in order:
 *  - veto always wins;
 *  - explicit whitelist or Steam identity detects immediately;
 *  - a reporter event detects once its window is focused and fullscreen
 *    (or borderless-equivalent), otherwise it waits as a candidate;
 *  - a window alone is never enough: non-reporter windows need whitelist
 *    or Steam evidence, which is what keeps fullscreen video players and
 *    browsers out of the detection path.
 */
class DetectionPolicy {
public:
  static DetectionDecision decide(const DetectionEvidence &evidence);
};

} // namespace TrophyBackend
