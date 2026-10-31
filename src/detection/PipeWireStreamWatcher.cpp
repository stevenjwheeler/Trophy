/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PipeWireStreamWatcher.h"

#include <QDebug>
#include <QFuture>
#include <QVariantMap>
#include <QtConcurrent/QtConcurrentRun>

namespace TrophyBackend {

PipeWireStreamWatcher::PipeWireStreamWatcher(QObject *parent)
    : QObject(parent) {
  m_timer.setSingleShot(false);
  connect(&m_timer, &QTimer::timeout, this, &PipeWireStreamWatcher::poll);
  connect(&m_watcher, &QFutureWatcher<QVariantList>::finished, this, [this] {
    m_inflight = false;
    applySnapshot(m_watcher.result());
  });
}

PipeWireStreamWatcher::~PipeWireStreamWatcher() { stop(); }

void PipeWireStreamWatcher::setSnapshotFunction(SnapshotFn fn) {
  m_snapshot = std::move(fn);
}

void PipeWireStreamWatcher::start(int intervalMs) {
  if (!m_snapshot) {
    qWarning() << "PipeWireStreamWatcher: no snapshot function configured";
    return;
  }
  if (intervalMs <= 0)
    intervalMs = 1000;
  m_timer.setInterval(intervalMs);
  if (!m_timer.isActive())
    m_timer.start();
  poll();
}

void PipeWireStreamWatcher::stop() {
  m_timer.stop();
  if (m_watcher.isRunning()) {
    // The snapshot is a bounded PipeWire round-trip; wait so the worker
    // cannot outlive this object while it holds the injected function.
    m_watcher.cancel();
    m_watcher.waitForFinished();
  }
  m_inflight = false;
  m_known.clear();
}

bool PipeWireStreamWatcher::isRunning() const { return m_timer.isActive(); }

QList<AudioStreamInfo> PipeWireStreamWatcher::streams() const {
  return m_known.values();
}

void PipeWireStreamWatcher::pollNow() { poll(); }

void PipeWireStreamWatcher::poll() {
  if (m_inflight || !m_snapshot)
    return;
  m_inflight = true;
  SnapshotFn snapshot = m_snapshot;
  // Exceptions must not escape either the worker or result().
  m_watcher.setFuture(QtConcurrent::run([snapshot]() -> QVariantList {
    try {
      return snapshot();
    } catch (...) {
      return {};
    }
  }));
}

void PipeWireStreamWatcher::applySnapshot(const QVariantList &apps) {
  QMap<quint32, AudioStreamInfo> next;
  for (const QVariant &value : apps) {
    const QVariantMap map = value.toMap();
    AudioStreamInfo info;
    info.nodeId = map.value(QStringLiteral("nodeId")).toUInt();
    info.pid = map.value(QStringLiteral("pid")).toLongLong();
    info.binary = map.value(QStringLiteral("binary")).toString();
    info.name = map.value(QStringLiteral("name")).toString();
    if (info.nodeId == 0)
      continue;
    next.insert(info.nodeId, info);
  }

  bool changed = false;
  for (auto it = next.constBegin(); it != next.constEnd(); ++it) {
    const auto previous = m_known.constFind(it.key());
    if (previous == m_known.constEnd() ||
        previous->pid != it->pid || previous->binary != it->binary ||
        previous->name != it->name) {
      emit streamAppeared(it.value());
      changed = true;
    }
  }
  for (auto it = m_known.constBegin(); it != m_known.constEnd(); ++it) {
    if (!next.contains(it.key())) {
      emit streamRemoved(it.key());
      changed = true;
    }
  }
  m_known = next;
  if (changed)
    emit streamsChanged();
}

} // namespace TrophyBackend
