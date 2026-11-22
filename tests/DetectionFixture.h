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
  static void setLaunchRuntime(GameDetectionService &service, qint64 pid) {
    auto &instance = service.m_instances.first();
    instance.launchContext.runtime = ProcessCensus::inspect(pid);
  }
  static void promotePayload(GameDetectionService &service, qint64 pid) {
    auto &instance = service.m_instances.first();
    instance.primaryPid = pid;
    instance.processes = {{pid, ProcessCensus::inspect(pid)}};
    ProcessGames::publish(service.m_instances);
    service.updateCurrentGame(instance.name, pid, instance.key);
  }
  static void replaceProcessRecord(GameDetectionService &service, const ProcessInfo &process) {
    service.m_instances.first().processes = {{process.pid, process}};
    ProcessGames::publish(service.m_instances);
  }
  static void addIndependentInstance(GameDetectionService &service, qint64 pid,
                                     const QString &key, bool select = false) {
    const auto process = ProcessCensus::inspect(pid);
    const auto name = service.m_whitelist->getWhitelistName(key);
    const auto id = key + "@" + process.identity();
    service.m_instances.append({id, key, name, pid, {{pid, process}}});
    ProcessGames::publish(service.m_instances);
    if (select) {
      service.m_selectedId = id;
      service.updateCurrentGame(name, pid, key);
    }
  }
  static void selectInstance(GameDetectionService &service, const QString &id) {
    for (const auto &instance : service.m_instances)
      if (instance.id == id) {
        service.m_selectedId = id;
        service.updateCurrentGame(instance.name, instance.primaryPid, instance.key);
        return;
      }
  }
  static void clear(GameDetectionService &service) {
    service.m_instances.clear();
    service.m_selectedId.clear();
    ProcessGames::publish(service.m_instances);
    service.updateCurrentGame({}, 0);
  }
};
} // namespace TrophyBackend
