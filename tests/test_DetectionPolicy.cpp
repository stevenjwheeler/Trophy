/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "detection/DetectionPolicy.h"

#include <gtest/gtest.h>

using TrophyBackend::DetectionDecision;
using TrophyBackend::DetectionEvidence;
using TrophyBackend::DetectionPolicy;

namespace {

DetectionEvidence reporterEvidence(bool focused, bool fullscreen,
                                   bool coversOutput = false,
                                   bool minimized = false) {
  DetectionEvidence evidence;
  evidence.reporter = true;
  evidence.focused = focused;
  evidence.fullscreen = fullscreen;
  evidence.coversOutput = coversOutput;
  evidence.minimized = minimized;
  return evidence;
}

} // namespace

TEST(DetectionPolicyTest, VetoWinsEvenWhenWhitelisted) {
  DetectionEvidence evidence = reporterEvidence(true, true);
  evidence.whitelisted = true;
  evidence.excluded = true;
  EXPECT_EQ(DetectionPolicy::decide(evidence), DetectionDecision::Ignore);
}

TEST(DetectionPolicyTest, WhitelistDetectsWithoutWindowOrReporter) {
  DetectionEvidence evidence;
  evidence.whitelisted = true;
  EXPECT_EQ(DetectionPolicy::decide(evidence), DetectionDecision::Detect);
}

TEST(DetectionPolicyTest, SteamDetectsWithoutWindowOrReporter) {
  DetectionEvidence evidence;
  evidence.steam = true;
  EXPECT_EQ(DetectionPolicy::decide(evidence), DetectionDecision::Detect);
}

TEST(DetectionPolicyTest, ReporterFocusedFullscreenDetects) {
  EXPECT_EQ(DetectionPolicy::decide(reporterEvidence(true, true)),
            DetectionDecision::Detect);
}

TEST(DetectionPolicyTest, ReporterBorderlessCoverageDetects) {
  EXPECT_EQ(DetectionPolicy::decide(reporterEvidence(true, false, true)),
            DetectionDecision::Detect);
}

TEST(DetectionPolicyTest, ReporterWindowedIsCandidate) {
  EXPECT_EQ(DetectionPolicy::decide(reporterEvidence(true, false)),
            DetectionDecision::Candidate);
}

TEST(DetectionPolicyTest, ReporterUnfocusedIsCandidate) {
  EXPECT_EQ(DetectionPolicy::decide(reporterEvidence(false, false)),
            DetectionDecision::Candidate);
}

TEST(DetectionPolicyTest, ReporterMinimizedIsCandidate) {
  // Minimizing is not process exit: keep the candidate, but do not detect
  // until it is visible again.
  EXPECT_EQ(DetectionPolicy::decide(reporterEvidence(true, true, false, true)),
            DetectionDecision::Candidate);
}

TEST(DetectionPolicyTest, WindowWithoutReporterIsIgnored) {
  // The rule that keeps fullscreen video players, browsers and emulators
  // out of detection: window context alone is never game evidence.
  DetectionEvidence evidence;
  evidence.focused = true;
  evidence.fullscreen = true;
  EXPECT_EQ(DetectionPolicy::decide(evidence), DetectionDecision::Ignore);
}

TEST(DetectionPolicyTest, ReporterWithoutAnyWindowIsCandidate) {
  DetectionEvidence evidence;
  evidence.reporter = true;
  EXPECT_EQ(DetectionPolicy::decide(evidence), DetectionDecision::Candidate);
}
