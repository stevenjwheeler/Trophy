/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "CaptureLifecycle.h"
#include "detection/GameDetectionService.h"
#include "settings/SettingsManager.h"
#include <QPointer>
#include <utility>

namespace TrophyBackend {
CaptureLifecycle::CaptureLifecycle(QObject *parent) : QObject(parent) {
  m_retryTimer.setSingleShot(true);
  connect(&m_retryTimer, &QTimer::timeout, this, &CaptureLifecycle::retryTimeout);
}

void CaptureLifecycle::configure(GameDetectionService *detector, SettingsManager *settings) {
  m_detector = detector;
  m_settings = settings;
}

void CaptureLifecycle::pin(const QString &game, const QVariantMap &identity) {
  m_game = game;
  m_key = identity.value("detectedGameKey").toString();
  m_instance = identity.value("detectedGameInstance").toString();
}

bool CaptureLifecycle::identityIsCurrent() const {
  return m_detector && m_settings && !m_instance.isEmpty() &&
         m_detector->captureIdentityInfo(m_game).value("detectedGameInstance").toString() == m_instance &&
         m_detector->currentGameKey() == m_key && !m_detector->isBlocked(m_key) &&
         !m_authorization->selectionFences.contains(m_instance) &&
         !m_settings->getRestoreTokenForIdentity(m_key, m_game).isEmpty();
}

bool CaptureLifecycle::instanceIsCurrent() const {
  return identityIsCurrent() && m_detector->instanceAlive(m_instance);
}

CaptureLifecycle::InstanceState CaptureLifecycle::pinnedInstanceState() const {
  if (!m_detector || m_instance.isEmpty()) return InstanceState::Unknown;
  if (m_detector->instanceAlive(m_instance)) return InstanceState::Running;
  if (m_detector->instanceLaunchContextAlive(m_instance)) return InstanceState::AwaitingCensus;
  return InstanceState::Ended;
}

bool CaptureLifecycle::awaitingCensus() const {
  return identityIsCurrent() && m_detector->instanceLaunchContextAlive(m_instance);
}

bool CaptureLifecycle::canRecover(IVideoCaptureSource::CaptureErrorKind kind,
                                  bool initializing) const {
  using Kind = IVideoCaptureSource::CaptureErrorKind;
  return (kind == Kind::SourceDestroyed || (initializing && kind == Kind::SourceNotReady)) &&
         (instanceIsCurrent() || awaitingCensus());
}

bool CaptureLifecycle::requiresSelection() const {
  if (!m_detector) return false;
  const auto instance = m_detector->captureIdentityInfo(m_detector->currentGameName())
                            .value("detectedGameInstance").toString();
  return !instance.isEmpty() && m_authorization->selectionFences.contains(instance);
}

void CaptureLifecycle::fenceSelection(const QString &instance) {
  if (!instance.isEmpty()) m_authorization->selectionFences.insert(instance);
}

void CaptureLifecycle::setAuthorization(std::shared_ptr<Authorization> authorization) {
  Q_ASSERT(authorization);
  m_authorization = std::move(authorization);
}

void CaptureLifecycle::cancelRetry() {
  m_retryTimer.stop();
  m_outputReady = {};
  m_retry = {};
  m_abandon = {};
}

void CaptureLifecycle::scheduleRetry(int delayMs, std::function<bool()> outputReady,
                                     std::function<void()> retry,
                                     std::function<void()> abandon) {
  cancelRetry();
  m_delayMs = qMax(1, delayMs);
  m_outputReady = std::move(outputReady);
  m_retry = std::move(retry);
  m_abandon = std::move(abandon);
  m_retryTimer.start(m_delayMs);
}

void CaptureLifecycle::retryTimeout() {
  if (!identityIsCurrent() || (!instanceIsCurrent() && !awaitingCensus())) {
    auto abandon = std::move(m_abandon);
    cancelRetry();
    if (abandon) abandon();
    return;
  }
  if (!instanceIsCurrent() || (m_outputReady && !m_outputReady())) {
    m_retryTimer.start(m_delayMs);
    return;
  }
  auto retry = std::move(m_retry);
  cancelRetry();
  if (retry) retry();
}

void CaptureLifecycle::bindSource(IVideoCaptureSource *source, const QString &game,
                                  const QString &tokenKey, bool manual,
                                  std::function<bool()> current,
                                  std::function<void()> ready,
                                  std::function<void(const QString &)> error,
                                  std::function<void(const QString &)> stale) {
  const QPointer<IVideoCaptureSource> owner(source);
  auto valid = [owner, current] { return owner && current(); };
  connect(source, &IVideoCaptureSource::ready, this,
          [valid, ready] { if (valid()) ready(); }, Qt::QueuedConnection);
  connect(source, &IVideoCaptureSource::captureError, this,
          [valid, error](const QString &text) { if (valid()) error(text); },
          Qt::QueuedConnection);
  connect(source, &IVideoCaptureSource::restoreTokenUpdated, this,
          [this, valid, game, tokenKey, manual](const QString &token) {
    if (valid() && m_settings && !manual && !game.isEmpty())
      m_settings->setRestoreTokenForIdentity(tokenKey, game, token);
  });
  connect(source, &IVideoCaptureSource::restoreTokenStale, this,
          [this, valid, stale, game, tokenKey, manual](const QString &token) {
    if (!valid()) return;
    // Revoke the rejected capability before teardown or any observer can
    // re-arm capture. This belongs to the originating attempt, even if its
    // game/PID is no longer selected when the application receives the notice.
    // A newer token saved in the meantime must not be removed.
    if (m_settings && !manual && !token.isEmpty()) {
      for (const auto &key : {tokenKey, game})
        if (m_settings->getRestoreToken(key) == token)
          m_settings->removeRestoreToken(key);
    }
    stale(token);
  });
}
} // namespace TrophyBackend
