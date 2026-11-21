/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ProcessCensus.h"

namespace TrophyBackend {
// Installation-scoped runtime evidence for one observed launch. Runtime
// helpers attest ancestry but are never themselves game/audio members.
struct GameLaunchContext {
  QString directory, prefix;
  ProcessInfo runtime;

  static GameLaunchContext attest(const ProcessInfo &seed,
                                  const QString &installation,
                                  const ProcessCensus::Snapshot &processes);
  bool hasScope() const { return !directory.isEmpty(); }
  bool contains(const ProcessInfo &process) const;
  bool matchesRuntime(const ProcessInfo &process) const;
  bool ownsSibling(const ProcessInfo &process,
                    const ProcessCensus::Snapshot &processes) const;
};
} // namespace TrophyBackend
