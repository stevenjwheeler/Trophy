/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#pragma once

#include <QDBusArgument>
#include <QDBusMetaType>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QMap>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVariantMap>

class GlobalShortcutTest;
class GlobalShortcutPortalTest;

namespace TrophyBackend {

/**
 * @brief One portal shortcut: id + {description, preferred_trigger}.
 * Custom D-Bus struct (like WaylandStream) because QtDBus has no
 * built-in QPair marshalling.
 */
struct PortalShortcut {
  QString id;
  QVariantMap options;
};

inline QDBusArgument &operator<<(QDBusArgument &arg,
                                 const PortalShortcut &s) {
  arg.beginStructure();
  arg << s.id << s.options;
  arg.endStructure();
  return arg;
}

inline const QDBusArgument &operator>>(const QDBusArgument &arg,
                                       PortalShortcut &s) {
  arg.beginStructure();
  arg >> s.id >> s.options;
  arg.endStructure();
  return arg;
}

/**
 * @brief System-wide hotkeys via the GlobalShortcuts portal.
 *
 * Registers toggle-recording (F9) and save-clip/bookmark (F10); the portal shows
 * a system dialog on first bind where the user can remap them. Works
 * from inside games. When no portal/backend exists the manager reports
 * unavailable; recording/clip actions remain accessible from the UI and
 * tray. No hardcoded in-app F9/F10 bindings: they would conflict with
 * user-remapped portal shortcuts and could trigger the action twice.
 */
class GlobalShortcutManager : public QObject {
  Q_OBJECT

  Q_PROPERTY(bool available READ isAvailable NOTIFY availabilityChanged)
  Q_PROPERTY(bool bound READ isBound NOTIFY boundChanged)
  Q_PROPERTY(QVariantMap boundShortcuts READ boundShortcuts NOTIFY boundChanged)
  Q_PROPERTY(bool enabled READ isEnabled WRITE setEnabled NOTIFY enabledChanged)

public:
  static constexpr const char *kToggleRecordingId = "toggle-recording";
  static constexpr const char *kSaveClipId = "save-clip";

  explicit GlobalShortcutManager(QObject *parent = nullptr);

  bool isAvailable() const;
  bool isBound() const { return m_bound; }
  QVariantMap boundShortcuts() const { return m_boundShortcuts; }
  bool isEnabled() const { return m_enabled; }

  // Shortcut catalogue (ids, descriptions, preferred triggers).
  static QList<PortalShortcut> shortcutDefinitions();
  // Bound-list (id, trigger_description) to display map. Public for tests.
  static QVariantMap boundMap(const QList<PortalShortcut> &shortcuts);
  // Applies a portal shortcut list and notifies when anything the UI
  // renders changes — including a remap that stays "bound" (the boolean
  // alone is not enough; QML binds to the map). Public for tests.
  void applyBoundShortcuts(const QList<PortalShortcut> &shortcuts);

public slots:
  void requestShortcuts();
  // One entry point for all "let the user pick keys" UI: opens the
  // portal dialog whether we have no session yet (full bind flow) or
  // an existing session with missing keys (remap dialog). Safe no-op
  // without a portal.
  void ensureSetup();
  // Master kill-switch: while off, portal activations are swallowed
  // (the portal keeps no release call, so we ignore instead).
  // Wired to SettingsManager::globalShortcutsEnabled.
  void setEnabled(bool enabled);
  // Reopen the portal's key dialog on the live session. Falls back to
  // requestShortcuts when there is no session yet.
  void reconfigureShortcuts();
  // Entry point for the D-Bus Activated signal; public for tests.
  void handleActivated(const QDBusObjectPath &sessionHandle,
                       const QString &id);
  // Emit the action signal for an id (no session check; for tests and
  // future non-portal shortcut sources like a daemon).
  void simulateActivation(const QString &id);

signals:
  void availabilityChanged();
  void boundChanged();
  void enabledChanged();
  void toggleRecordingRequested();
  void saveClipRequested();

private slots:
  void onSessionResponse(unsigned int response, const QVariantMap &results,
                         const QDBusMessage &message = QDBusMessage());
  void onBindResponse(unsigned int response, const QVariantMap &results,
                      const QDBusMessage &message = QDBusMessage());
  void onShortcutsChanged(const QDBusObjectPath &sessionHandle,
                          const QList<PortalShortcut> &shortcuts);
  void onRequestTimeout();

private:
  friend class ::GlobalShortcutTest;
  friend class ::GlobalShortcutPortalTest;

  void bindShortcuts();
  // Emits availabilityChanged only on an actual flip (QML binds to
  // the available property for the no-portal fallback path).
  void setAvailable(bool available);
  void disconnectSessionSignals();
  void retireUnboundSession();
  quint64 beginRequest(bool binding);
  void clearRequest();
  void watchRequest(const QDBusPendingCall &call, quint64 generation,
                     bool binding);
  void queryAvailability();
  void ensureResponseSubscription();

  QString m_sessionHandle;
  QString m_pendingRequestPath;
  // Permanent portal Request/Response subscription: the daemon-side
  // match rule must exist before any method call goes out (see
  // ensureResponseSubscription). Both Response slots already drop
  // anything whose path is not the current m_pendingRequestPath.
  bool m_responseSubscribed = false;
  quint64 m_requestGeneration = 0;
  bool m_requestInFlight = false;
  bool m_bindingRequest = false;
  QVariantMap m_boundShortcuts;
  bool m_bound = false;
  bool m_enabled = true;
  bool m_available = false;
  quint64 m_availabilityGeneration = 0;
  QTimer m_availabilityTimeout;
  // Covers the method reply AND the subsequent portal Response. Expiry
  // invalidates reply watchers before allowing a fresh flow.
  QTimer m_requestTimeout;
};

} // namespace TrophyBackend

Q_DECLARE_METATYPE(TrophyBackend::PortalShortcut)
Q_DECLARE_METATYPE(QList<TrophyBackend::PortalShortcut>)
