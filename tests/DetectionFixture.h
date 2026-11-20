/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "detection/GameDetectionService.h"
#include "detection/GameWhitelist.h"
namespace TrophyBackend {
// Recording tests inject domain snapshots, never compositor evidence.
class DetectionFixture {
public:
  static void publish(GameDetectionService &service, qint64 pid,
                      const QString &key) {
    auto p = ProcessCensus::inspect(pid);
    if (!p.pid) {
      p.pid = pid;
      p.start = 1;
    }
    const auto name = service.m_whitelist->getWhitelistName(key);
    service.m_selectedId = key + "@" + p.identity();
    service.m_instances = {{service.m_selectedId, key, name, pid, {{pid, p}}}};
    ProcessGames::publish(service.m_instances);
    service.updateCurrentGame(name, pid, key);
  }
};
} // namespace TrophyBackend
