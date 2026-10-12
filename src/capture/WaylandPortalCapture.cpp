/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WaylandPortalCapture.h"

#include "capture/EglDmaBuf.h"

#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusInterface>
#include <QDBusMessage>
#include <QDBusMetaType>
#include <QDBusObjectPath>
#include <QDBusPendingCall>
#include <QDBusReply>
#include <QDebug>
#include <QElapsedTimer>
#include <QMap>
#include <QStringList>
#include <QUuid>
#include <chrono>

#include <pipewire/extensions/metadata.h>
#include <pipewire/type.h>
#include <spa/param/props.h>
#include <spa/param/buffers.h>
#include <spa/utils/result.h>
#include <spa/pod/iter.h>
#include <algorithm>
#include <cstring>
#include <vector>

extern "C" {
#include <libavutil/pixfmt.h>
#include <libdrm/drm_fourcc.h>
}

namespace TrophyBackend {

struct WaylandStream {
  uint32_t nodeId;
  QVariantMap options;
};

} // namespace TrophyBackend

Q_DECLARE_METATYPE(TrophyBackend::WaylandStream)

inline QDBusArgument &operator<<(QDBusArgument &argument,
                                 const TrophyBackend::WaylandStream &stream) {
  argument.beginStructure();
  argument << stream.nodeId << stream.options;
  argument.endStructure();
  return argument;
}

inline const QDBusArgument &operator>>(const QDBusArgument &argument,
                                       TrophyBackend::WaylandStream &stream) {
  argument.beginStructure();
  argument >> stream.nodeId >> stream.options;
  argument.endStructure();
  return argument;
}

namespace {
// Moved out of function scope for cleaner code
struct NodeLookupData {
  uint32_t targetId;
  QString name;
  uint32_t pid = 0;
  bool found = false;

  struct pw_main_loop *loop = nullptr;
  int pendingSync = 0;
  bool syncComplete = false;
  bool coreFailed = false;
  struct spa_hook coreListener = {};
  struct spa_hook registryListener = {};

  struct pw_metadata *metadata = nullptr;
  struct spa_hook metadataListener = {};
  struct pw_registry *registry = nullptr;

  uint32_t clientId = 0xFFFFFFFFu;
  QMap<uint32_t, QString> clientNames; // Stores id -> application.name
};

bool waitForCoreSync(NodeLookupData *data, struct pw_core *core) {
  data->syncComplete = false;
  data->pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
  if (data->pendingSync < 0)
    return false;

  QElapsedTimer elapsed;
  elapsed.start();
  struct pw_loop *loop = pw_main_loop_get_loop(data->loop);
  while (!data->syncComplete && !data->coreFailed && elapsed.elapsed() < 250) {
    if (pw_loop_iterate(loop, 50) < 0)
      return false;
  }
  return data->syncComplete && !data->coreFailed;
}
} // namespace

namespace TrophyBackend {

WaylandPortalCapture::WaylandPortalCapture(QObject *parent)
    : IVideoCaptureSource(parent) {
  qDBusRegisterMetaType<WaylandStream>();
  qDBusRegisterMetaType<QList<WaylandStream>>();
}

WaylandPortalCapture::~WaylandPortalCapture() {
  stopCapture();
  stopPipeWireStream();
}

bool WaylandPortalCapture::initialize(const CaptureConfig &config) {
  m_config = config;
  m_restoreToken = config.waylandRestoreToken;
  m_restoreTokenUsed = !m_restoreToken.isEmpty();
  m_resolution = QSize(0, 0); // Will be updated by PipeWire
  m_useDmaBuf = config.useDmaBuf;
  m_dmaBufNegotiated = false;
  m_offeredModifiers.clear();
  m_buffersResultLogged = false;
#ifdef HAVE_EGL
  if (m_useDmaBuf) {
    const auto mods = queryDmaBufModifiers(DRM_FORMAT_XRGB8888);
    // Offer tiled (non-LINEAR) modifiers first: LINEAR DMA-BUF is not
    // allocatable on some drivers (verified NVIDIA), which makes the
    // compositor agree at format level then fall back to SHM buffers.
    for (uint64_t m : mods) {
      if (m != DRM_FORMAT_MOD_LINEAR && m != DRM_FORMAT_MOD_INVALID)
        m_offeredModifiers.push_back(m);
    }
    for (uint64_t m : mods) {
      if (m == DRM_FORMAT_MOD_LINEAR)
        m_offeredModifiers.push_back(m);
    }
    // Bound the offer list: one EnumFormat pod per modifier.
    if (m_offeredModifiers.size() > 8)
      m_offeredModifiers.resize(8);
    qDebug() << "Wayland Portal: EGL reports" << mods.size()
             << "DMA-BUF modifiers for XRGB8888, offering"
             << m_offeredModifiers.size();
  }
#endif
  m_dmaBufFrames = 0;
  m_shmFrames = 0;
  m_droppedBuffers = 0;
  m_lastBufferType = -1;
  m_multiPlaneWarned = false;
  m_gpuOnlyWarned = false;

  qDebug() << "Wayland Portal capture initialized. Restore token available:"
           << !m_restoreToken.isEmpty()
           << "DMA-BUF:" << (m_useDmaBuf ? "enabled" : "disabled");
  return true;
}

void WaylandPortalCapture::setFrameCallback(FrameCallback callback) {
  std::lock_guard<std::mutex> lock(m_callbackMutex);
  m_callback = std::move(callback);
}

bool WaylandPortalCapture::startCapture() {
  if (m_capturing) {
    return true;
  }
  requestScreenShare();
  return true;
}

void WaylandPortalCapture::stopCapture() {
  if (!m_capturing) {
    // Still drop a lingering portal session (e.g. init'd but never
    // started, or stopped twice) so no tray indicator is left behind.
    closePortalSession();
    return;
  }
  stopPipeWireStream();
  m_capturing = false;
  closePortalSession();
  emit captureStopped();
  qDebug() << "Wayland Portal capture stopped";
}

void WaylandPortalCapture::closePortalSession() {
  if (m_sessionHandle.isEmpty())
    return;
  const QString handle = m_sessionHandle;
  m_sessionHandle.clear();
  if (!QDBusConnection::sessionBus().isConnected())
    return;
  QDBusInterface session("org.freedesktop.portal.Desktop", handle,
                         "org.freedesktop.portal.Session",
                         QDBusConnection::sessionBus());
  if (!session.isValid()) {
    qDebug() << "Wayland Portal: no session interface for" << handle;
    return;
  }
  // Fire-and-forget: a blocking Close on the GUI thread can hang teardown
  // if the portal side is ever slow to answer.
  session.asyncCall("Close");
  qDebug() << "Wayland Portal: session closed:" << handle;
}

bool WaylandPortalCapture::isCapturing() const { return m_capturing; }

CaptureCapabilities WaylandPortalCapture::capabilities() const {
  CaptureCapabilities caps;
  caps.supportsWindowCapture = true;
  caps.supportsMonitorCapture = true;
  caps.supportsRegionCapture = false;
  caps.supportsCursorCapture = true;
  caps.supportsHardwareFrames = true; // PipeWire can provide DMA-BUF
  caps.maxFps = 60;
  return caps;
}

QSize WaylandPortalCapture::nativeResolution() const { return m_resolution; }

void WaylandPortalCapture::requestScreenShare() {
  if (m_capturing)
    return;

  QDBusInterface portal(
      "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast", QDBusConnection::sessionBus());

  if (!portal.isValid()) {
    qCritical() << "Portal ScreenCast interface not valid!";
    emit captureError("Portal ScreenCast interface not valid!");
    return;
  }

  // 1. Create Session
  QString token =
      QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
  QVariantMap options;
  options["session_handle_token"] = token;
  options["handle_token"] = token;

  QDBusMessage msg = portal.call("CreateSession", options);
  if (msg.type() == QDBusMessage::ErrorMessage || msg.arguments().isEmpty()) {
    qCritical() << "Failed to call CreateSession:" << msg.errorMessage();
    const QString reason = msg.type() == QDBusMessage::ErrorMessage
                               ? msg.errorMessage()
                               : QStringLiteral("portal returned no request handle");
    emit captureError("Failed to call CreateSession: " + reason);
    return;
  }

  QDBusObjectPath requestPath = msg.arguments().at(0).value<QDBusObjectPath>();
  if (requestPath.path().isEmpty()) {
    emit captureError("Failed to call CreateSession: invalid request handle");
    return;
  }

  QDBusConnection::sessionBus().connect(
      "org.freedesktop.portal.Desktop", requestPath.path(),
      "org.freedesktop.portal.Request", "Response", this,
      SLOT(onSessionCreated(unsigned int, QVariantMap)));

  m_pendingRequestPath = requestPath.path();
  qDebug() << "Wayland Portal: Session creation requested...";
}

void WaylandPortalCapture::onSessionCreated(unsigned int response,
                                            const QVariantMap &results) {
  QDBusConnection::sessionBus().disconnect(
      "org.freedesktop.portal.Desktop", m_pendingRequestPath,
      "org.freedesktop.portal.Request", "Response", this,
      SLOT(onSessionCreated(unsigned int, QVariantMap)));
  m_pendingRequestPath.clear();

  if (response != 0) {
    qWarning() << "Wayland Portal: Session creation failed or cancelled";
    emit captureError("Wayland Portal: Session creation failed or cancelled");
    return;
  }

  m_sessionHandle = results["session_handle"].toString();
  if (m_sessionHandle.isEmpty()) {
    qCritical() << "Wayland Portal: successful CreateSession response has no session handle";
    emit captureError("Wayland Portal: portal returned no session handle");
    return;
  }
  qDebug() << "Wayland Portal: Session created:" << m_sessionHandle;

  requestSelectSources();
}

void WaylandPortalCapture::requestSelectSources() {
  // 2. Select Sources
  QDBusInterface portal(
      "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast", QDBusConnection::sessionBus());

  QString token =
      QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
  QVariantMap options;
  options["handle_token"] = token;
  options["multiple"] = false;
  // Game approvals list windows only (shorter picker); manual recordings
  // keep monitors as well. 1 = Monitor, 2 = Window.
  const unsigned int sourceTypes =
      m_config.windowOnly ? 2u : (1u | 2u);
  options["types"] = sourceTypes;
  options["cursor_mode"] = static_cast<unsigned int>(2); // Embedded = 2
  options["persist_mode"] = static_cast<unsigned int>(2);

  if (!m_restoreToken.isEmpty()) {
    options["restore_token"] = m_restoreToken;
  }
  // Fingerprint (not the token itself) + requested types: a restore that
  // still shows a picker is diagnosable from the log — e.g. a token saved
  // by a differently-attributed process, or a monitor token against a
  // windows-only request.
  qDebug() << "Wayland Portal: SelectSources types:" << sourceTypes
           << "restore token:"
           << (m_restoreToken.isEmpty()
                   ? QStringLiteral("none")
                   : m_restoreToken.left(4) + QStringLiteral("..."));

  QDBusMessage msg =
      portal.call("SelectSources", QDBusObjectPath(m_sessionHandle), options);
  if (msg.type() == QDBusMessage::ErrorMessage || msg.arguments().isEmpty()) {
    const QString reason = msg.type() == QDBusMessage::ErrorMessage
                               ? msg.errorMessage()
                               : QStringLiteral("portal returned no request handle");
    qCritical() << "Failed to call SelectSources:" << reason;
    emit captureError("Failed to select capture source: " + reason);
    closePortalSession();
    return;
  }
  QDBusObjectPath requestPath = msg.arguments().at(0).value<QDBusObjectPath>();
  if (requestPath.path().isEmpty()) {
    emit captureError("Failed to select capture source: invalid request handle");
    closePortalSession();
    return;
  }

  QDBusConnection::sessionBus().connect(
      "org.freedesktop.portal.Desktop", requestPath.path(),
      "org.freedesktop.portal.Request", "Response", this,
      SLOT(onSourcesSelected(unsigned int, QVariantMap)));
  m_pendingRequestPath = requestPath.path();
}

void WaylandPortalCapture::onSourcesSelected(unsigned int response,
                                             const QVariantMap &results) {
  QDBusConnection::sessionBus().disconnect(
      "org.freedesktop.portal.Desktop", m_pendingRequestPath,
      "org.freedesktop.portal.Request", "Response", this,
      SLOT(onSourcesSelected(unsigned int, QVariantMap)));
  m_pendingRequestPath.clear();

  if (response != 0) {
    // A saved restore token may have gone stale (game reinstalled, portal
    // reset permissions). Do NOT retry without it here: that would pop a
    // bare portal picker with no context. Hand back to the app, which
    // deletes the stale token and re-authorizes through the New Game prompt
    // (a picker following that click is the user's own action).
    if (m_restoreTokenUsed) {
      qWarning() << "Wayland Portal: source selection rejected saved restore "
                    "token; re-authorize instead of showing a picker";
      emit restoreTokenStale(m_restoreToken);
      return;
    }
    qWarning() << "Wayland Portal: Source selection failed or cancelled";
    emit captureError("Wayland Portal: Source selection failed or cancelled");
    return;
  }

  // 3. Start
  QDBusInterface portal(
      "org.freedesktop.portal.Desktop", "/org/freedesktop/portal/desktop",
      "org.freedesktop.portal.ScreenCast", QDBusConnection::sessionBus());

  QString token =
      QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-');
  QVariantMap options;
  options["handle_token"] = token;

  QDBusMessage msg =
      portal.call("Start", QDBusObjectPath(m_sessionHandle), "", options);
  if (msg.type() == QDBusMessage::ErrorMessage || msg.arguments().isEmpty()) {
    const QString reason = msg.type() == QDBusMessage::ErrorMessage
                               ? msg.errorMessage()
                               : QStringLiteral("portal returned no request handle");
    qCritical() << "Failed to call Start:" << reason;
    emit captureError("Failed to start portal capture: " + reason);
    closePortalSession();
    return;
  }
  QDBusObjectPath requestPath = msg.arguments().at(0).value<QDBusObjectPath>();
  if (requestPath.path().isEmpty()) {
    emit captureError("Failed to start portal capture: invalid request handle");
    closePortalSession();
    return;
  }

  QDBusConnection::sessionBus().connect(
      "org.freedesktop.portal.Desktop", requestPath.path(),
      "org.freedesktop.portal.Request", "Response", this,
      SLOT(onStartResponse(unsigned int, QVariantMap)));
  m_pendingRequestPath = requestPath.path();
}

void WaylandPortalCapture::onStartResponse(unsigned int response,
                                           const QVariantMap &results) {
  QDBusConnection::sessionBus().disconnect(
      "org.freedesktop.portal.Desktop", m_pendingRequestPath,
      "org.freedesktop.portal.Request", "Response", this,
      SLOT(onStartResponse(unsigned int, QVariantMap)));
  m_pendingRequestPath.clear();

  if (response != 0) {
    // Same rule at Start stage: no silent picker retry — re-authorize.
    if (m_restoreTokenUsed) {
      qWarning() << "Wayland Portal: Start rejected saved restore token; "
                    "re-authorize instead of showing a picker";
      emit restoreTokenStale(m_restoreToken);
      return;
    }
    qWarning() << "Wayland Portal: Failed to start screencast";
    emit captureError("Wayland Portal: Failed to start screencast");
    return;
  }

  if (results.contains("restore_token")) {
    m_restoreToken = results["restore_token"].toString();
    emit restoreTokenUpdated(m_restoreToken);
  }

  QVariant streamsVar = results["streams"];
  if (!streamsVar.isValid()) {
    qCritical() << "Wayland Portal: No PipeWire streams returned";
    emit captureError("Wayland Portal: No PipeWire streams returned");
    return;
  }

  QList<WaylandStream> streams;
  if (streamsVar.canConvert<QDBusArgument>()) {
    streams =
        qdbus_cast<QList<WaylandStream>>(streamsVar.value<QDBusArgument>());
  }

  if (streams.isEmpty()) {
    qCritical() << "Wayland Portal: Stream list is empty";
    emit captureError("Wayland Portal: Stream list is empty");
    return;
  }

  const WaylandStream &firstStream = streams.first();
  uint32_t nodeId = firstStream.nodeId;
  QVariantMap streamOptions = firstStream.options;

  m_pipewireNodeId = static_cast<int>(nodeId);

  // Fallback portal metadata
  if (streamOptions.contains("title")) {
    m_sourceLabel = streamOptions["title"].toString();
  }
  if (streamOptions.contains("app_id")) {
    m_sourceAppId = streamOptions["app_id"].toString();
    if (m_sourceLabel.isEmpty()) {
      m_sourceLabel = m_sourceAppId;
      int lastDot = m_sourceLabel.lastIndexOf('.');
      if (lastDot != -1)
        m_sourceLabel = m_sourceLabel.mid(lastDot + 1);
    }
  }

  // Establish capture before asking PipeWire for optional identity metadata.
  // The metadata query is best-effort and bounded; it must never delay the
  // start notification or prevent frames from flowing.
  if (!startPipeWireStream(nodeId)) {
    closePortalSession();
    emit captureError("Failed to initialize the PipeWire capture stream");
    return;
  }
  m_capturing = true;
  emit captureStarted();

  // --- PipeWire Meta-data lookup ---
  NodeLookupData lookupData;
  lookupData.targetId = nodeId;
  lookupData.loop = pw_main_loop_new(nullptr);
  struct pw_context *context = nullptr;
  struct pw_core *core = nullptr;
  bool lookupReady = false;
  if (lookupData.loop) {
    context = pw_context_new(pw_main_loop_get_loop(lookupData.loop), nullptr, 0);
    if (context)
      core = pw_context_connect(context, nullptr, 0);
  }
  if (!lookupData.loop || !context || !core) {
    qWarning() << "Wayland Portal: unable to create PipeWire metadata lookup;"
                  " using portal labels";
  }

  static const struct pw_core_events core_events = [] {
    pw_core_events e = {};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = [](void *data, uint32_t id, int seq) {
      auto *d = static_cast<NodeLookupData *>(data);
      if (id == PW_ID_CORE && seq == d->pendingSync)
        d->syncComplete = true;
    };
    e.error = [](void *data, uint32_t, int, int, const char *) {
      static_cast<NodeLookupData *>(data)->coreFailed = true;
    };
    return e;
  }();
  if (core) {
    pw_core_add_listener(core, &lookupData.coreListener, &core_events,
                         &lookupData);
    lookupReady = true;
  }

  static const struct pw_metadata_events metadata_events = [] {
    pw_metadata_events e = {};
    e.version = PW_VERSION_METADATA_EVENTS;
    e.property = [](void *data, uint32_t subject, const char *key,
                    const char *type, const char *value) {
      auto *d = static_cast<NodeLookupData *>(data);
      if (key && value && subject == d->targetId) {
        if (strcmp(key, "window.label") == 0 ||
            strcmp(key, "node.description") == 0) {
          d->name = QString::fromUtf8(value);
          d->found = true;
        }
      }
      return 0;
    };
    return e;
  }();

  struct pw_registry *registry = core
                                     ? pw_core_get_registry(
                                           core, PW_VERSION_REGISTRY, 0)
                                     : nullptr;
  lookupData.registry = registry;

  static const struct pw_registry_events reg_events = [] {
    pw_registry_events e = {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = [](void *data, uint32_t id, uint32_t permissions,
                  const char *type, uint32_t version,
                  const struct spa_dict *props) {
      auto *d = static_cast<NodeLookupData *>(data);
      if (!props)
        return;

      if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
        const char *appName = spa_dict_lookup(props, PW_KEY_APP_NAME);
        if (appName) {
          d->clientNames[id] = QString::fromUtf8(appName);
          // Retrospectively apply if we already found the target node
          if (id == d->clientId) {
            QString name = d->clientNames[id];
            if (!name.isEmpty() &&
                (d->name.isEmpty() || d->name.contains("kwin") ||
                 d->name.contains("wayland"))) {
              d->name = name;
            }
            d->found = true;
          }
        }
      } else if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0 &&
                 id == d->targetId) {
        const char *desc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
        const char *name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        const char *pidStr = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
        const char *cliId = spa_dict_lookup(props, PW_KEY_CLIENT_ID);

        if (desc)
          d->name = QString::fromUtf8(desc);
        else if (name)
          d->name = QString::fromUtf8(name);

        if (pidStr)
          d->pid = QString(pidStr).toUInt();

        if (cliId) {
          d->clientId = QString(cliId).toUInt();
          // Apply immediately if we already saw the client
          if (d->clientNames.contains(d->clientId)) {
            QString clientName = d->clientNames[d->clientId];
            if (!clientName.isEmpty() &&
                (d->name.isEmpty() || d->name.contains("kwin") ||
                 d->name.contains("wayland"))) {
              d->name = clientName;
            }
          }
        }
        d->found = true;
      } else if (strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0 &&
                 !d->metadata) {
        const char *metaName = spa_dict_lookup(props, "metadata.name");
        if (metaName && strcmp(metaName, "settings") != 0) {
          d->metadata = (struct pw_metadata *)pw_registry_bind(
              d->registry, id, type, PW_VERSION_METADATA, 0);
          if (d->metadata) {
            pw_metadata_add_listener(d->metadata, &d->metadataListener,
                                     &metadata_events, d);
          }
        }
      }
    };
    return e;
  }();
  if (registry) {
    pw_registry_add_listener(registry, &lookupData.registryListener,
                             &reg_events, &lookupData);
  } else {
    lookupReady = false;
  }

  // First sync to process all existing globals (nodes, clients, metadata
  // objects)
  if (lookupReady)
    lookupReady = waitForCoreSync(&lookupData, core);

  // Second sync: If we bound to a metadata object, wait for it to stream its
  // properties
  if (lookupReady && lookupData.metadata)
    lookupReady = waitForCoreSync(&lookupData, core);

  if (lookupData.found) {
    bool isGeneric = lookupData.name.toLower().contains("kwin") ||
                     lookupData.name.toLower().contains("wayland");
    if (m_sourceLabel.isEmpty() || (!isGeneric && lookupData.pid != 0)) {
      m_sourceLabel = lookupData.name;
    }
    m_sourcePid = lookupData.pid;
    qDebug() << "Wayland Portal selected source:" << m_sourceLabel
             << "PID:" << m_sourcePid;
  }

  if (lookupData.metadata) {
    spa_hook_remove(&lookupData.metadataListener);
    pw_proxy_destroy((struct pw_proxy *)lookupData.metadata);
  }
  if (registry) {
    spa_hook_remove(&lookupData.registryListener);
    pw_proxy_destroy((struct pw_proxy *)registry);
  }
  if (core) {
    spa_hook_remove(&lookupData.coreListener);
    pw_core_disconnect(core);
  }
  if (context)
    pw_context_destroy(context);
  if (lookupData.loop)
    pw_main_loop_destroy(lookupData.loop);
  // ------------------------

  emit screenShareGranted(QString::number(nodeId));
}

bool WaylandPortalCapture::startPipeWireStream(uint32_t nodeId) {
  pw_init(nullptr, nullptr);

  m_pw_loop = pw_thread_loop_new("trophy-wayland-capture", nullptr);
  if (!m_pw_loop) {
    qCritical() << "Failed to create PipeWire thread loop";
    return false;
  }

  m_pw_context = pw_context_new(pw_thread_loop_get_loop(m_pw_loop), nullptr, 0);
  if (!m_pw_context) {
    qCritical() << "Failed to create PipeWire context";
    stopPipeWireStream();
    return false;
  }

  if (pw_thread_loop_start(m_pw_loop) < 0) {
    qCritical() << "Failed to start PipeWire thread loop";
    stopPipeWireStream();
    return false;
  }
  m_pw_loopStarted = true;

  pw_thread_loop_lock(m_pw_loop);

  m_pw_core = pw_context_connect(m_pw_context, nullptr, 0);
  if (!m_pw_core) {
    qCritical() << "Failed to connect to PipeWire core";
    pw_thread_loop_unlock(m_pw_loop);
    stopPipeWireStream();
    return false;
  }

  static const pw_stream_events stream_events = [] {
    pw_stream_events e = {};
    e.version = PW_VERSION_STREAM_EVENTS;
    e.state_changed = WaylandPortalCapture::on_stream_state_changed;
    e.param_changed = WaylandPortalCapture::on_stream_param_changed;
    e.process = WaylandPortalCapture::on_stream_process;
    return e;
  }();

  m_pw_stream = pw_stream_new(m_pw_core, "trophy-video-stream",
                              pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                                                PW_KEY_MEDIA_CATEGORY,
                                                "Capture", nullptr));

  if (!m_pw_stream) {
    qCritical() << "Failed to create PipeWire stream";
    pw_thread_loop_unlock(m_pw_loop);
    stopPipeWireStream();
    return false;
  }

  pw_stream_add_listener(m_pw_stream, &m_pw_stream_listener, &stream_events,
                         this);

  uint8_t buffer[4096];
  spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));

  if (m_useDmaBuf) {
    // --- DMA-BUF negotiation (Stage A) ---
    // Offer one XRGB8888 format per EGL-queried modifier (tiled first,
    // LINEAR last), then BGRx shared memory as the final fallback.
    // The compositor picks the first format it can back with real
    // buffers. Without EGL data a single modifier-agnostic format is
    // offered, as before.
    std::vector<const spa_pod *> params;
    params.reserve(m_offeredModifiers.size() + 2);
    if (!m_offeredModifiers.empty()) {
      QStringList modHex;
      for (uint64_t mod : m_offeredModifiers) {
        modHex << QString::number(mod, 16);
        params.push_back(reinterpret_cast<const spa_pod *>(
            spa_pod_builder_add_object(
                &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
                SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
                SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
                SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_xRGB),
                SPA_FORMAT_VIDEO_modifier,
                SPA_POD_Long(static_cast<int64_t>(mod)))));
      }
      qDebug() << "Wayland Portal: offering DMA-BUF modifiers:"
               << modHex.join(u',');
    } else {
      params.push_back(reinterpret_cast<const spa_pod *>(
          spa_pod_builder_add_object(
              &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
              SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
              SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
              SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_xRGB),
              SPA_FORMAT_VIDEO_modifier, SPA_POD_CHOICE_FLAGS_Long(0))));
    }

    // Final choice: BGRx (shared memory fallback)
    params.push_back(reinterpret_cast<const spa_pod *>(
        spa_pod_builder_add_object(
            &b, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat,
            SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video),
            SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw),
            SPA_FORMAT_VIDEO_format, SPA_POD_Id(SPA_VIDEO_FORMAT_BGRx))));

    int res = pw_stream_connect(m_pw_stream, PW_DIRECTION_INPUT, nodeId,
                                (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS),
                                params.data(), params.size());
    if (res < 0) {
      qCritical() << "Failed to connect PipeWire stream:" << spa_strerror(res);
      pw_thread_loop_unlock(m_pw_loop);
      stopPipeWireStream();
      return false;
    }
  } else {
    // DMA-BUF disabled by user – always use shared memory
    const spa_pod *params[1];
    spa_video_info_raw info =
        SPA_VIDEO_INFO_RAW_INIT(.format = SPA_VIDEO_FORMAT_BGRx);
    params[0] = spa_format_video_raw_build(&b, SPA_PARAM_EnumFormat, &info);

    int res = pw_stream_connect(m_pw_stream, PW_DIRECTION_INPUT, nodeId,
                                (pw_stream_flags)(PW_STREAM_FLAG_AUTOCONNECT |
                                                  PW_STREAM_FLAG_MAP_BUFFERS),
                                params, 1);
    if (res < 0) {
      qCritical() << "Failed to connect PipeWire stream:" << spa_strerror(res);
      pw_thread_loop_unlock(m_pw_loop);
      stopPipeWireStream();
      return false;
    }
  }

  pw_thread_loop_unlock(m_pw_loop);
  return true;
}

void WaylandPortalCapture::stopPipeWireStream() {
  const uint64_t totalFrames = m_dmaBufFrames + m_shmFrames;
  if (totalFrames > 0 || m_droppedBuffers > 0) {
    qDebug() << "Wayland Portal: capture summary DMA-BUF frames:" << m_dmaBufFrames
             << "SHM frames:" << m_shmFrames
             << "dropped buffers:" << m_droppedBuffers;
  }
  if (m_pw_loop && m_pw_loopStarted) {
    pw_thread_loop_stop(m_pw_loop);
    m_pw_loopStarted = false;
  }

  if (m_pw_stream) {
    pw_stream_destroy(m_pw_stream);
    m_pw_stream = nullptr;
  }

  if (m_pw_core) {
    pw_core_disconnect(m_pw_core);
    m_pw_core = nullptr;
  }

  if (m_pw_context) {
    pw_context_destroy(m_pw_context);
    m_pw_context = nullptr;
  }

  if (m_pw_loop) {
    pw_thread_loop_destroy(m_pw_loop);
    m_pw_loop = nullptr;
  }
}

void WaylandPortalCapture::on_stream_state_changed(void *data,
                                                   enum pw_stream_state old,
                                                   enum pw_stream_state state,
                                                   const char *error) {
  WaylandPortalCapture *self = static_cast<WaylandPortalCapture *>(data);
  qDebug() << "PipeWire stream state changed:" << pw_stream_state_as_string(old)
           << "->" << pw_stream_state_as_string(state);

  if (state == PW_STREAM_STATE_ERROR) {
    qCritical() << "PipeWire stream error:"
                << (error ? error : "Unknown error");
    emit self->captureError(error ? error : "Unknown PipeWire error");
  }
}

void WaylandPortalCapture::on_stream_param_changed(
    void *data, unsigned int id, const struct spa_pod *param) {
  WaylandPortalCapture *self = static_cast<WaylandPortalCapture *>(data);
  if (!param)
    return;

  if (id == SPA_PARAM_Buffers && !self->m_buffersResultLogged) {
    // Ground truth for whether the DmaBuf buffer-type request was honored
    // by the compositor. Logged once per capture.
    self->m_buffersResultLogged = true;
    QStringList typeNames;
    auto describeMask = [&typeNames](int32_t mask) {
      if (mask & (1 << SPA_DATA_MemPtr))
        typeNames << "MemPtr";
      if (mask & (1 << SPA_DATA_MemFd))
        typeNames << "MemFd";
      if (mask & (1 << SPA_DATA_DmaBuf))
        typeNames << "DmaBuf";
    };
    if (spa_pod_is_object(param)) {
      const struct spa_pod_object *obj =
          reinterpret_cast<const struct spa_pod_object *>(param);
      const struct spa_pod_prop *prop = nullptr;
      SPA_POD_OBJECT_FOREACH(obj, prop) {
        if (prop->key != SPA_PARAM_BUFFERS_dataType)
          continue;
        if (SPA_POD_TYPE(&prop->value) == SPA_TYPE_Int) {
          describeMask(
              reinterpret_cast<const struct spa_pod_int *>(&prop->value)
                  ->value);
        } else if (SPA_POD_TYPE(&prop->value) == SPA_TYPE_Choice) {
          const struct spa_pod_choice *c =
              reinterpret_cast<const struct spa_pod_choice *>(&prop->value);
          if (SPA_POD_CHOICE_VALUE_SIZE(c) == sizeof(int32_t) &&
              SPA_POD_CHOICE_N_VALUES(c) > 0)
            describeMask(
                *(const int32_t *)SPA_POD_CHOICE_VALUES(c));
        }
      }
    }
    qDebug() << "Wayland Portal: negotiated buffer types:"
             << (typeNames.isEmpty() ? QStringLiteral("(unparsed)")
                                     : typeNames.join(u','));
  }

  if (id == SPA_PARAM_Format) {
    // Check whether the compositor negotiated a DMA-BUF modifier-aware format.
    // If so, SPA_FORMAT_VIDEO_modifier will be present in the format object.
    // NOTE: zero-initialized — when the compositor picks the SHM fallback
    // (BGRx, no modifier property) the parser leaves .modifier untouched,
    // and stack garbage here once faked "DMA-BUF active".
    spa_video_info_raw info{};
    if (spa_format_video_raw_parse(param, &info) < 0)
      return;

    // DMA-BUF is active only when the compositor picked one of our xRGB +
    // modifier offers. The BGRx offer is the shared-memory fallback.
    const bool pickedDmaBufFormat = (info.format == SPA_VIDEO_FORMAT_xRGB);
    self->m_dmaBufNegotiated = self->m_useDmaBuf && pickedDmaBufFormat &&
                                (info.modifier != DRM_FORMAT_MOD_INVALID);

    // Store the actual negotiated modifier for use per-frame.
    self->m_negotiatedModifier = self->m_dmaBufNegotiated
                                     ? info.modifier
                                     : DRM_FORMAT_MOD_LINEAR;

    // Convert SPA video format to DRM format for DMA-BUF descriptor
    switch (info.format) {
    case SPA_VIDEO_FORMAT_BGRx:
      self->m_negotiatedFormat = DRM_FORMAT_XRGB8888;
      break;
    case SPA_VIDEO_FORMAT_RGBx:
      self->m_negotiatedFormat = DRM_FORMAT_XBGR8888;
      break;
    case SPA_VIDEO_FORMAT_xRGB:
      self->m_negotiatedFormat = DRM_FORMAT_XRGB8888;
      break;
    case SPA_VIDEO_FORMAT_xBGR:
      self->m_negotiatedFormat = DRM_FORMAT_XBGR8888;
      break;
    default:
      // Fallback to XRGB8888 for unknown formats
      self->m_negotiatedFormat = DRM_FORMAT_XRGB8888;
      qWarning() << "Unknown SPA video format:" << info.format 
                 << "falling back to DRM_FORMAT_XRGB8888";
      break;
    }

    self->m_resolution = QSize(info.size.width, info.size.height);
    qDebug() << "Wayland Portal: Negotiated resolution:" << self->m_resolution
             << "DMA-BUF active:" << self->m_dmaBufNegotiated
             << "Format:" << QString::number(self->m_negotiatedFormat, 16)
             << "Modifier:" << QString::number(self->m_negotiatedModifier, 16);

    // ---- Stage 2: Buffer-type negotiation ----
    // Now that we know the compositor CAN export DMA-BUF, explicitly request
    // DMA-BUF buffer objects. Without this step KWin still sends MemFd buffers
    // even though the format modifier was accepted.
    if (self->m_dmaBufNegotiated) {
      uint8_t bufBuf[512];
      spa_pod_builder bb = SPA_POD_BUILDER_INIT(bufBuf, sizeof(bufBuf));

      // Offer DmaBuf as primary, MemFd as fallback – KWin will pick DmaBuf.
      const spa_pod *bufParams[1];
      bufParams[0] = reinterpret_cast<const spa_pod *>(spa_pod_builder_add_object(
          &bb,
          SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
          SPA_PARAM_BUFFERS_buffers,  SPA_POD_CHOICE_RANGE_Int(4, 1, 32),
          SPA_PARAM_BUFFERS_blocks,   SPA_POD_Int(1),
          SPA_PARAM_BUFFERS_dataType, SPA_POD_CHOICE_FLAGS_Int(
              (1 << SPA_DATA_DmaBuf) | (1 << SPA_DATA_MemFd))));

      pw_stream_update_params(self->m_pw_stream, bufParams, 1);
      qDebug() << "Wayland Portal: Requested DMA-BUF buffer objects from compositor";
    }

    emit self->ready();
  }
}

void WaylandPortalCapture::on_stream_process(void *data) {
  WaylandPortalCapture *self = static_cast<WaylandPortalCapture *>(data);
  pw_buffer *b = pw_stream_dequeue_buffer(self->m_pw_stream);
  if (!b)
    return;

  spa_buffer *buf = b->buffer;
  const auto &d = buf->datas[0];

  // Per-frame qDebug at 60fps floods the log; only log verbosely when
  // TROPHY_VIDEO_DEBUG is set, otherwise log the first frame of each
  // buffer type, buffer-type transitions, and every 600th frame.
  const bool verbose = qEnvironmentVariableIsSet("TROPHY_VIDEO_DEBUG");
  auto noteBufferType = [self, verbose](int curType, uint64_t count,
                                        const char *kind) {
    const bool changed =
        self->m_lastBufferType != -1 && self->m_lastBufferType != curType;
    self->m_lastBufferType = curType;
    if (verbose || changed || count == 1 || count % 600 == 0) {
      qDebug() << "WaylandPortalCapture:" << kind << "frame #" << count
               << (changed ? "(buffer-type transition)" : "");
    }
  };

  // --- Timestamp (shared by both paths) ---
  const int64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
                         std::chrono::steady_clock::now().time_since_epoch())
                         .count();

  VideoFrame frame;
  frame.width     = self->m_resolution.width();
  frame.height    = self->m_resolution.height();
  frame.timestamp = ts;

  if (d.type == SPA_DATA_DmaBuf && d.fd >= 0) {
    // ---- Zero-copy DMA-BUF path ----
    frame.isDmaBuf    = true;    frame.dmaBufFd    = static_cast<int>(d.fd);
    frame.dmaBufStride  = d.chunk->stride;
    frame.dmaBufOffset  = static_cast<int>(d.chunk->offset);
    // The modifier was captured during format negotiation and stored as
    // m_resolution is to the geometry. We expose it via the DRM format
    // constant for the negotiated format.
    frame.drmFormat   = self->m_negotiatedFormat;
    frame.drmModifier = self->m_negotiatedModifier;
    frame.data        = d.data ? static_cast<const uint8_t *>(d.data) : nullptr; // CPU fallback if available
    frame.stride      = frame.dmaBufStride;
    
    // Set pixel format based on available data
    if (frame.data) {
      frame.pixelFormat = AV_PIX_FMT_BGRA; // CPU data is in BGRA format
    } else {
      frame.pixelFormat = AV_PIX_FMT_DRM_PRIME; // GPU-only DMA-BUF
    }

    // Validate that we only have single-plane DMA-BUF (common for screen capture)
    if (buf->n_datas > 1 && !self->m_multiPlaneWarned) {
      self->m_multiPlaneWarned = true;
      qWarning() << "Multi-plane DMA-BUF not supported, has" << buf->n_datas << "planes (using plane 0; further occurrences suppressed)";
    }

    if (!frame.data && !self->m_gpuOnlyWarned) {
      self->m_gpuOnlyWarned = true;
      qWarning() << "WaylandPortalCapture: GPU-only DMA-BUF frame (no CPU mapping); encoders without a working hwmap import will drop it (further occurrences suppressed)";
    }

    // Invoke under the callback lock: setFrameCallback (session teardown)
    // blocks until this delivery finishes, so the session can't be
    // destroyed mid-invoke.
    {
      std::lock_guard<std::mutex> lock(self->m_callbackMutex);
      if (self->m_callback) {
        self->m_dmaBufFrames++;
        noteBufferType(1, self->m_dmaBufFrames, "dmaBuf");
        self->m_callback(frame);
      }
    }
  } else if (d.data) {
    // ---- Shared-memory (MemFd / mmap) path ----
    frame.isDmaBuf    = false;
    frame.data        = static_cast<const uint8_t *>(d.data);
    frame.stride      = d.chunk->stride;
    frame.pixelFormat = AV_PIX_FMT_BGRA;

    {
      std::lock_guard<std::mutex> lock(self->m_callbackMutex);
      if (self->m_callback) {
        self->m_shmFrames++;
        noteBufferType(0, self->m_shmFrames, "shm");
        self->m_callback(frame);
      }
    }
  } else {
    // No valid data pointer and no FD — skip silently.
    self->m_droppedBuffers++;
    if (verbose || self->m_droppedBuffers == 1 || self->m_droppedBuffers % 600 == 0) {
      qWarning() << "PipeWire: buffer has neither DMA-BUF fd nor mapped data"
                 << "(total dropped:" << self->m_droppedBuffers << ")";
    }
  }

  pw_stream_queue_buffer(self->m_pw_stream, b);
}

} // namespace TrophyBackend
