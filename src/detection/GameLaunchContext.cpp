/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "GameLaunchContext.h"
#include <QSet>

namespace TrophyBackend {
namespace {
QString base(QString path) {
  path.replace('\\', '/');
  return path.section('/', -1);
}
QString processName(const QString &payload) {
  auto name = base(payload).toLower();
  if (name.endsWith(".exe"))
    name.chop(4);
  return name;
}
ProcessInfo privateRuntime(const ProcessInfo &seed,
                           const ProcessCensus::Snapshot &processes) {
  const auto context = processes.value(seed.parent);
  if (base(context.executable) != "pv-adverb" || context.start > seed.start)
    return {};
  const auto &args = context.arguments;
  const int separator = args.indexOf("--");
  if (separator < 1)
    return {};
  int command = separator + 1;
  if (args.value(command).startsWith('/') &&
      base(args.value(command)) == "umu-shim")
    ++command;
  const auto interpreter = base(args.value(command));
  if (interpreter == "python3" || interpreter.startsWith("python3."))
    ++command;
  if (!args.value(command).startsWith('/') ||
      base(args.value(command)) != "proton" ||
      args.value(command + 1) != "waitforexitandrun" ||
      args.value(command + 2) != seed.payload())
    return {};
  auto child = context;
  QSet<qint64> visited{context.pid};
  for (int depth = 0; depth < 8; ++depth) {
    const auto parent = processes.value(child.parent);
    if (!parent.pid || visited.contains(parent.pid) || parent.start > child.start)
      return {};
    visited.insert(parent.pid);
    if (base(parent.payload()) == "umu-run") {
      const int target = base(parent.executable) == "umu-run" ? 1 : 2;
      return parent.arguments.value(target) == seed.payload() ? context : ProcessInfo{};
    }
    if (!QStringList{"srt-bwrap", "bwrap"}.contains(base(parent.executable)))
      return {};
    child = parent;
  }
  return {};
}
} // namespace

GameLaunchContext GameLaunchContext::attest(
    const ProcessInfo &seed, const QString &installation,
    const ProcessCensus::Snapshot &processes) {
  if (installation.isEmpty() || !seed.payload().startsWith(installation + "/"))
    return {};
  return {installation, seed.winePrefix, privateRuntime(seed, processes)};
}

bool GameLaunchContext::contains(const ProcessInfo &process) const {
  return hasScope() && process.payload().startsWith(directory + "/") &&
         process.winePrefix == prefix;
}

bool GameLaunchContext::matchesRuntime(const ProcessInfo &process) const {
  return runtime.pid && process.pid == runtime.pid &&
         process.start == runtime.start && process.executable == runtime.executable &&
         process.payload() == runtime.payload() && process.arguments == runtime.arguments &&
         process.winePrefix == runtime.winePrefix;
}

bool GameLaunchContext::ownsSibling(
    const ProcessInfo &process, const ProcessCensus::Snapshot &processes) const {
  if (!contains(process) || !runtime.pid)
    return false;
  if (!matchesRuntime(processes.value(runtime.pid)))
    return false;
  auto child = process;
  QSet<qint64> visited{process.pid};
  for (int depth = 0; depth < 64; ++depth) {
    const auto parent = processes.value(child.parent);
    if (!parent.pid || visited.contains(parent.pid) || parent.start > child.start)
      return false;
    if (parent.pid == runtime.pid)
      return true;
    if (QStringList{"pv-adverb", "wineserver", "services", "explorer"}
            .contains(processName(parent.payload())))
      return false;
    visited.insert(parent.pid);
    child = parent;
  }
  return false;
}
} // namespace TrophyBackend
