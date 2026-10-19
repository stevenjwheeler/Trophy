/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include "ShareExportEngine.h"
#include <QObject>
#include <QTimer>
#include <QUrl>
#include <QStringList>
#include <future>
#include <memory>
#include <deque>
#include <QHash>

namespace TrophyBackend {
class StorageManager;
class ShareExporter : public QObject {
  Q_OBJECT
  Q_PROPERTY(bool busy READ busy NOTIFY changed)
  Q_PROPERTY(bool inspecting READ inspecting NOTIFY changed)
  Q_PROPERTY(double progress READ progress NOTIFY changed)
  Q_PROPERTY(QString sourcePath READ sourcePath NOTIFY changed)
  Q_PROPERTY(QVariantMap info READ info NOTIFY changed)
  Q_PROPERTY(QString error READ error NOTIFY changed)
  Q_PROPERTY(QString resultPath READ resultPath NOTIFY changed)
  Q_PROPERTY(QVariantList jobs READ jobs NOTIFY changed)
public:
  explicit ShareExporter(StorageManager *storage, QObject *parent = nullptr);
  ~ShareExporter() override;
  bool busy() const { return !m_jobs.empty(); }
  bool inspecting() const { return m_inspecting; }
  double progress() const;
  QVariantList jobs() const;
  QString sourcePath() const { return m_source; }
  QVariantMap info() const { return m_info; }
  QString error() const { return m_error; }
  QString resultPath() const { return m_result; }
  Q_INVOKABLE bool inspect(const QString &source);
   Q_INVOKABLE bool start(const QString &directory, int maxHeight, qint64 targetBytes, const QVariantList &audioStreams);
  Q_INVOKABLE bool startBatch(const QString &directory, int maxHeight, qint64 targetBytes, const QVariantList &captures);
  Q_INVOKABLE bool copyOriginals(const QStringList &sources, const QString &directory);
  Q_INVOKABLE void cancel();
  Q_INVOKABLE void cancelJob(const QString &id);
  Q_INVOKABLE void release();
  Q_INVOKABLE QUrl resultFolder() const;
  Q_INVOKABLE QString localFolderPath(const QUrl &url) const;
signals:
  void changed();
  void finished(const QString &path);
  void failed(const QString &reason);
  // Per-job identity lets the shared toast stack retain this job's completion
  // while the next queued export is already running.
  void jobSettled(const QString &id, const QString &sourcePath,
                  const QString &resultPath, const QString &error, bool cancelled,
                  bool copyOriginal = false);
private:
  struct Result { QVariantMap info; };
  void poll();
  void unpin();
  bool pin(const QString &source);
  void unpinSource(const QString &source);
  void startNext();
  struct Job {
    QString id, source, directory, launchError;
    ShareExportOptions options;
    std::shared_ptr<std::atomic_bool> cancel = std::make_shared<std::atomic_bool>(false);
    std::shared_ptr<std::atomic<double>> progress = std::make_shared<std::atomic<double>>(0.0);
    std::future<ShareExportResult> worker;
    bool started = false;
    bool copyOriginal = false;
  };
  std::deque<std::unique_ptr<Job>> m_jobs;
  QHash<QString, int> m_pins;
  quint64 m_nextJobId = 0;
  StorageManager *m_storage;
  QString m_source, m_error, m_result;
  QVariantMap m_info;
  bool m_inspecting = false, m_pinned = false, m_releaseAfterWork = false;
  std::shared_ptr<std::atomic_bool> m_cancel = std::make_shared<std::atomic_bool>(false);
  std::future<Result> m_worker;
  QTimer m_timer;
};
}
