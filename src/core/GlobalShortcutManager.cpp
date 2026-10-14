/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GlobalShortcutManager.h"

#include <QDBusConnection>
#include <QDBusServiceWatcher>
#include <QDBusVariant>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCallWatcher>
#include <QCoreApplication>
#include <QDebug>
#include <QRegularExpression>
#include <QUuid>

namespace TrophyBackend {

namespace {
const char *kService = "org.freedesktop.portal.Desktop";
const char *kPath = "/org/freedesktop/portal/desktop";
const char *kInterface = "org.freedesktop.portal.GlobalShortcuts";
const char *kRequestInterface = "org.freedesktop.portal.Request";

QString newToken() {
  return QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
}

bool validPortalPath(const QString &path) {
  static const QRegularExpression pattern(
      QStringLiteral("\\A(/[A-Za-z0-9_]+)+\\z"));
  return pattern.match(path).hasMatch();
}

QString requestPathFromReply(const QDBusMessage &reply) {
  if (reply.type() != QDBusMessage::ReplyMessage ||
      reply.arguments().size() != 1 ||
      reply.arguments().first().metaType() != QMetaType::fromType<QDBusObjectPath>())
    return {};
  const QString path = reply.arguments().first().value<QDBusObjectPath>().path();
  return validPortalPath(path) ? path : QString();
}

QDBusMessage portalCall(const QString &method, const QVariantList &arguments) {
  auto message = QDBusMessage::createMethodCall(kService, kPath, kInterface, method);
  message.setArguments(arguments);
  return message;
}
} // namespace

GlobalShortcutManager::GlobalShortcutManager(QObject *parent)
    : QObject(parent) {
  qDBusRegisterMetaType<PortalShortcut>();
  qDBusRegisterMetaType<QList<PortalShortcut>>();
  m_requestTimeout.setSingleShot(true);
  m_requestTimeout.setInterval(10000);
  connect(&m_requestTimeout, &QTimer::timeout, this,
           &GlobalShortcutManager::onRequestTimeout);
  m_availabilityTimeout.setSingleShot(true);
  m_availabilityTimeout.setInterval(5000);
  connect(&m_availabilityTimeout, &QTimer::timeout, this,
          [this] { setAvailable(false); });
  auto *service = new QDBusServiceWatcher(kService, QDBusConnection::sessionBus(),
      QDBusServiceWatcher::WatchForOwnerChange, this);
  connect(service, &QDBusServiceWatcher::serviceOwnerChanged, this,
          [this](const QString &, const QString &oldOwner, const QString &) {
    if (!oldOwner.isEmpty()) {
      clearRequest();
      retireUnboundSession();
    }
    queryAvailability();
  });
  queryAvailability();
  ensureResponseSubscription();
}

bool GlobalShortcutManager::isAvailable() const {
  return m_available;
}

void GlobalShortcutManager::queryAvailability() {
  const quint64 generation = ++m_availabilityGeneration;
  m_availabilityTimeout.start();
  auto message = QDBusMessage::createMethodCall(
      kService, kPath, "org.freedesktop.DBus.Properties", "Get");
  message.setArguments({QString::fromLatin1(kInterface), QStringLiteral("version")});
  auto *watcher = new QDBusPendingCallWatcher(
      QDBusConnection::sessionBus().asyncCall(message, m_availabilityTimeout.interval()), this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, generation](QDBusPendingCallWatcher *finished) {
    const auto reply = finished->reply();
    finished->deleteLater();
    if (generation != m_availabilityGeneration)
      return;
    bool available = false;
    if (reply.type() == QDBusMessage::ReplyMessage && reply.arguments().size() == 1 &&
        reply.arguments().first().metaType() == QMetaType::fromType<QDBusVariant>()) {
      const QVariant version = reply.arguments().first().value<QDBusVariant>().variant();
      available = version.metaType() == QMetaType::fromType<uint>() && version.toUInt() > 0;
    }
    setAvailable(available);
  });
}

void GlobalShortcutManager::setAvailable(bool available) {
  ++m_availabilityGeneration;
  m_availabilityTimeout.stop();
  if (m_available == available)
    return;
  m_available = available;
  emit availabilityChanged();
}

void GlobalShortcutManager::disconnectSessionSignals() {
  QDBusConnection::sessionBus().disconnect(
      QString::fromLatin1(kService), QString::fromLatin1(kPath),
      QString::fromLatin1(kInterface), "Activated", this,
      SLOT(handleActivated(QDBusObjectPath, QString)));
  QDBusConnection::sessionBus().disconnect(
      QString::fromLatin1(kService), QString::fromLatin1(kPath),
      QString::fromLatin1(kInterface), "ShortcutsChanged", this,
      SLOT(onShortcutsChanged(QDBusObjectPath, QList<PortalShortcut>)));
}

void GlobalShortcutManager::onRequestTimeout() {
  if (!m_requestInFlight)
    return;
  qWarning() << "GlobalShortcuts portal request timed out; a later"
                "ensureSetup can start a fresh flow";
  clearRequest();
  // A lost BindShortcuts response leaves an unbound session behind.
  // Retire it too, otherwise requestShortcuts rejects every fresh retry.
  retireUnboundSession();
}

quint64 GlobalShortcutManager::beginRequest(bool binding) {
  const quint64 generation = ++m_requestGeneration;
  m_requestInFlight = true;
  m_bindingRequest = binding;
  m_requestTimeout.start();
  return generation;
}

void GlobalShortcutManager::ensureResponseSubscription() {
  if (m_responseSubscribed)
    return;
  // Same race as the ScreenCast flow: the portal can emit Response
  // within milliseconds of the method reply, before a connect issued
  // after the reply registers its daemon-side match rule. Subscribe
  // once with no path filter; both Response slots key off
  // m_pendingRequestPath and the request generation.
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), QString(),
      QString::fromLatin1(kRequestInterface), "Response", this,
      SLOT(onSessionResponse(unsigned int, QVariantMap, QDBusMessage)));
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), QString(),
      QString::fromLatin1(kRequestInterface), "Response", this,
      SLOT(onBindResponse(unsigned int, QVariantMap, QDBusMessage)));
  m_responseSubscribed = true;
}

void GlobalShortcutManager::clearRequest() {
  m_requestTimeout.stop();
  // No per-request disconnect: the Response subscription is permanent
  // (see ensureResponseSubscription) and both slots ignore anything
  // that is not the current pending path/generation.
  m_pendingRequestPath.clear();
  m_requestInFlight = false;
  m_bindingRequest = false;
  ++m_requestGeneration;
}

void GlobalShortcutManager::watchRequest(const QDBusPendingCall &call,
                                        quint64 generation, bool binding) {
  auto *watcher = new QDBusPendingCallWatcher(call, this);
  connect(watcher, &QDBusPendingCallWatcher::finished, this,
          [this, generation, binding](QDBusPendingCallWatcher *finished) {
    const QDBusMessage reply = finished->reply();
    finished->deleteLater();
    if (!m_requestInFlight || generation != m_requestGeneration)
      return;
    const QString requestPath = requestPathFromReply(reply);
    if (requestPath.isEmpty()) {
      qWarning() << "GlobalShortcuts" << (binding ? "BindShortcuts" : "CreateSession")
                 << "failed or gave no valid request path:" << reply.errorMessage();
      clearRequest();
      retireUnboundSession();
      return;
    }
    // No per-reply connect: the permanent subscription from
    // ensureResponseSubscription() already covers this path. Just
    // publish the dispatch key the Response slot matches on.
    m_pendingRequestPath = requestPath;
    setAvailable(true); // A valid method reply proves interface support too.
    // Do not restart the timer: method wait and Response share one budget.
  });
}

void GlobalShortcutManager::retireUnboundSession() {
  disconnectSessionSignals();
  m_sessionHandle.clear();
  // A portal may push ShortcutsChanged before its Bind response. Such a
  // map cannot remain usable after the owning session has been retired.
  applyBoundShortcuts({});
}

QList<PortalShortcut> GlobalShortcutManager::shortcutDefinitions() {
  PortalShortcut toggle;
  toggle.id = QString::fromLatin1(kToggleRecordingId);
  toggle.options["description"] = QCoreApplication::translate(
      "GlobalShortcutManager", "Start/stop recording the detected game");
  toggle.options["preferred_trigger"] = QStringLiteral("F9");
  PortalShortcut clip;
  clip.id = QString::fromLatin1(kSaveClipId);
  clip.options["description"] = QCoreApplication::translate(
      "GlobalShortcutManager", "Save the last moments as a clip");
  clip.options["preferred_trigger"] = QStringLiteral("F10");
  return {toggle, clip};
}

QVariantMap
GlobalShortcutManager::boundMap(const QList<PortalShortcut> &shortcuts) {
  QVariantMap bound;
  for (const PortalShortcut &s : shortcuts)
    bound.insert(s.id, s.options.value("trigger_description").toString());
  return bound;
}

void GlobalShortcutManager::requestShortcuts() {
  if (m_bound || m_requestInFlight || !m_pendingRequestPath.isEmpty() ||
      !m_sessionHandle.isEmpty())
    return;
  const quint64 generation = beginRequest(false);
  QVariantMap options;
  options["handle_token"] = newToken();
  options["session_handle_token"] = newToken();
  watchRequest(QDBusConnection::sessionBus().asyncCall(
      portalCall("CreateSession", {options}), m_requestTimeout.interval()), generation, false);
}

void GlobalShortcutManager::onSessionResponse(unsigned int response,
                                              const QVariantMap &results,
                                              const QDBusMessage &message) {
  if (!m_requestInFlight || m_bindingRequest || m_pendingRequestPath.isEmpty() ||
      (!message.path().isEmpty() && message.path() != m_pendingRequestPath))
    return;
  clearRequest();
  if (response != 0) {
    qWarning() << "GlobalShortcuts session creation failed or was cancelled";
    return;
  }
  m_sessionHandle = results.value("session_handle").toString();
  if (!validPortalPath(m_sessionHandle)) {
    m_sessionHandle.clear();
    qWarning() << "GlobalShortcuts session gave no handle";
    return;
  }
  setAvailable(true);

  // Activations + remaps for our session.
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), QString::fromLatin1(kPath),
      QString::fromLatin1(kInterface), "Activated", this,
      SLOT(handleActivated(QDBusObjectPath, QString)));
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), QString::fromLatin1(kPath),
      QString::fromLatin1(kInterface), "ShortcutsChanged", this,
      SLOT(onShortcutsChanged(QDBusObjectPath, QList<PortalShortcut>)));

  bindShortcuts();
}

void GlobalShortcutManager::bindShortcuts() {
  if (m_sessionHandle.isEmpty() || m_requestInFlight ||
      !m_pendingRequestPath.isEmpty())
    return;
  const quint64 generation = beginRequest(true);
  QVariantMap options;
  options["handle_token"] = newToken();
  watchRequest(QDBusConnection::sessionBus().asyncCall(portalCall(
      "BindShortcuts", {QVariant::fromValue(QDBusObjectPath(m_sessionHandle)),
      QVariant::fromValue(shortcutDefinitions()), QString(), options}),
      m_requestTimeout.interval()), generation, true);
}

void GlobalShortcutManager::onBindResponse(unsigned int response,
                                           const QVariantMap &results,
                                           const QDBusMessage &message) {
  if (!m_requestInFlight || !m_bindingRequest || m_pendingRequestPath.isEmpty() ||
      (!message.path().isEmpty() && message.path() != m_pendingRequestPath))
    return;
  clearRequest();
  if (response != 0) {
    qWarning() << "GlobalShortcuts bind failed or was cancelled";
    // A cancelled bind leaves no usable session: drop the handle and
    // its signal connections so a retry goes through the full
    // CreateSession flow instead of landing on reconfigure forever.
    retireUnboundSession();
    return;
  }
  QList<PortalShortcut> bound;
  const QVariant raw = results.value("shortcuts");
  if (raw.canConvert<QDBusArgument>()) {
    bound = qdbus_cast<QList<PortalShortcut>>(raw.value<QDBusArgument>());
  }
  if (bound.isEmpty()) {
    qWarning() << "GlobalShortcuts bind returned no shortcuts";
    // A successful Response with unusable data is not a usable bind.
    // Keeping its handle strands ensureSetup on ConfigureShortcuts rather
    // than permitting a fresh CreateSession/BindShortcuts retry.
    retireUnboundSession();
    return;
  }
  applyBoundShortcuts(bound);
  qDebug() << "Global shortcuts bound:" << m_boundShortcuts.keys();
}

void GlobalShortcutManager::applyBoundShortcuts(
    const QList<PortalShortcut> &shortcuts) {
  const QVariantMap next = boundMap(shortcuts);
  const bool bound = !next.isEmpty();
  // Compare the whole map, not just the bool: remapping keys while still
  // bound must still notify, or the UI keeps showing the old triggers
  // until the app restarts.
  if (next == m_boundShortcuts && bound == m_bound)
    return;
  m_boundShortcuts = next;
  m_bound = bound;
  emit boundChanged();
}

void GlobalShortcutManager::onShortcutsChanged(
    const QDBusObjectPath &sessionHandle,
    const QList<PortalShortcut> &shortcuts) {
  if (sessionHandle.path() != m_sessionHandle)
    return;
  qDebug() << "Portal reported shortcut change:" << boundMap(shortcuts);
  applyBoundShortcuts(shortcuts);
}

void GlobalShortcutManager::ensureSetup() {
  if (m_requestInFlight || !m_pendingRequestPath.isEmpty())
    return;
  // requestShortcuts no-ops when a session already exists or a request
  // is in flight; reconfigureShortcuts falls back to it without a
  // session. Exactly one path acts.
  if (m_sessionHandle.isEmpty())
    requestShortcuts();
  else
    reconfigureShortcuts();
}

void GlobalShortcutManager::reconfigureShortcuts() {
  if (m_requestInFlight || !m_pendingRequestPath.isEmpty())
    return;
  if (m_sessionHandle.isEmpty()) {
    requestShortcuts();
    return;
  }
  // Empirical (KDE 6.x GlobalShortcuts backend): a repeat BindShortcuts
  // on a live session returns the existing bindings WITHOUT showing any
  // dialog, so it cannot be used to re-pick keys. ConfigureShortcuts is
  // the spec'd way to ask, and it does show UI — on KDE that means the
  // System Settings keybinds page rather than a compact prompt. Better a
  // heavyweight dialog than a button that does nothing.
  QVariantMap options;
  QDBusConnection::sessionBus().asyncCall(portalCall("ConfigureShortcuts",
      {QVariant::fromValue(QDBusObjectPath(m_sessionHandle)), QString(), options}), 10000);
}

void GlobalShortcutManager::handleActivated(
    const QDBusObjectPath &sessionHandle, const QString &id) {
  if (sessionHandle.path() != m_sessionHandle || m_sessionHandle.isEmpty())
    return;
  simulateActivation(id);
}

void GlobalShortcutManager::simulateActivation(const QString &id) {
  if (!m_enabled)
    return; // switched off in Settings: swallow the keypress
  if (id == QLatin1String(kToggleRecordingId)) {
    emit toggleRecordingRequested();
  } else if (id == QLatin1String(kSaveClipId)) {
    emit saveClipRequested();
  }
}

void GlobalShortcutManager::setEnabled(bool enabled) {
  if (m_enabled != enabled) {
    m_enabled = enabled;
    emit enabledChanged();
  }
}

} // namespace TrophyBackend
