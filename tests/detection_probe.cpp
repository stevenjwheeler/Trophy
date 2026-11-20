/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "audio/PipeWireAudioSource.h"
#include "detection/GameDefinitions.h"
#include "detection/ProcessGames.h"
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QThread>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <sys/resource.h>

namespace {
double cpuMs() {
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  return (usage.ru_utime.tv_sec + usage.ru_stime.tv_sec) * 1000.0 +
         (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1000.0;
}
} // namespace

int main(int argc, char **argv) {
  QCoreApplication app(argc, argv);
  if (app.arguments().contains("--benchmark")) {
    // Local, warm-cache measurement, not a universal performance guarantee.
    constexpr int iterations = 100;
    QJsonObject output;
    TrophyBackend::ProcessCensus::Snapshot snapshot;
    QList<TrophyBackend::SteamGame> steam;
    const auto measure = [&](const QString &name, auto operation) {
      QElapsedTimer timer;
      timer.start();
      const auto cpu = cpuMs();
      for (int i = 0; i < iterations; ++i)
        operation();
      output[name] =
          QJsonObject{{"meanWallMs", timer.nsecsElapsed() / 1e6 / iterations},
                      {"meanCpuMs", (cpuMs() - cpu) / iterations}};
    };
    measure("census", [&] { snapshot = TrophyBackend::ProcessCensus::scan(); });
    measure("steamMetadata",
            [&] { steam = TrophyBackend::ProcessGames::steamLibrary(); });
    measure("identify", [&] {
      TrophyBackend::ProcessGames::identify(
          snapshot, steam, TrophyBackend::trophyGameDefinitions(), {});
    });
    output["processes"] = snapshot.size();
    output["steamGames"] = steam.size();
    output["iterations"] = iterations;
    const auto json = QJsonDocument(output).toJson();
    std::fwrite(json.constData(), 1, json.size(), stdout);
    return 0;
  }
  const auto games = TrophyBackend::ProcessGames::identify(
      TrophyBackend::ProcessCensus::scan(),
      TrophyBackend::ProcessGames::steamLibrary(), {}, {});
  TrophyBackend::ProcessGames::publish(games);
  QJsonArray output;
  for (const auto &g : games) {
    QJsonArray processes;
    for (const auto &p : g.processes) {
      processes.append(
          QJsonObject{{"pid", p.pid},
                      {"start", qint64(p.start)},
                      {"parent", p.parent},
                      {"exe", p.executable},
                      {"payload", p.payload()},
                      {"arguments", QJsonArray::fromStringList(p.arguments)}});
    }
    QString name, serial;
    const auto node = TrophyBackend::PipeWireAudioSource::findNodeForGame(
        g.primaryPid, {"trophy-instance:" + g.id}, &name, &serial);
    QJsonObject entry{{"id", g.id},
                      {"key", g.key},
                      {"name", g.name},
                      {"processes", processes},
                      {"audioNode", qint64(node)},
                      {"audioName", name},
                      {"audioSerial", serial}};
    if (app.arguments().contains("--sample") && node != 0xffffffffu) {
      std::atomic<quint64> count{0};
      std::atomic<double> energy{0};
      auto source = TrophyBackend::PipeWireAudioSource::createApplicationSource(
          node, name, serial);
      source->setCallback([&](const TrophyBackend::AudioFrame &frame) {
        double sum = 0;
        const int n = frame.frameCount * frame.channels;
        for (int i = 0; i < n; ++i)
          sum += frame.samples[i] * frame.samples[i];
        energy.fetch_add(sum);
        count.fetch_add(n);
      });
      const bool started = source->initialize({}) && source->start();
      if (started)
        QThread::msleep(1000);
      source->stop();
      entry["sampleStarted"] = started;
      entry["sampleCount"] = qint64(count.load());
      entry["sampleRms"] =
          count.load() ? std::sqrt(energy.load() / count.load()) : 0;
    }
    output.append(entry);
  }
  const QJsonObject result{
      {"games", output},
      {"audio", QJsonArray::fromVariantList(
                    TrophyBackend::PipeWireAudioSource::enumerateAudioApps())}};
  const auto json = QJsonDocument(result).toJson();
  std::fwrite(json.constData(), 1, json.size(), stdout);
}
