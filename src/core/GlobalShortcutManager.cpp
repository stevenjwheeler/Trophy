/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "GlobalShortcutManager.h"

#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusReply>
#include <QCoreApplication>
#include <QDebug>
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

QDBusInterface portalInterface() {
  return QDBusInterface(QString::fromLatin1(kService),
                        QString::fromLatin1(kPath),
                        QString::fromLatin1(kInterface),
                        QDBusConnection::sessionBus());
}

void connectRequest(const QString &requestPath, QObject *receiver,
                    const char *slot) {
  QDBusConnection::sessionBus().connect(
      QString::fromLatin1(kService), requestPath,
      QString::fromLatin1(kRequestInterface), "Response", receiver, slot);
}

void disconnectRequest(const QString &requestPath, QObject *receiver,
                       const char *slot) {
  QDBusConnection::sessionBus().disconnect(
      QString::fromLatin1(kService), requestPath,
      QString::fromLatin1(kRequestInterface), "Response", receiver, slot);
}
} // namespace

GlobalShortcutManager::GlobalShortcutManager(QObject *parent)
    : QObject(parent) {
  qDBusRegisterMetaType<PortalShortcut>();
  qDBusRegisterMetaType<QList<PortalShortcut>>();
}

bool GlobalShortcutManager::isAvailable() const {
  return portalInterface().isValid();
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
  if (m_bound || !m_pendingRequestPath.isEmpty() || !m_sessionHandle.isEmpty())
    return;
  QDBusInterface portal = portalInterface();
  if (!portal.isValid()) {
    qInfo() << "GlobalShortcuts portal unavailable; using in-app shortcuts";
    return;
  }
  QVariantMap options;
  options["handle_token"] = newToken();
  options["session_handle_token"] = newToken();
  QDBusMessage reply = portal.call("CreateSession", options);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    qWarning() << "GlobalShortcuts CreateSession failed:"
               << reply.errorMessage();
    return;
  }
  const QDBusObjectPath requestPath =
      reply.arguments().at(0).value<QDBusObjectPath>();
  connectRequest(requestPath.path(), this,
                 SLOT(onSessionResponse(unsigned int, QVariantMap)));
  m_pendingRequestPath = requestPath.path();
}

void GlobalShortcutManager::onSessionResponse(unsigned int response,
                                              const QVariantMap &results) {
  disconnectRequest(m_pendingRequestPath, this,
                    SLOT(onSessionResponse(unsigned int, QVariantMap)));
  m_pendingRequestPath.clear();
  if (response != 0) {
    qWarning() << "GlobalShortcuts session creation failed or was cancelled";
    return;
  }
  m_sessionHandle = results.value("session_handle").toString();
  if (m_sessionHandle.isEmpty()) {
    qWarning() << "GlobalShortcuts session gave no handle";
    return;
  }

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
  if (m_sessionHandle.isEmpty() || !m_pendingRequestPath.isEmpty())
    return;
  QDBusInterface portal = portalInterface();
  QVariantMap options;
  options["handle_token"] = newToken();
  QDBusMessage reply = portal.call(
      "BindShortcuts", QDBusObjectPath(m_sessionHandle),
      QVariant::fromValue(shortcutDefinitions()), QString(), options);
  if (reply.type() == QDBusMessage::ErrorMessage) {
    qWarning() << "GlobalShortcuts BindShortcuts failed:"
               << reply.errorMessage();
    return;
  }
  const QDBusObjectPath requestPath =
      reply.arguments().at(0).value<QDBusObjectPath>();
  connectRequest(requestPath.path(), this,
                 SLOT(onBindResponse(unsigned int, QVariantMap)));
  m_pendingRequestPath = requestPath.path();
}

void GlobalShortcutManager::onBindResponse(unsigned int response,
                                           const QVariantMap &results) {
  disconnectRequest(m_pendingRequestPath, this,
                    SLOT(onBindResponse(unsigned int, QVariantMap)));
  m_pendingRequestPath.clear();
  if (response != 0) {
    qWarning() << "GlobalShortcuts bind failed or was cancelled";
    return;
  }
  QList<PortalShortcut> bound;
  const QVariant raw = results.value("shortcuts");
  if (raw.canConvert<QDBusArgument>()) {
    bound = qdbus_cast<QList<PortalShortcut>>(raw.value<QDBusArgument>());
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
  // requestShortcuts no-ops when a session already exists or a request
  // is in flight; reconfigureShortcuts falls back to it without a
  // session. Exactly one path acts.
  if (m_sessionHandle.isEmpty())
    requestShortcuts();
  else
    reconfigureShortcuts();
}

void GlobalShortcutManager::reconfigureShortcuts() {
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
  QDBusInterface portal = portalInterface();
  if (!portal.isValid())
    return;
  QVariantMap options;
  portal.call("ConfigureShortcuts", QDBusObjectPath(m_sessionHandle),
              QString(), options);
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
