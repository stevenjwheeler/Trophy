/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PipeWireAudioSource.h"
#include "AppAudioGroup.h"
#include "ApplicationAudioIdentity.h"

#include <QDebug>
#include <QList>
#include <atomic>
#include <cmath>
#include <cstring>
#include <map>
#include <mutex>
#include <vector>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <qdebug.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

namespace TrophyBackend {

namespace {
constexpr int kPipeWireSyncTimeoutMs = 3000;

template <typename Complete, typename Failed>
static bool runCoreSync(struct pw_main_loop *loop, int sequence,
                        Complete &&complete, Failed &&failed) {
  if (!loop || sequence < 0)
    return false;
  const bool settled = detail::waitForPipeWireSync(
      [loop](int waitMs) {
        return pw_loop_iterate(pw_main_loop_get_loop(loop), waitMs);
      },
      [&] { return complete() || failed(); }, kPipeWireSyncTimeoutMs);
  return settled && complete() && !failed();
}
} // namespace

// ---------------------------------------------------------------------------
// Private implementation struct (keeps PipeWire types out of the header)
// ---------------------------------------------------------------------------

struct PipeWireAudioSource::Private {
  PipeWireAudioSource *q = nullptr;

  struct pw_thread_loop *loop = nullptr;
  struct pw_context *context = nullptr;
  struct pw_core *core = nullptr;
  struct pw_stream *stream = nullptr;
  struct spa_hook stream_listener = {};
  struct spa_hook core_listener = {};
  std::unique_ptr<AppAudioGroup> appGroup;
  // Set on the loop thread, checked during start while the loop is locked.
  // Failed native resources remain owned until stop/reinitialize; never tear
  // them down from a listener (which would invert the lifecycle/loop locks).
  std::atomic<bool> failed{false};
  bool loopStarted = false;
  bool loopLocked = false;

  // Written in initialize() on the GUI thread, read on the PipeWire
  // thread (onProcess) and the GUI thread (start): atomics keep both
  // directions data-race free.
  std::atomic<int> channels{2};
  std::atomic<int> sampleRate{48000};

  // --- stream callbacks ---

  static void onProcess(void *userdata) {
    auto *self = static_cast<Private *>(userdata);

    if (self->failed.load(std::memory_order_acquire))
      return;
    if (self->appGroup && !self->appGroup->acceptsAudio()) {
      // Still dequeue/return producer buffers while registry verification is
      // pending. Never deliver unverified PCM to the mixer/encoder.
      if (auto *buffer = self->q->m_operations.dequeueBuffer(self->stream))
        self->q->m_operations.queueBuffer(self->stream, buffer);
      if (self->appGroup->rejectedRoute())
        self->fail(QStringLiteral("Application audio link does not match the pinned app"));
      return;
    }
    struct pw_buffer *b = self->q->m_operations.dequeueBuffer(self->stream);
    if (!b)
      return;

    self->q->processBuffer(b->buffer,
                          self->channels.load(std::memory_order_relaxed),
                          self->sampleRate.load(std::memory_order_relaxed));
    self->q->m_operations.queueBuffer(self->stream, b);
  }

  void fail(const QString &message) {
    if (failed.exchange(true, std::memory_order_acq_rel))
      return;
    const bool wasCapturing = q->m_capturing.exchange(false);
    q->m_peakLevel.store(0.0f);
    if (wasCapturing)
      emit q->captureStopped();
    emit q->captureError(message);
  }

  static void onCoreError(void *userdata, uint32_t id, int /*seq*/, int res,
                          const char *message) {
    // Object-local errors need not kill an otherwise usable capture core.
    if (id != PW_ID_CORE || res >= 0)
      return;
    static_cast<Private *>(userdata)->fail(
        QString::fromUtf8(message ? message : "PipeWire core error"));
  }

  static void onStateChanged(void *userdata, enum pw_stream_state /*old*/,
                             enum pw_stream_state state, const char *error) {
    auto *self = static_cast<Private *>(userdata);
    qDebug() << "PipeWire stream state:" << pw_stream_state_as_string(state)
             << "[" << self->q->m_displayName << "]";

    if (state == PW_STREAM_STATE_ERROR) {
      qWarning() << "PipeWire stream error:" << (error ? error : "unknown");
      self->fail(QString::fromUtf8(error ? error : "PipeWire error"));
    }

    if (self->appGroup && (state == PW_STREAM_STATE_PAUSED ||
                           state == PW_STREAM_STATE_STREAMING))
      self->appGroup->refresh();

    if (state == PW_STREAM_STATE_STREAMING) {
      // Read back the properties WirePlumber set on the stream after routing.
      // If our requested target.object / node.target was overridden we'll see
      // it here.
      const struct pw_properties *props =
          self->q->m_operations.getProperties(self->stream);
      const char *actualTarget =
          props ? spa_dict_lookup(&props->dict, "target.object") : nullptr;
      const char *actualNode =
          props ? spa_dict_lookup(&props->dict, "node.target") : nullptr;

      const bool hadTarget = (self->q->m_targetNodeId != PW_ID_ANY) ||
                             !self->q->m_targetNodeName.isEmpty() ||
                             !self->q->m_targetSerial.isEmpty();

      if (hadTarget) {
        const QString expected = !self->q->m_targetSerial.isEmpty()
                                     ? self->q->m_targetSerial
                                 : !self->q->m_targetNodeName.isEmpty()
                                     ? self->q->m_targetNodeName
                                     : QString::number(self->q->m_targetNodeId);
        const QString gotTarget = actualTarget ? QString::fromUtf8(actualTarget)
                                               : QStringLiteral("(none)");
        const QString gotNode = actualNode ? QString::fromUtf8(actualNode)
                                           : QStringLiteral("(none)");

        if (actualTarget && expected != gotTarget) {
          qWarning() << "ROUTING FALLBACK [" << self->q->m_displayName << "]"
                     << "— requested target.object:" << expected
                     << "but WirePlumber set:" << gotTarget
                     << "(node.target:" << gotNode << ")";
        } else {
          qDebug() << "Routing requested [" << self->q->m_displayName << "]"
                   << "— target.object:" << gotTarget
                   << "node.target:" << gotNode;
        }
      } else {
        // No specific target requested — log where WirePlumber routed us
        qDebug() << "Auto-routed [" << self->q->m_displayName << "]"
                 << "— target.object:"
                 << (actualTarget ? actualTarget : "(none)")
                 << "node.target:" << (actualNode ? actualNode : "(none)");
      }
    }
  }
};

// ---------------------------------------------------------------------------
// Node enumeration helper (one-shot, synchronous)
// ---------------------------------------------------------------------------

namespace {

// ---------------------------------------------------------------------------
// Per-lookup registry cache.
//
// Fed by registry global-add/remove events during the single bounded sync
// pump, matched after the pump instead of binding every node. Each lookup
// call owns its connection and its cache instance (stack-local), so no
// background subscription exists and no call site timing changes —
// PipeWire is still only contacted post-grant by these same helpers.
// The mutex guards the map for the GUI-thread vs session-retry-path
// requirement; in practice it is uncontended (callbacks fire on the
// calling thread via pw_loop_iterate). Callbacks only insert/erase plain
// data under the lock — never user code, never logging (qDebug message
// handlers count as user code), so copies leave the lock before any
// matching or logging.
// ---------------------------------------------------------------------------

struct CachedNode {
  QString nodeName;
  QString appName;
  QString binary;
  QString appId;
  uint32_t pid = 0;
  bool hasPid = false;
  QString serial;
  QString mediaClass;
};

static CachedNode nodeFromProps(const struct spa_dict *props) {
  CachedNode n;
  if (!props)
    return n;
  const char *v = nullptr;
  if ((v = spa_dict_lookup(props, PW_KEY_NODE_NAME)) != nullptr)
    n.nodeName = QString::fromUtf8(v);
  if ((v = spa_dict_lookup(props, PW_KEY_APP_NAME)) != nullptr)
    n.appName = QString::fromUtf8(v);
  if ((v = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY)) != nullptr)
    n.binary = QString::fromUtf8(v);
  if ((v = spa_dict_lookup(props, PW_KEY_APP_ID)) != nullptr)
    n.appId = QString::fromUtf8(v);
  if ((v = spa_dict_lookup(props, "object.serial")) != nullptr)
    n.serial = QString::fromUtf8(v);
  if ((v = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS)) != nullptr)
    n.mediaClass = QString::fromUtf8(v);
  if ((v = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID)) != nullptr) {
    bool ok = false;
    const uint32_t pid = QString::fromUtf8(v).toUInt(&ok);
    if (ok) {
      n.pid = pid;
      n.hasPid = true;
    }
  }
  return n;
}

struct RegistryCache {
  mutable std::mutex mutex;
  std::map<uint32_t, CachedNode> nodes;

  void upsert(uint32_t id, CachedNode node) {
    std::lock_guard<std::mutex> lock(mutex);
    nodes[id] = std::move(node);
  }
  void erase(uint32_t id) {
    std::lock_guard<std::mutex> lock(mutex);
    nodes.erase(id);
  }
  std::map<uint32_t, CachedNode> copy() const {
    std::lock_guard<std::mutex> lock(mutex);
    return nodes;
  }
};

struct NodeEnumData {
  QString processName;
  QString nodeName; // For sink node lookup by exact name
  uint32_t nodeId = PW_ID_ANY;
  QString foundNodeName;  // PW_KEY_NODE_NAME of the matched app node
  QString foundSerial;    // object.serial of the matched app node
  uint32_t targetPid = 0; // Search by process ID instead of name
  int pendingSync = 0;
  bool syncComplete = false;
  bool syncFailed = false;
  struct pw_main_loop *loop = nullptr;
  struct spa_hook registryListener = {};
  struct spa_hook coreListener = {};
  RegistryCache cache; // id -> announce-time props (add/remove maintained)
};

static void enumRegistryGlobal(void *userdata, uint32_t id,
                               uint32_t /*permissions*/, const char *type,
                               uint32_t /*version*/,
                               const struct spa_dict *props) {
  auto *data = static_cast<NodeEnumData *>(userdata);
  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;
  if (!props)
    return;
  // Cache add side (removal is in enumRegistryRemove). Plain-data
  // insert under the lock; matching/logging below stays outside it.
  data->cache.upsert(id, nodeFromProps(props));

  const char *binary = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
  const char *appName = spa_dict_lookup(props, PW_KEY_APP_NAME);
  const char *pwNodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  const char *appId = spa_dict_lookup(props, PW_KEY_APP_ID);

  // If searching by node name (e.g., "easyeffects_sink" for sink monitor)
  // Also handles numeric node IDs stored as strings
  if (!data->nodeName.isEmpty()) {
    // Check if nodeName is actually a numeric node ID
    bool isNumeric = false;
    uint32_t numericId = data->nodeName.toUInt(&isNumeric);
    if (isNumeric && numericId == id) {
      qDebug() << "Found PipeWire node by ID:" << id;
      data->nodeId = id;
      // Record the real name too: pinning must use the name (or serial),
      // never the bare numeric ID — WirePlumber reads target.object as a
      // serial and IDs collide with serials (easyeffects id/serial skew
      // captured the wrong node live).
      if (pwNodeName)
        data->foundNodeName = QString::fromUtf8(pwNodeName);
      return;
    }
    // Otherwise check by string name
    if (pwNodeName && QString(pwNodeName) == data->nodeName) {
      qDebug() << "Found PipeWire node:" << data->nodeName << "-> ID:" << id;
      data->nodeId = id;
      data->foundNodeName = QString::fromUtf8(pwNodeName);
      return;
    }
    // If we're searching by nodeName and didn't find it, don't fall through to
    // process search
    return;
  }

  // For process name search: only match application playback streams
  // (media.class = "Stream/Output/Audio"). This avoids accidentally
  // matching EasyEffects filter nodes or other internal nodes that may
  // happen to share the app name but cannot deliver audio to a capture stream.
  const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!mediaClass || strcmp(mediaClass, "Stream/Output/Audio") != 0)
    return;

  // Search by PID
  if (data->targetPid != 0) {
    const char *pidStr = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
    if (pidStr && QString(pidStr).toUInt() == data->targetPid) {
      const char *serial = spa_dict_lookup(props, "object.serial");
      qDebug() << "Found PipeWire node for PID" << data->targetPid
               << "-> ID:" << id;
      data->nodeId = id;
      if (pwNodeName)
        data->foundNodeName = QString::fromUtf8(pwNodeName);
      if (serial)
        data->foundSerial = QString::fromUtf8(serial);
      return;
    }
    // If searching by PID and didn't match, stop here
    return;
  }

  // Search by process name
  QString target = data->processName.toLower();

  auto matches = [&](const char *s) {
    return s && QString(s).toLower().contains(target);
  };

  if (matches(binary) || matches(appName) || matches(pwNodeName) ||
      matches(appId)) {
    const char *serial = spa_dict_lookup(props, "object.serial");
    qDebug() << "Found PipeWire node for" << data->processName << "-> ID:" << id
             << "media.class: Stream/Output/Audio"
             << "name:" << (pwNodeName ? pwNodeName : "?")
             << "serial:" << (serial ? serial : "?");
    data->nodeId = id;
    if (pwNodeName)
      data->foundNodeName = QString::fromUtf8(pwNodeName);
    if (serial)
      data->foundSerial = QString::fromUtf8(serial);
    return; // Stop after first match to prevent overwriting with subsequent
            // nodes
  }
}

static void enumRegistryRemove(void *userdata, uint32_t id) {
  auto *data = static_cast<NodeEnumData *>(userdata);
  data->cache.erase(id);
  // A node that matched and then vanished mid-pump must not be reported.
  if (data->nodeId == id) {
    data->nodeId = PW_ID_ANY;
    data->foundNodeName.clear();
    data->foundSerial.clear();
  }
}

static void enumCoreDone(void *userdata, uint32_t id, int seq) {
  auto *data = static_cast<NodeEnumData *>(userdata);
  if (id == PW_ID_CORE && seq == data->pendingSync)
    data->syncComplete = true;
}

static void enumCoreError(void *userdata, uint32_t id, int /*seq*/, int res,
                          const char *message) {
  auto *data = static_cast<NodeEnumData *>(userdata);
  if (id != PW_ID_CORE)
    return;
  data->syncFailed = true;
  qWarning() << "PipeWire node enumeration failed:" << res
             << (message ? message : "core error");
}

/**
 * @brief Find a PipeWire node by name (works for any node type)
 * @param nodeName The node name to search for
 * @param outNodeName Optional: receives PW_KEY_NODE_NAME
 * @param outSerial   Optional: receives object.serial
 * @return PipeWire node ID, or PW_ID_ANY if not found
 */
static uint32_t findNodeByName(const QString &nodeName,
                               QString *outNodeName = nullptr,
                               QString *outSerial = nullptr) {
  if (nodeName.isEmpty())
    return PW_ID_ANY;

  pw_init(nullptr, nullptr);

  NodeEnumData data;
  data.nodeName = nodeName;
  data.loop = pw_main_loop_new(nullptr);
  if (!data.loop)
    return PW_ID_ANY;

  struct pw_context *ctx =
      pw_context_new(pw_main_loop_get_loop(data.loop), nullptr, 0);
  if (!ctx) {
    pw_main_loop_destroy(data.loop);
    return PW_ID_ANY;
  }

  struct pw_core *core = pw_context_connect(ctx, nullptr, 0);
  if (!core) {
    pw_context_destroy(ctx);
    pw_main_loop_destroy(data.loop);
    return PW_ID_ANY;
  }

  static const struct pw_core_events core_events = [] {
    struct pw_core_events e = {};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = enumCoreDone;
    e.error = enumCoreError;
    return e;
  }();
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  struct pw_registry *registry =
      pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

  static const struct pw_registry_events registry_events = [] {
    struct pw_registry_events e = {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = enumRegistryGlobal;
    e.global_remove = enumRegistryRemove;
    return e;
  }();
  pw_registry_add_listener(registry, &data.registryListener, &registry_events,
                           &data);

  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
  if (!runCoreSync(
          data.loop, data.pendingSync, [&] { return data.syncComplete; },
          [&] { return data.syncFailed; })) {
    qWarning() << "PipeWire node enumeration sync timed out or failed";
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(registry));
    pw_core_disconnect(core);
    pw_context_destroy(ctx);
    pw_main_loop_destroy(data.loop);
    return PW_ID_ANY;
  }

  uint32_t result = data.nodeId;
  if (outNodeName)
    *outNodeName = data.foundNodeName;
  if (outSerial)
    *outSerial = data.foundSerial;

  pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(registry));
  pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(data.loop);

  return result;
}

// findSinkByName removed (unused)

// ---------------------------------------------------------------------------
// Default device query — reads PipeWire metadata for default.audio.sink/source
// ---------------------------------------------------------------------------

struct DefaultDeviceData {
  QString sinkName;
  QString sourceName;
  bool gotMetadata = false;
  int pendingSync = 0;
  bool syncComplete = false;
  bool syncFailed = false;
  struct pw_main_loop *loop = nullptr;
  struct pw_core *core = nullptr;
  struct pw_registry *registry = nullptr;
  struct pw_metadata *metadata = nullptr;
  struct spa_hook registryListener = {};
  struct spa_hook coreListener = {};
  struct spa_hook metadataListener = {};
};

static int onDefaultMetaProp(void *userdata, uint32_t subject, const char *key,
                             const char * /*type*/, const char *value) {
  auto *data = static_cast<DefaultDeviceData *>(userdata);
  if (!key || !value || subject != PW_ID_CORE)
    return 0;

  // value is JSON like {"name":"easyeffects_sink"} — extract the name field
  const char *nameKey = strstr(value, "\"name\"");
  if (!nameKey)
    return 0;
  const char *colon = strchr(nameKey + 6, ':');
  if (!colon)
    return 0;
  const char *open = strchr(colon, '"');
  if (!open)
    return 0;
  const char *close = strchr(open + 1, '"');
  if (!close)
    return 0;
  QString name =
      QString::fromUtf8(open + 1, static_cast<int>(close - open - 1));

  if (strcmp(key, "default.audio.sink") == 0)
    data->sinkName = name;
  else if (strcmp(key, "default.audio.source") == 0)
    data->sourceName = name;

  return 0;
}

static const struct pw_metadata_events defaultMetaEvents = [] {
  struct pw_metadata_events e = {};
  e.version = PW_VERSION_METADATA_EVENTS;
  e.property = onDefaultMetaProp;
  return e;
}();

static void onDefaultRegistryGlobal(void *userdata, uint32_t id,
                                    uint32_t /*perms*/, const char *type,
                                    uint32_t version,
                                    const struct spa_dict *props) {
  auto *data = static_cast<DefaultDeviceData *>(userdata);
  if (strcmp(type, PW_TYPE_INTERFACE_Metadata) != 0)
    return;

  const char *metaName =
      props ? spa_dict_lookup(props, "metadata.name") : nullptr;
  if (!metaName || strcmp(metaName, "default") != 0)
    return;

  data->metadata = static_cast<struct pw_metadata *>(
      pw_registry_bind(data->registry, id, type, version, 0));
  if (data->metadata) {
    memset(&data->metadataListener, 0, sizeof(data->metadataListener));
    pw_metadata_add_listener(data->metadata, &data->metadataListener,
                             &defaultMetaEvents, data);
    data->gotMetadata = true;
  }
}

static void onDefaultCoreDone(void *userdata, uint32_t id, int seq) {
  auto *data = static_cast<DefaultDeviceData *>(userdata);
  if (id == PW_ID_CORE && seq == data->pendingSync)
    data->syncComplete = true;
}

static void onDefaultCoreError(void *userdata, uint32_t id, int /*seq*/,
                               int res, const char *message) {
  auto *data = static_cast<DefaultDeviceData *>(userdata);
  if (id != PW_ID_CORE)
    return;
  data->syncFailed = true;
  qWarning() << "PipeWire default-device query failed:" << res
             << (message ? message : "core error");
}

/**
 * @brief Query PipeWire metadata for the default sink and source node names.
 * @return {sinkName, sourceName} as set by KDE/WirePlumber
 */
static std::pair<QString, QString> queryDefaultDevices() {
  pw_init(nullptr, nullptr);

  DefaultDeviceData data;
  data.loop = pw_main_loop_new(nullptr);
  if (!data.loop)
    return {};

  struct pw_context *ctx =
      pw_context_new(pw_main_loop_get_loop(data.loop), nullptr, 0);
  if (!ctx) {
    pw_main_loop_destroy(data.loop);
    return {};
  }

  data.core = pw_context_connect(ctx, nullptr, 0);
  if (!data.core) {
    pw_context_destroy(ctx);
    pw_main_loop_destroy(data.loop);
    return {};
  }

  static const struct pw_core_events core_events = [] {
    struct pw_core_events e = {};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = onDefaultCoreDone;
    e.error = onDefaultCoreError;
    return e;
  }();
  pw_core_add_listener(data.core, &data.coreListener, &core_events, &data);

  data.registry = pw_core_get_registry(data.core, PW_VERSION_REGISTRY, 0);

  static const struct pw_registry_events reg_events = [] {
    struct pw_registry_events e = {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = onDefaultRegistryGlobal;
    return e;
  }();
  pw_registry_add_listener(data.registry, &data.registryListener, &reg_events,
                           &data);

  // First sync: enumerate globals and bind to the metadata object
  data.pendingSync = pw_core_sync(data.core, PW_ID_CORE, 0);
  bool success = runCoreSync(
      data.loop, data.pendingSync, [&] { return data.syncComplete; },
      [&] { return data.syncFailed; });

  // Second sync: flush metadata property events that arrive after binding
  if (data.gotMetadata) {
    data.syncComplete = false;
    data.pendingSync = pw_core_sync(data.core, PW_ID_CORE, 0);
    success = success && runCoreSync(
                             data.loop, data.pendingSync,
                             [&] { return data.syncComplete; },
                             [&] { return data.syncFailed; });
  }

  auto result = success ? std::make_pair(data.sinkName, data.sourceName)
                        : std::pair<QString, QString>{};

  if (data.metadata)
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(data.metadata));
  pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(data.registry));
  pw_core_disconnect(data.core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(data.loop);

  return result;
}

} // namespace

namespace {

// ---------------------------------------------------------------------------
// Info-based node matching.
//
// Registry-global properties are a snapshot at announce time: client-set
// identity props (notably application.process.binary) often arrive later
// via node info updates. Proven live 2026-10-01: Discord's playback stream
// carries binary "Discord" in its current info, but announce-time globals
// lack it, so global-only matching missed a present node on every tick
// while the full node name matched fine. Matching therefore binds
// candidates and reads CURRENT info props.
// ---------------------------------------------------------------------------

struct InfoMatchRequest {
  QString hintLower;      // product-name hint, lowercased (empty if unused)
  bool exactBinary = false;
  uint32_t pid = 0;       // PID hint (0 if unused)
  bool gatherAll = false; // collect every candidate instead of first match
  QVariantList apps;      // gatherAll results: {name, binary, nodeId}
  uint32_t foundId = PW_ID_ANY;
  QString foundName;
  QString foundSerial;
  bool done = false;
};

struct InfoMatchData;

struct InfoBindEntry {
  uint32_t id = PW_ID_ANY;
  struct pw_node *proxy = nullptr;
  struct spa_hook hook = {};
  bool gotInfo = false;
  InfoMatchData *owner = nullptr;
};

struct InfoMatchData {
  InfoMatchRequest *req = nullptr;
  QList<uint32_t> candidates;       // Stream/Output/Audio node ids (phase 1)
  RegistryCache cache; // announce-time props per candidate id (phase 1)
  std::vector<InfoBindEntry> binds; // phase 2 (reserved upfront: stable)
  int pendingSync = 0;
  bool syncComplete = false;
  bool syncFailed = false;
  struct pw_main_loop *loop = nullptr;
  struct spa_hook registryListener = {};
  struct spa_hook coreListener = {};
};

static bool infoPropsMatch(const struct spa_dict *props,
                           const InfoMatchRequest *req) {
  if (!props || !req)
    return false;
  if (req->pid != 0) {
    const char *pidStr = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
    return pidStr && QString(pidStr).toUInt() == req->pid;
  }
  if (req->hintLower.isEmpty())
    return false;
  if (req->exactBinary) {
    const char *binary = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
    return binary && detail::matchesAudioBinary(QString::fromUtf8(binary),
                                                req->hintLower);
  }
  auto matches = [&](const char *key) {
    const char *s = spa_dict_lookup(props, key);
    return s && QString(s).toLower().contains(req->hintLower);
  };
  return matches(PW_KEY_APP_PROCESS_BINARY) || matches(PW_KEY_APP_NAME) ||
         matches(PW_KEY_NODE_NAME) || matches(PW_KEY_APP_ID);
}

static void infoMatchGather(const struct spa_dict *props, uint32_t id,
                            InfoMatchRequest *req) {
  const char *binary = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
  const char *appName = spa_dict_lookup(props, PW_KEY_APP_NAME);
  const char *nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  // Display prefers the binary: it is the stable, recognizable product
  // name ("Discord") while node names are engine-internal
  // ("WEBRTC VoiceEngine"). The voice setting stores the binary, so the
  // dropdown value and the matcher speak the same language.
  QString display;
  if (binary && strlen(binary) > 0)
    display = QString::fromUtf8(binary);
  else if (appName && strlen(appName) > 0)
    display = QString::fromUtf8(appName);
  else if (nodeName)
    display = QString::fromUtf8(nodeName);
  if (display.isEmpty())
    return;
  QVariantMap app;
  app["name"] = display;
  app["binary"] = binary ? QString::fromUtf8(binary) : "";
  app["nodeId"] = id;
  const char *pid = spa_dict_lookup(props, PW_KEY_APP_PROCESS_ID);
  app["pid"] = pid ? QString::fromUtf8(pid).toLongLong() : 0;
  req->apps.append(app);
}

static void infoMatchNodeInfo(void *userdata, const struct pw_node_info *info) {
  auto *entry = static_cast<InfoBindEntry *>(userdata);
  InfoMatchData *data = entry->owner;
  if (!data || !data->req || data->req->done || entry->gotInfo)
    return; // one evaluation per node
  entry->gotInfo = true;
  if (!info || !info->props)
    return;
  InfoMatchRequest *req = data->req;
  if (req->gatherAll) {
    infoMatchGather(info->props, entry->id, req);
    return;
  }
  if (infoPropsMatch(info->props, req)) {
    const char *name = spa_dict_lookup(info->props, PW_KEY_NODE_NAME);
    const char *serial = spa_dict_lookup(info->props, "object.serial");
    req->foundId = entry->id;
    if (name)
      req->foundName = QString::fromUtf8(name);
    if (serial)
      req->foundSerial = QString::fromUtf8(serial);
    req->done = true;
    qDebug() << "Found PipeWire node via info props:" << req->foundName
             << "-> ID:" << entry->id;
    pw_main_loop_quit(data->loop);
  }
}

static void infoMatchRegistryGlobal(void *userdata, uint32_t id,
                                    uint32_t /*permissions*/, const char *type,
                                    uint32_t /*version*/,
                                    const struct spa_dict *props) {
  auto *data = static_cast<InfoMatchData *>(userdata);
  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0 || !props)
    return;
  // Phase-1 prefilter on announce-time class (core SPA property, always
  // present at announce); identity matching happens on current info.
  const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!mediaClass || strcmp(mediaClass, "Stream/Output/Audio") != 0)
    return;
  data->cache.upsert(id, nodeFromProps(props));
  data->candidates.append(id);
}

static void infoMatchRegistryRemove(void *userdata, uint32_t id) {
  auto *data = static_cast<InfoMatchData *>(userdata);
  data->cache.erase(id);
  data->candidates.removeAll(id);
}

static void infoMatchCoreDone(void *userdata, uint32_t id, int seq) {
  auto *data = static_cast<InfoMatchData *>(userdata);
  if (id == PW_ID_CORE && seq == data->pendingSync)
    data->syncComplete = true;
}

static void infoMatchCoreError(void *userdata, uint32_t id, int /*seq*/,
                               int res, const char *message) {
  auto *data = static_cast<InfoMatchData *>(userdata);
  if (id != PW_ID_CORE)
    return;
  data->syncFailed = true;
  qWarning() << "PipeWire info query failed:" << res
             << (message ? message : "core error");
}

static void runInfoMatch(InfoMatchRequest &req) {
  req.foundId = PW_ID_ANY;
  req.done = false;
  pw_init(nullptr, nullptr);

  InfoMatchData data;
  data.req = &req;
  data.loop = pw_main_loop_new(nullptr);
  if (!data.loop)
    return;
  struct pw_context *ctx =
      pw_context_new(pw_main_loop_get_loop(data.loop), nullptr, 0);
  if (!ctx) {
    pw_main_loop_destroy(data.loop);
    data.loop = nullptr;
    return;
  }
  struct pw_core *core = pw_context_connect(ctx, nullptr, 0);
  if (!core) {
    pw_context_destroy(ctx);
    pw_main_loop_destroy(data.loop);
    data.loop = nullptr;
    return;
  }

  static const struct pw_core_events core_events = [] {
    struct pw_core_events e = {};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = infoMatchCoreDone;
    e.error = infoMatchCoreError;
    return e;
  }();
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  struct pw_registry *registry =
      pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
  static const struct pw_registry_events registry_events = [] {
    struct pw_registry_events e = {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = infoMatchRegistryGlobal;
    e.global_remove = infoMatchRegistryRemove;
    return e;
  }();
  pw_registry_add_listener(registry, &data.registryListener, &registry_events,
                           &data);

  // Phase 1: collect candidate output-stream ids.
  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
  bool success = runCoreSync(
      data.loop, data.pendingSync, [&] { return data.syncComplete; },
      [&] { return data.syncFailed; });

  // Phase 1b: match on the announce-time snapshot before binding anything.
  // A hit costs zero binds on the same connection (single pump total); a
  // miss — or gatherAll, which needs current info props because identity
  // fields like the binary arrive post-announce — falls through to the
  // bind phase below. The predicate mirrors infoPropsMatch (announce-time
  // edition — keep in sync); matching runs on copies taken under the lock,
  // in announce order so the first-match choice matches the bind path.
  if (success && !req.gatherAll) {
    const std::map<uint32_t, CachedNode> snapshot = data.cache.copy();
    const QList<uint32_t> order = data.candidates;
    uint32_t hitId = PW_ID_ANY;
    QString hitName;
    QString hitSerial;
    for (uint32_t id : order) {
      const auto it = snapshot.find(id);
      if (it == snapshot.end())
        continue; // pruned by global_remove mid-pump
      const CachedNode &n = it->second;
      bool hit = false;
      if (req.pid != 0) {
        hit = n.hasPid && n.pid == req.pid;
      } else if (!req.hintLower.isEmpty()) {
        auto contains = [&](const QString &s) {
          return !s.isEmpty() && s.toLower().contains(req.hintLower);
        };
        hit = req.exactBinary
                  ? detail::matchesAudioBinary(n.binary, req.hintLower)
                  : contains(n.binary) || contains(n.appName) ||
                        contains(n.nodeName) || contains(n.appId);
      }
      if (hit) {
        hitId = id;
        hitName = n.nodeName;
        hitSerial = n.serial;
        break;
      }
    }
    if (hitId != PW_ID_ANY) {
      req.foundId = hitId;
      req.foundName = hitName;
      req.foundSerial = hitSerial;
      req.done = true;
      qDebug() << "Found PipeWire node via registry snapshot:" << hitName
               << "-> ID:" << hitId;
    }
  }

  // Phase 2: bind the cached candidates and match/gather on current info
  // props. Only candidate Stream/Output/Audio globals are bound — never
  // the full registry — and a phase-1b snapshot hit skips binds entirely
  // (bindIds is empty, so the second sync below is skipped too).
  // The server answers binds (info) before the sync reply on the same
  // connection, so one round trip collects everything; done ends the loop
  // even if some node never infos (no hang).
  static const struct pw_node_events node_events = [] {
    struct pw_node_events e = {};
    e.version = PW_VERSION_NODE_EVENTS;
    e.info = infoMatchNodeInfo;
    return e;
  }();
  data.binds.reserve(data.candidates.size());
  // A snapshot hit binds nothing: the answer is already in req.
  const QList<uint32_t> bindIds = req.done ? QList<uint32_t>{}
                                           : data.candidates;
  for (uint32_t id : bindIds) {
    struct pw_node *proxy = static_cast<struct pw_node *>(pw_registry_bind(
        registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
    if (!proxy)
      continue;
    InfoBindEntry entry;
    entry.id = id;
    entry.proxy = proxy;
    entry.owner = &data;
    data.binds.push_back(entry);
    pw_node_add_listener(data.binds.back().proxy, &data.binds.back().hook,
                         &node_events, &data.binds.back());
  }
  if (!data.binds.empty()) {
    data.syncComplete = false;
    data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
    success = success && runCoreSync(
                             data.loop, data.pendingSync,
                             [&] { return data.syncComplete; },
                             [&] { return data.syncFailed; });
  }

  for (auto &entry : data.binds) {
    if (entry.proxy)
      pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(entry.proxy));
  }
  pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(registry));
  pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(data.loop);
  data.loop = nullptr;
  if (!success) {
    req.foundId = PW_ID_ANY;
    req.apps.clear();
  }
}

} // namespace

uint32_t PipeWireAudioSource::findNodeByProcessName(const QString &processName,
                                                    QString *outNodeName,
                                                    QString *outSerial) {
  // Snapshot first (zero binds on hit), current node info on miss:
  // identity props (notably application.process.binary) can arrive after
  // announce, so globals alone cannot prove absence.
  InfoMatchRequest req;
  req.hintLower = processName.toLower();
  runInfoMatch(req);
  if (outNodeName)
    *outNodeName = req.foundName;
  if (outSerial)
    *outSerial = req.foundSerial;
  return req.foundId;
}

uint32_t PipeWireAudioSource::findNodeByPid(uint32_t pid, QString *outNodeName,
                                            QString *outSerial) {
  if (pid == 0)
    return PW_ID_ANY;

  // Same two-tier matching as names (snapshot hit, else current info).
  // PID props can also arrive after announce (and Proton streams carry no
  // PID at all — then this misses, and the exe-name hint is the fallback).
  InfoMatchRequest req;
  req.pid = pid;
  runInfoMatch(req);
  if (outNodeName)
    *outNodeName = req.foundName;
  if (outSerial)
    *outSerial = req.foundSerial;
  return req.foundId;
}

uint32_t PipeWireAudioSource::findNodeByBinary(const QString &binary,
                                             QString *outNodeName,
                                             QString *outSerial) {
  InfoMatchRequest req;
  req.hintLower = binary.toLower();
  req.exactBinary = true;
  runInfoMatch(req);
  if (outNodeName) *outNodeName = req.foundName;
  if (outSerial) *outSerial = req.foundSerial;
  return req.foundId;
}

// ---------------------------------------------------------------------------
// Audio app enumeration
// ---------------------------------------------------------------------------

// (App enumeration gathers via the info-match core above, so no
// separate globals-only enumerator is kept — it could not see
// post-announce identity props.)

QVariantList PipeWireAudioSource::enumerateAudioApps() {
  // Gather from current node info, not announce-time globals (identity
  // props like the binary arrive after announce — globals would list
  // engine-internal names with empty binaries).
  InfoMatchRequest req;
  req.gatherAll = true;
  runInfoMatch(req);
  return req.apps;
}

QString PipeWireAudioSource::preferredTarget(const QString &serial,
                                             const QString &nodeName,
                                             uint32_t nodeId) {
  if (!serial.isEmpty())
    return serial;
  if (!nodeName.isEmpty())
    return nodeName;
  return QString::number(nodeId);
}

// ---------------------------------------------------------------------------
// PipeWireAudioSource
// ---------------------------------------------------------------------------

PipeWireAudioSource::PipeWireAudioSource(SourceType type, QObject *parent)
    : IAudioSource(parent), d(std::make_unique<Private>()), m_type(type),
      m_targetNodeId(PW_ID_ANY) {
  d->q = this;

  switch (type) {
  case SourceType::DesktopAudio:
    m_displayName = "Desktop Audio";
    break;
  case SourceType::ApplicationAudio:
    m_displayName = "Application Audio";
    break;
  case SourceType::Microphone:
    m_displayName = "Microphone";
    break;
  }
}

PipeWireAudioSource::~PipeWireAudioSource() { stop(); }

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createMonitorSource(const QString &sinkName,
                                        bool deferResolution) {
  auto src = std::make_unique<PipeWireAudioSource>(SourceType::DesktopAudio);
  src->m_identifier = sinkName;
  src->m_displayName = "Desktop Audio";
  src->m_deviceResolutionPending = true;
  if (!deferResolution)
    src->resolveDeviceTarget();
  return src;
}

void PipeWireAudioSource::resolveDeviceTarget() {
  const bool monitor = m_type == SourceType::DesktopAudio;

  // Resolve which sink to monitor: explicit name, or look up the PipeWire
  // default
  QString target = m_identifier;
  if (target.isEmpty() || target == "default") {
    auto [defaultSink, defaultSource] = queryDefaultDevices();
    const QString chosen = monitor ? defaultSink : defaultSource;
    if (!chosen.isEmpty()) {
      target = chosen;
    }
  }

  if (!target.isEmpty() && target != "default") {
    uint32_t sinkNodeId =
        findNodeByName(target, &m_targetNodeName, &m_targetSerial);
    if (sinkNodeId != PW_ID_ANY) {
      m_targetNodeId = sinkNodeId;
      qDebug() << "Monitor source will capture from sink" << target
               << "(node ID:" << sinkNodeId << "serial:" << m_targetSerial
               << ")";
    } else {
      qWarning() << "Could not find sink node:" << target
                 << "— will use PW_ID_ANY";
    }
  }

  m_deviceResolutionPending = false;
}

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createApplicationSource(uint32_t nodeId,
                                              const QString &nodeName,
                                              const QString &serial,
                                              const QString &expectedBinary) {
  auto src =
      std::make_unique<PipeWireAudioSource>(SourceType::ApplicationAudio);
  src->m_targetNodeId = nodeId;
  src->m_targetNodeName = nodeName;
  src->m_targetSerial = serial;
  src->m_expectedBinary = expectedBinary;
  src->m_identifier = nodeName.isEmpty() ? QString::number(nodeId) : nodeName;
  src->m_displayName =
      QString("Application Audio (%1)")
          .arg(nodeName.isEmpty() ? QString::number(nodeId) : nodeName);
  return src;
}

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createInputSource(const QString &deviceName,
                                      bool deferResolution) {
  auto src = std::make_unique<PipeWireAudioSource>(SourceType::Microphone);
  src->m_identifier = deviceName;
  src->m_displayName = deviceName.isEmpty() ? "Microphone" : deviceName;

  src->m_deviceResolutionPending = true;
  if (!deferResolution)
    src->resolveDeviceTarget();
  return src;
}

void PipeWireAudioSource::setCallback(AudioCallback callback) {
  AudioCallbackHandle published;
  if (callback)
    published =
        std::make_shared<AudioCallbackRegistration>(std::move(callback));
  m_callback.store(published, std::memory_order_release);
}

void PipeWireAudioSource::processBuffer(struct spa_buffer *buf, int channels,
                                      int sampleRate) {
  if (!buf || buf->n_datas == 0 || !buf->datas || !buf->datas[0].chunk)
    return;
  const auto &data = buf->datas[0];
  const auto &chunk = *data.chunk;
  // This stream consumes contiguous interleaved F32, not wrapped chunks.
  // Reject malformed bounds or partial frames rather than silently clamping
  // them. Subtraction after the offset check avoids offset+size overflow.
  if (!data.data || channels <= 0 || sampleRate <= 0 || chunk.size == 0 ||
      chunk.offset >= data.maxsize || chunk.size > data.maxsize - chunk.offset ||
      chunk.offset % sizeof(float) != 0 || chunk.size % sizeof(float) != 0 ||
      reinterpret_cast<uintptr_t>(data.data) % alignof(float) != 0)
    return;
  const uint32_t totalSamples = chunk.size / sizeof(float);
  if (totalSamples % static_cast<uint32_t>(channels) != 0)
    return;
  auto *samples = reinterpret_cast<float *>(
      static_cast<unsigned char *>(data.data) + chunk.offset);
  // Retain the registration before invoking; publication may replace it
  // concurrently, but never mutates the retained std::function.
  const auto callback = snapshotCallback();
  if (!callback || !callback->callback)
    return;

  uint32_t frameCount = totalSamples / static_cast<uint32_t>(channels);
  float peak = 0.0f;
  for (uint32_t i = 0; i < totalSamples; ++i) {
    float abs = std::fabs(samples[i]);
    if (abs > peak)
      peak = abs;
  }
  float cur = m_peakLevel.load();
  m_peakLevel.store(peak > cur ? peak : cur * 0.95f);

  float gain = volume();
  if (gain != 1.0f) {
    for (uint32_t i = 0; i < totalSamples; ++i)
      samples[i] *= gain;
  }

  AudioFrame frame;
  frame.samples = samples;
  frame.frameCount = static_cast<int>(frameCount);
  frame.channels = channels;
  frame.sampleRate = sampleRate;
  // spa_chunk::offset is a byte offset, not PTS. No timestamp metadata is
  // consumed here, so leave the timestamp unspecified (zero).
  frame.timestamp = 0;
  callback->callback(frame);
}

PipeWireAudioSource::Operations PipeWireAudioSource::realOperations() {
  return {
      [] { pw_init(nullptr, nullptr); },
      [] { return pw_thread_loop_new("trophy-audio", nullptr); },
      [](pw_thread_loop *loop) {
        return pw_context_new(pw_thread_loop_get_loop(loop), nullptr, 0);
      },
      pw_thread_loop_start, pw_thread_loop_lock, pw_thread_loop_unlock,
      pw_thread_loop_stop, pw_thread_loop_destroy,
      [](pw_context *context) { return pw_context_connect(context, nullptr, 0); },
      pw_core_disconnect, pw_context_destroy,
      [] {
        return pw_properties_new(
            PW_KEY_APP_NAME, "Trophy", PW_KEY_MEDIA_TYPE, "Audio",
            PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);
      },
      pw_properties_set, pw_properties_free, pw_stream_new, pw_stream_add_listener,
      [](pw_stream *stream, uint32_t target, int flags, const spa_pod *param) {
        return pw_stream_connect(stream, PW_DIRECTION_INPUT, target,
                                 static_cast<pw_stream_flags>(flags), &param, 1);
      },
      pw_stream_destroy,
      [](pw_core *core, spa_hook *hook, const pw_core_events *events, void *data) {
        pw_core_add_listener(core, hook, events, data);
      },
      pw_stream_dequeue_buffer, pw_stream_queue_buffer, pw_stream_get_properties,
      [](PipeWireAudioSource *source) {
        source->d->appGroup = std::make_unique<AppAudioGroup>(
             source->d->core, source->d->stream, source->m_targetNodeId,
            source->m_targetSerial, source->m_expectedBinary);
      }};
}

bool PipeWireAudioSource::initialize(const AudioConfig &config) {
  std::unique_lock lifecycleLock(m_lifecycleMutex);
  if (config.channels <= 0 || config.sampleRate <= 0) {
    qWarning() << "Refusing PipeWire audio source with invalid config:"
               << "channels" << config.channels << "sampleRate"
               << config.sampleRate;
    return false;
  }
  // A valid reinitialization replaces, rather than overwrites, owned resources.
  // Invalid input above leaves an existing capture untouched.
  const bool wasCapturing = stopLocked();
  if (m_deviceResolutionPending)
    resolveDeviceTarget();
  const auto finish = [&](bool result) {
    lifecycleLock.unlock();
    if (wasCapturing)
      emit captureStopped();
    return result;
  };
  m_config = config;
  d->failed.store(false, std::memory_order_release);
  d->channels.store(config.channels, std::memory_order_relaxed);
  d->sampleRate.store(config.sampleRate, std::memory_order_relaxed);

  m_operations.init();

  d->loop = m_operations.newLoop();
  if (!d->loop) {
    qCritical() << "Failed to create PipeWire thread loop";
    return finish(false);
  }

  d->context = m_operations.newContext(d->loop);
  if (!d->context) {
    qCritical() << "Failed to create PipeWire context";
    stopLocked();
    return finish(false);
  }

  qDebug() << "PipeWire audio source initialized:" << m_displayName;
  return finish(true);
}

bool PipeWireAudioSource::start() {
  std::unique_lock lifecycleLock(m_lifecycleMutex);
  if (m_capturing || d->failed.load(std::memory_order_acquire) ||
      !d->loop || !d->context)
    return false;

  // Start the PipeWire event loop thread
  if (m_operations.startLoop(d->loop) < 0) {
    qCritical() << "Failed to start PipeWire thread loop";
    stopLocked();
    return false;
  }
  d->loopStarted = true;

  m_operations.lockLoop(d->loop);
  d->loopLocked = true;

  d->core = m_operations.connectCore(d->context);
  if (!d->core) {
    stopLocked();
    qCritical() << "Failed to connect to PipeWire";
    return false;
  }
  static const pw_core_events coreEvents = [] {
    pw_core_events events{};
    events.version = PW_VERSION_CORE_EVENTS;
    events.error = Private::onCoreError;
    return events;
  }();
  memset(&d->core_listener, 0, sizeof(d->core_listener));
  m_operations.addCoreListener(d->core, &d->core_listener, &coreEvents, d.get());

  // Build stream properties
  struct pw_properties *props = m_operations.newProperties();
  if (!props) {
    qCritical() << "Failed to create PipeWire stream properties";
    stopLocked();
    return false;
  }
  bool propertiesOk = true;
  const auto setProperty = [&](const char *key, const char *value) {
    if (propertiesOk && m_operations.setProperty(props, key, value) < 0)
      propertiesOk = false;
  };

  // Desktop audio: capture from the sink monitor (all audio going to speakers)
  if (m_type == SourceType::DesktopAudio) {
    setProperty(PW_KEY_STREAM_CAPTURE_SINK, "true");
  }

  // Application audio: capture from a specific application output stream.
  // Microphone with specific target: capture from a specific input node.
  //
  // For both cases we must pin WirePlumber's routing via target.object /
  // node.target properties AND PW_STREAM_FLAG_DONT_RECONNECT.  Without these,
  // WirePlumber sees a plain AUTOCONNECT stream and routes it to whatever it
  // considers the "default" source — ignoring the node ID we pass to
  // pw_stream_connect.
  //
  // target.object and node.target are both set for compatibility:
  //  - node.target: WirePlumber ≤0.4, accepts node name or numeric ID
  //  - target.object: WirePlumber 0.5+, accepts object serial or node name
  if (m_type == SourceType::ApplicationAudio && m_targetNodeId != PW_ID_ANY) {
    // ApplicationAudio captures the OUTPUT side of the target app's stream
    setProperty(PW_KEY_STREAM_CAPTURE_SINK, "true");
    setProperty("node.dont-fallback", "true");
    setProperty("node.dont-move", "true");

    // Serial first (unambiguous), then name — never a bare numeric ID:
    // WirePlumber reads target.object as a serial and IDs collide with
    // serials (easyeffects id/serial skew captured the wrong node live).
    const QString bestTarget =
        preferredTarget(m_targetSerial, m_targetNodeName, m_targetNodeId);
    const QByteArray ba = bestTarget.toUtf8();
    setProperty("target.object", ba.constData());

    const QString legacyTarget =
        !m_targetNodeName.isEmpty()
            ? m_targetNodeName
            : preferredTarget(m_targetSerial, m_targetNodeName, m_targetNodeId);
    const QByteArray lba = legacyTarget.toUtf8();
    setProperty("node.target", lba.constData());

    qDebug() << "Application audio target — node.target:" << legacyTarget
             << "target.object:" << bestTarget;

  } else if (m_type == SourceType::Microphone && m_targetNodeId != PW_ID_ANY) {
    // Microphone with an explicit device: pin WirePlumber to that node so it
    // cannot reroute to the default input (e.g., EasyEffects source).
    const QString bestTarget =
        preferredTarget(m_targetSerial, m_targetNodeName, m_targetNodeId);
    const QByteArray ba = bestTarget.toUtf8();
    setProperty("target.object", ba.constData());

    const QString legacyTarget =
        !m_targetNodeName.isEmpty()
            ? m_targetNodeName
            : preferredTarget(m_targetSerial, m_targetNodeName, m_targetNodeId);
    setProperty("node.target", legacyTarget.toUtf8().constData());

    qDebug() << "Microphone target pinned — target.object:" << bestTarget;
  }

  const QByteArray streamNameBa = m_streamName.toUtf8();
  if (!propertiesOk) {
    m_operations.freeProperties(props);
    qCritical() << "Failed to set PipeWire stream properties";
    stopLocked();
    return false;
  }
  // pw_stream_new consumes props even on failure.
  d->stream = m_operations.newStream(d->core, streamNameBa.constData(), props);
  if (!d->stream) {
    stopLocked();
    qCritical() << "Failed to create PipeWire stream";
    return false;
  }

  // Register stream events
  static const struct pw_stream_events stream_events = {
      PW_VERSION_STREAM_EVENTS,
      nullptr, // destroy
      Private::onStateChanged,
      nullptr, // control_info
      nullptr, // io_changed
      nullptr, // param_changed
      nullptr, // add_buffer
      nullptr, // remove_buffer
      Private::onProcess,
      nullptr, // drained
      nullptr, // command
      nullptr, // trigger_done
  };

  memset(&d->stream_listener, 0, sizeof(d->stream_listener));
  m_operations.addListener(d->stream, &d->stream_listener, &stream_events,
                           d.get());

  // Build audio format
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod *params[1];

  struct spa_audio_info_raw audio_info = {};
  audio_info.format = SPA_AUDIO_FORMAT_F32;
  audio_info.channels =
      static_cast<uint32_t>(d->channels.load(std::memory_order_relaxed));
  audio_info.rate =
      static_cast<uint32_t>(d->sampleRate.load(std::memory_order_relaxed));
  // Do not specify channel positions — leaving them as
  // SPA_AUDIO_CHANNEL_UNKNOWN (zero) lets PipeWire's format adapter upmix mono
  // sources to stereo and handle any other channel-count mismatch
  // automatically.

  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

  // Connect the stream
  qDebug() << "Connecting PipeWire stream" << m_displayName << "to node"
           << (m_targetNodeId == PW_ID_ANY ? "PW_ID_ANY (auto)"
                                           : QString::number(m_targetNodeId));

  // For application audio: use DONT_RECONNECT so WirePlumber can't override
  // our target.object and re-route to the default source (mic).
  // Also: if no valid target is specified, don't use AUTOCONNECT to prevent
  // connecting to a random node (like the default).
  enum pw_stream_flags flags =
      static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_MAP_BUFFERS);

  if (m_type == SourceType::ApplicationAudio) {
    if (m_targetNodeId != PW_ID_ANY) {
      // Valid target: AUTOCONNECT + DONT_RECONNECT pins WirePlumber to our node
      flags = static_cast<enum pw_stream_flags>(
          flags | PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_DONT_RECONNECT);
    }
    // If no valid target (PW_ID_ANY), no AUTOCONNECT — stream stays
    // disconnected (silent)
  } else if (m_type == SourceType::Microphone && m_targetNodeId != PW_ID_ANY) {
    // Specific mic selected: pin to that node, prevent WirePlumber from
    // rerouting to the default input (e.g. EasyEffects source)
    flags = static_cast<enum pw_stream_flags>(
        flags | PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_DONT_RECONNECT);
  } else {
    // Desktop audio or microphone with no explicit target: let WirePlumber
    // choose
    flags =
        static_cast<enum pw_stream_flags>(flags | PW_STREAM_FLAG_AUTOCONNECT);
  }

  int ret =
      m_operations.connectStream(d->stream, m_targetNodeId, flags, params[0]);

  if (ret < 0 || d->failed.load(std::memory_order_acquire)) {
    m_operations.unlockLoop(d->loop);
    d->loopLocked = false;
    if (ret < 0)
      qCritical() << "Failed to connect PipeWire stream:" << strerror(-ret);
    else
      qCritical() << "PipeWire listener reported failure during stream setup";
    stopLocked();
    return false;
  }

  if (m_type == SourceType::ApplicationAudio && m_targetNodeId != PW_ID_ANY &&
      m_operations.attachAppGroup)
    m_operations.attachAppGroup(this);

  // Publish before releasing the loop: an immediately dispatched error must
  // not be overwritten by start setting capturing back to true.
  m_capturing = true;
  m_operations.unlockLoop(d->loop);
  d->loopLocked = false;
  lifecycleLock.unlock();
  emit captureStarted();
  qDebug() << "PipeWire audio capture started:" << m_displayName;
  return true;
}

void PipeWireAudioSource::stop() {
  bool wasCapturing;
  {
    std::lock_guard lifecycleLock(m_lifecycleMutex);
    wasCapturing = stopLocked();
  }
  if (wasCapturing)
    emit captureStopped();
}

bool PipeWireAudioSource::stopLocked() {
  const bool wasCapturing = m_capturing.exchange(false);
  if (!d->loop)
    return wasCapturing;

  if (d->loop) {
    if (d->loopStarted && !d->loopLocked) {
      m_operations.lockLoop(d->loop);
      d->loopLocked = true;
    }

    d->appGroup.reset();
    if (d->stream) {
      m_operations.destroyStream(d->stream);
      d->stream = nullptr;
    }
    if (d->core) {
      m_operations.disconnectCore(d->core);
      d->core = nullptr;
    }

    if (d->loopLocked) {
      m_operations.unlockLoop(d->loop);
      d->loopLocked = false;
    }
    if (d->loopStarted) {
      m_operations.stopLoop(d->loop);
      d->loopStarted = false;
    }

    if (d->context) {
      m_operations.destroyContext(d->context);
      d->context = nullptr;
    }

    m_operations.destroyLoop(d->loop);
    d->loop = nullptr;
  }

  qDebug() << "PipeWire audio capture stopped:" << m_displayName;
  return wasCapturing;
}

bool PipeWireAudioSource::isCapturing() const { return m_capturing; }

} // namespace TrophyBackend
