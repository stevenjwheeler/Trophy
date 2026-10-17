/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QFutureWatcher>
#include <QHash>
#include <QObject>
#include <QQueue>
#include <QSet>
#include <QThreadPool>
#include <QUrl>
#include <atomic>
#include <memory>

class QQuickImageProvider;

namespace TrophyBackend {
struct PreviewFrames;
struct PreviewResult {
  QString path;
  QString key;
  int count = 0;
};

// On-demand, serial, software-only generation. No media I/O on the GUI thread.
class PreviewCache : public QObject {
  Q_OBJECT
public:
  explicit PreviewCache(QObject *parent = nullptr);
  ~PreviewCache() override;
  Q_INVOKABLE void requestPreview(const QUrl &source, int requestId);
  Q_INVOKABLE void cancelPreview(int requestId);
  void preparePreview(const QString &sourcePath);
  QQuickImageProvider *createImageProvider();
  Q_INVOKABLE void retainFrames(const QString &key);
  Q_INVOKABLE void releaseFrames(const QString &key);
signals:
  // Empty preview means failure; callers keep their static thumbnail.
  void previewReady(const QUrl &source, int requestId, const QUrl &preview);
  void previewFramesReady(const QUrl &source, int requestId, const QUrl &preview,
                          const QString &key, int count, int fps);
private:
  void startNext();
  QThreadPool m_pool;
  QFutureWatcher<PreviewResult> m_watcher;
  QHash<QString, QSet<int>> m_waiters;
  QQueue<QString> m_queue;
  QString m_active;
  bool m_completing = false;
  int m_backgroundTicket = -1;
  std::shared_ptr<std::atomic_bool> m_cancel;
  std::shared_ptr<PreviewFrames> m_frames;
};
}
