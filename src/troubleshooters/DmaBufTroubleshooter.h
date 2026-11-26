/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "capture/IVideoCaptureSource.h"
#include <QObject>
#include <QTimer>
#include <QVariantMap>
#include <condition_variable>
#include <deque>
#include <future>
#include <mutex>

namespace TrophyBackend {

// Evidence from delivered buffers and the production encoder, not capabilities.
struct DmaBufTestEvidence {
  qint64 dmaFrames = 0, shmFrames = 0;
  qint64 singlePlane = 0, multiPlane = 0;
  qint64 direct = 0, egl = 0, vpp = 0;
  qint64 accepted = 0, decoded = 0, fallback = 0, dropped = 0;
  QString encoder, error;
  QVariantMap report() const;
};

class DmaBufTroubleshooter : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool available READ available NOTIFY changed)
  Q_PROPERTY(QString stage READ stage NOTIFY changed)
  Q_PROPERTY(QVariantMap result READ result NOTIFY changed)
public:
  explicit DmaBufTroubleshooter(QObject *parent = nullptr);
  ~DmaBufTroubleshooter() override;
  bool busy() const { return bool(m_run); }
  bool available() const { return m_available; }
  QString stage() const { return m_stage; }
  QVariantMap result() const { return m_result; }
  void setAvailable(bool available);
  Q_INVOKABLE void start();
  Q_INVOKABLE void cancel();
signals:
  void changed();
private:
  friend class DmaBufTroubleshooterTest;
  struct Run {
    std::mutex mutex;
    std::condition_variable wake;
    std::deque<VideoFrame> frames;
    bool stopping = false;
    DmaBufTestEvidence evidence;
  };
  static void probe(Run &run, const std::function<void()> &complete);
  void receive(const VideoFrame &frame);
  void stop(const QString &reason = {});
  void poll();
  std::function<std::unique_ptr<IVideoCaptureSource>()> m_captureFactory;
  std::function<void(Run &, const std::function<void()> &)> m_probe = probe;
  std::unique_ptr<IVideoCaptureSource> m_capture;
  std::unique_ptr<Run> m_run;
  std::future<void> m_worker;
  QTimer m_deadline, m_poll;
  bool m_available = true;
  bool m_cancelled = false;
  quint64 m_generation = 0;
  QString m_stage, m_stopReason;
  QVariantMap m_result;
};
} // namespace TrophyBackend
