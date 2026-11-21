/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "capture/IVideoCaptureSource.h"
#include <QObject>
#include <QSet>
#include <QTimer>
#include <QVariantMap>
#include <functional>
#include <memory>

namespace TrophyBackend {
class GameDetectionService;
class SettingsManager;

// Capture policy is independent of the output's retention strategy. Managers
// drain/finalize their own output before arming the single continuation timer.
class CaptureLifecycle : public QObject {
public:
  struct Authorization {
    QSet<QString> selectionFences;
  };
  explicit CaptureLifecycle(QObject *parent = nullptr);
  void configure(GameDetectionService *detector, SettingsManager *settings);
  void pin(const QString &game, const QVariantMap &identity);
  QString game() const { return m_game; }
  bool hasPinnedInstance() const { return !m_instance.isEmpty(); }
  enum class InstanceState { Unknown, Running, AwaitingCensus, Ended };
  // Exit classification is independent of permission/selection fences:
  // an ended sharing grant never authorizes another capture attempt.
  InstanceState pinnedInstanceState() const;
  bool identityIsCurrent() const;
  bool instanceIsCurrent() const;
  bool awaitingCensus() const;
  bool canRecover(IVideoCaptureSource::CaptureErrorKind kind, bool initializing) const;
  bool requiresSelection() const;
  void fenceSelection(const QString &instance);
  std::shared_ptr<Authorization> authorization() const { return m_authorization; }
  void setAuthorization(std::shared_ptr<Authorization> authorization);
  QSet<QString> &selectionFences() { return m_authorization->selectionFences; }

  // Wait for native retirement before calling this. Revalidate at delivery;
  // an authoritative census gap waits without opening another portal session.
  void scheduleRetry(int delayMs, std::function<bool()> outputReady,
                     std::function<void()> retry, std::function<void()> abandon);
  void cancelRetry();

  // All source events are attempt-scoped, including token deliveries. The
  // manager's generation predicate also fences asynchronous output preparation.
  void bindSource(IVideoCaptureSource *source, const QString &game,
                  const QString &tokenKey, bool manual,
                  std::function<bool()> current, std::function<void()> ready,
                  std::function<void(const QString &)> error,
                  std::function<void(const QString &)> stale);

private:
  void retryTimeout();
  GameDetectionService *m_detector = nullptr;
  SettingsManager *m_settings = nullptr;
  QString m_game, m_key, m_instance;
  std::shared_ptr<Authorization> m_authorization = std::make_shared<Authorization>();
  QTimer m_retryTimer;
  int m_delayMs = 1500;
  std::function<bool()> m_outputReady;
  std::function<void()> m_retry, m_abandon;
};
} // namespace TrophyBackend
