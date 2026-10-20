/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QObject>
#include <QElapsedTimer>
#include <QTimer>
#include <QVariantList>
#include <QStringList>
#include <functional>
#include <QMap>

class DeletionQueueTest;
namespace TrophyBackend {
class StorageManager;
// Files stay in place (and counted/pinned) during the grace period. Hiding
// is a library-view operation, so Undo and a crash before expiry lose no data.
class DeletionQueue : public QObject {
  Q_OBJECT
  Q_PROPERTY(QVariantList pending READ pending NOTIFY pendingChanged)
public:
  static constexpr int kUndoMs = 10000;
  explicit DeletionQueue(StorageManager *storage, QObject *parent = nullptr);
  ~DeletionQueue() override;
  QVariantList pending() const;
  Q_INVOKABLE QVariantMap status(const QString &id) const;
  Q_INVOKABLE bool enqueue(const QStringList &paths);
  Q_INVOKABLE bool undo(const QString &id);
signals:
  void pendingChanged();
  void hiddenPathsChanged(const QStringList &paths);
  void failed(const QString &message);
private slots:
  void tick();
private:
  friend class ::DeletionQueueTest;
  struct Batch { QString id; QStringList paths; qint64 started; bool committing = false; QMap<QString, QString> identities; };
  QVariantMap describe(const Batch &batch) const;
  void publish();
  void completed(int deleted, int total);
  StorageManager *m_storage;
  QList<Batch> m_batches;
  QElapsedTimer m_clock;
  std::function<qint64()> m_now;
  QTimer m_timer;
};
}
