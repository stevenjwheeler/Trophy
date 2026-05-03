/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "PipeWireAudioSource.h"

#include <QDebug>
#include <cmath>
#include <cstring>

#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <qdebug.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

namespace TrophyBackend {

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

  int channels = 2;
  int sampleRate = 48000;

  // --- stream callbacks ---

  static void onProcess(void *userdata) {
    auto *self = static_cast<Private *>(userdata);

    struct pw_buffer *b = pw_stream_dequeue_buffer(self->stream);
    if (!b)
      return;

    struct spa_buffer *buf = b->buffer;
    auto *samples = static_cast<float *>(buf->datas[0].data);

    if (samples && buf->datas[0].chunk->size > 0 && self->q->m_callback) {
      uint32_t totalSamples = buf->datas[0].chunk->size / sizeof(float);
      uint32_t frameCount =
          totalSamples / static_cast<uint32_t>(self->channels);

      // Update peak level
      float peak = 0.0f;
      for (uint32_t i = 0; i < totalSamples; ++i) {
        float abs = std::fabs(samples[i]);
        if (abs > peak)
          peak = abs;
      }
      float cur = self->q->m_peakLevel.load();
      self->q->m_peakLevel.store(peak > cur ? peak : cur * 0.95f);

      // Apply volume (gain)
      float volume = self->q->volume();
      if (volume != 1.0f) {
        for (uint32_t i = 0; i < totalSamples; ++i) {
          samples[i] *= volume;
        }
      }

      AudioFrame frame;
      frame.samples = samples;
      frame.frameCount = static_cast<int>(frameCount);
      frame.channels = self->channels;
      frame.sampleRate = self->sampleRate;
      frame.timestamp = b->buffer->datas[0].chunk->offset; // best-effort PTS

      self->q->m_callback(frame);
    }

    pw_stream_queue_buffer(self->stream, b);
  }

  static void onStateChanged(void *userdata, enum pw_stream_state /*old*/,
                             enum pw_stream_state state, const char *error) {
    auto *self = static_cast<Private *>(userdata);
    qDebug() << "PipeWire stream state:" << pw_stream_state_as_string(state)
             << "[" << self->q->m_displayName << "]";

    if (state == PW_STREAM_STATE_ERROR) {
      qWarning() << "PipeWire stream error:" << (error ? error : "unknown");
      emit self->q->captureError(QString(error ? error : "PipeWire error"));
    }

    if (state == PW_STREAM_STATE_STREAMING) {
      // Read back the properties WirePlumber set on the stream after routing.
      // If our requested target.object / node.target was overridden we'll see
      // it here.
      const struct pw_properties *props =
          pw_stream_get_properties(self->stream);
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
          qDebug() << "Routing confirmed [" << self->q->m_displayName << "]"
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

struct NodeEnumData {
  QString processName;
  QString nodeName; // For sink node lookup by exact name
  uint32_t nodeId = PW_ID_ANY;
  QString foundNodeName;  // PW_KEY_NODE_NAME of the matched app node
  QString foundSerial;    // object.serial of the matched app node
  uint32_t targetPid = 0; // Search by process ID instead of name
  int pendingSync = 0;
  struct pw_main_loop *loop = nullptr;
  struct spa_hook registryListener = {};
  struct spa_hook coreListener = {};
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
      return;
    }
    // Otherwise check by string name
    if (pwNodeName && QString(pwNodeName) == data->nodeName) {
      qDebug() << "Found PipeWire node:" << data->nodeName << "-> ID:" << id;
      data->nodeId = id;
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

static void enumCoreDone(void *userdata, uint32_t id, int seq) {
  auto *data = static_cast<NodeEnumData *>(userdata);
  if (id == PW_ID_CORE && seq == data->pendingSync)
    pw_main_loop_quit(data->loop);
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
    return e;
  }();
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  struct pw_registry *registry =
      pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

  static const struct pw_registry_events registry_events = [] {
    struct pw_registry_events e = {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = enumRegistryGlobal;
    return e;
  }();
  pw_registry_add_listener(registry, &data.registryListener, &registry_events,
                           &data);

  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
  pw_main_loop_run(data.loop);

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
    pw_main_loop_quit(data->loop);
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
  pw_main_loop_run(data.loop);

  // Second sync: flush metadata property events that arrive after binding
  if (data.gotMetadata) {
    data.pendingSync = pw_core_sync(data.core, PW_ID_CORE, 0);
    pw_main_loop_run(data.loop);
  }

  auto result = std::make_pair(data.sinkName, data.sourceName);

  if (data.metadata)
    pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(data.metadata));
  pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(data.registry));
  pw_core_disconnect(data.core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(data.loop);

  return result;
}

} // namespace

uint32_t PipeWireAudioSource::findNodeByProcessName(const QString &processName,
                                                    QString *outNodeName,
                                                    QString *outSerial) {
  pw_init(nullptr, nullptr);

  NodeEnumData data;
  data.processName = processName;
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
    return e;
  }();
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  struct pw_registry *registry =
      pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

  static const struct pw_registry_events registry_events = [] {
    struct pw_registry_events e = {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = enumRegistryGlobal;
    return e;
  }();
  pw_registry_add_listener(registry, &data.registryListener, &registry_events,
                           &data);

  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
  pw_main_loop_run(data.loop);

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

uint32_t PipeWireAudioSource::findNodeByPid(uint32_t pid, QString *outNodeName,
                                            QString *outSerial) {
  if (pid == 0)
    return PW_ID_ANY;

  pw_init(nullptr, nullptr);

  NodeEnumData data;
  data.targetPid = pid;
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
    return e;
  }();
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  struct pw_registry *registry =
      pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

  static const struct pw_registry_events registry_events = [] {
    struct pw_registry_events e = {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = enumRegistryGlobal;
    return e;
  }();
  pw_registry_add_listener(registry, &data.registryListener, &registry_events,
                           &data);

  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
  pw_main_loop_run(data.loop);

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

// ---------------------------------------------------------------------------
// Audio app enumeration
// ---------------------------------------------------------------------------

namespace {

struct AudioAppEnumData {
  QVariantList apps;
  struct pw_main_loop *loop = nullptr;
  struct spa_hook registryListener = {};
  struct spa_hook coreListener = {};
  int pendingSync = 0;
};

static void enumAppsRegistryGlobal(void *userdata, uint32_t id,
                                   uint32_t /*permissions*/, const char *type,
                                   uint32_t /*version*/,
                                   const struct spa_dict *props) {
  auto *data = static_cast<AudioAppEnumData *>(userdata);
  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;
  if (!props)
    return;

  // Only enumerate application playback streams
  const char *mediaClass = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
  if (!mediaClass || strcmp(mediaClass, "Stream/Output/Audio") != 0)
    return;

  const char *binary = spa_dict_lookup(props, PW_KEY_APP_PROCESS_BINARY);
  const char *appName = spa_dict_lookup(props, PW_KEY_APP_NAME);
  const char *pwNodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);

  // Build display name: prefer app name, fall back to binary or node name
  QString displayName;
  if (appName && strlen(appName) > 0) {
    displayName = QString::fromUtf8(appName);
  } else if (binary && strlen(binary) > 0) {
    displayName = QString::fromUtf8(binary);
  } else if (pwNodeName) {
    displayName = QString::fromUtf8(pwNodeName);
  }

  if (!displayName.isEmpty()) {
    QVariantMap app;
    app["name"] = displayName;
    app["binary"] = binary ? QString::fromUtf8(binary) : "";
    app["nodeId"] = id;
    data->apps.append(app);
    qDebug() << "Audio app found:" << displayName << "-> ID:" << id;
  }
}

static void enumAppsCoreDone(void *userdata, uint32_t id, int seq) {
  auto *data = static_cast<AudioAppEnumData *>(userdata);
  if (id == PW_ID_CORE && seq == data->pendingSync)
    pw_main_loop_quit(data->loop);
}

} // namespace

QVariantList PipeWireAudioSource::enumerateAudioApps() {
  pw_init(nullptr, nullptr);

  AudioAppEnumData data;
  data.loop = pw_main_loop_new(nullptr);
  if (!data.loop)
    return {};

  struct pw_context *ctx =
      pw_context_new(pw_main_loop_get_loop(data.loop), nullptr, 0);
  if (!ctx) {
    pw_main_loop_destroy(data.loop);
    return {};
  }

  struct pw_core *core = pw_context_connect(ctx, nullptr, 0);
  if (!core) {
    pw_context_destroy(ctx);
    pw_main_loop_destroy(data.loop);
    return {};
  }

  static const struct pw_core_events core_events = [] {
    struct pw_core_events e = {};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = enumAppsCoreDone;
    return e;
  }();
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  struct pw_registry *registry =
      pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

  static const struct pw_registry_events registry_events = [] {
    struct pw_registry_events e = {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global = enumAppsRegistryGlobal;
    return e;
  }();
  pw_registry_add_listener(registry, &data.registryListener, &registry_events,
                           &data);

  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
  pw_main_loop_run(data.loop);

  QVariantList result = data.apps;

  pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(registry));
  pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(data.loop);

  return result;
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
PipeWireAudioSource::createMonitorSource(const QString &sinkName) {
  auto src = std::make_unique<PipeWireAudioSource>(SourceType::DesktopAudio);
  src->m_identifier = sinkName;
  src->m_displayName = "Desktop Audio";

  // Resolve which sink to monitor: explicit name, or look up the PipeWire
  // default
  QString target = sinkName;
  if (target.isEmpty() || target == "default") {
    auto [defaultSink, defaultSource] = queryDefaultDevices();
    if (!defaultSink.isEmpty()) {
      target = defaultSink;
      qDebug() << "Monitor source: resolved PipeWire default sink to" << target;
    }
  }

  if (!target.isEmpty() && target != "default") {
    uint32_t sinkNodeId =
        findNodeByName(target, &src->m_targetNodeName, &src->m_targetSerial);
    if (sinkNodeId != PW_ID_ANY) {
      src->m_targetNodeId = sinkNodeId;
      qDebug() << "Monitor source will capture from sink" << target
               << "(node ID:" << sinkNodeId << "serial:" << src->m_targetSerial
               << ")";
    } else {
      qWarning() << "Could not find sink node:" << target
                 << "— will use PW_ID_ANY";
    }
  }

  return src;
}

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createApplicationSource(uint32_t nodeId,
                                             const QString &nodeName,
                                             const QString &serial) {
  auto src =
      std::make_unique<PipeWireAudioSource>(SourceType::ApplicationAudio);
  src->m_targetNodeId = nodeId;
  src->m_targetNodeName = nodeName;
  src->m_targetSerial = serial;
  src->m_identifier = nodeName.isEmpty() ? QString::number(nodeId) : nodeName;
  src->m_displayName =
      QString("Application Audio (%1)")
          .arg(nodeName.isEmpty() ? QString::number(nodeId) : nodeName);
  return src;
}

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createInputSource(const QString &deviceName) {
  auto src = std::make_unique<PipeWireAudioSource>(SourceType::Microphone);
  src->m_identifier = deviceName;
  src->m_displayName = deviceName.isEmpty() ? "Microphone" : deviceName;

  // Resolve which source to capture: explicit name, or look up the PipeWire
  // default
  QString target = deviceName;
  if (target.isEmpty() || target == "default") {
    auto [defaultSink, defaultSource] = queryDefaultDevices();
    if (!defaultSource.isEmpty()) {
      target = defaultSource;
      qDebug() << "Input source: resolved PipeWire default source to" << target;
    }
  }

  if (!target.isEmpty() && target != "default") {
    uint32_t nodeId =
        findNodeByName(target, &src->m_targetNodeName, &src->m_targetSerial);
    if (nodeId != PW_ID_ANY) {
      src->m_targetNodeId = nodeId;
      qDebug() << "Input source will capture from node" << target
               << "(node ID:" << nodeId << "serial:" << src->m_targetSerial
               << ")";
    } else {
      qWarning() << "Could not find input node:" << target
                 << "— will use PW_ID_ANY";
    }
  }

  return src;
}

void PipeWireAudioSource::setCallback(AudioCallback callback) {
  m_callback = std::move(callback);
}

bool PipeWireAudioSource::initialize(const AudioConfig &config) {
  m_config = config;
  d->channels = config.channels;
  d->sampleRate = config.sampleRate;

  pw_init(nullptr, nullptr);

  d->loop = pw_thread_loop_new("trophy-audio", nullptr);
  if (!d->loop) {
    qCritical() << "Failed to create PipeWire thread loop";
    return false;
  }

  d->context = pw_context_new(pw_thread_loop_get_loop(d->loop), nullptr, 0);
  if (!d->context) {
    qCritical() << "Failed to create PipeWire context";
    pw_thread_loop_destroy(d->loop);
    d->loop = nullptr;
    return false;
  }

  qDebug() << "PipeWire audio source initialized:" << m_displayName;
  return true;
}

bool PipeWireAudioSource::start() {
  if (m_capturing || !d->loop || !d->context)
    return false;

  // Start the PipeWire event loop thread
  if (pw_thread_loop_start(d->loop) < 0) {
    qCritical() << "Failed to start PipeWire thread loop";
    return false;
  }

  pw_thread_loop_lock(d->loop);

  d->core = pw_context_connect(d->context, nullptr, 0);
  if (!d->core) {
    pw_thread_loop_unlock(d->loop);
    pw_thread_loop_stop(d->loop);
    qCritical() << "Failed to connect to PipeWire";
    return false;
  }

  // Build stream properties
  struct pw_properties *props = pw_properties_new(
      PW_KEY_APP_NAME, "Trophy", PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture", PW_KEY_MEDIA_ROLE, "Screen", nullptr);

  // Desktop audio: capture from the sink monitor (all audio going to speakers)
  if (m_type == SourceType::DesktopAudio) {
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
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
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");

    // Prefer serial (WirePlumber 0.5), then name, then numeric ID as fallback
    const QString &bestTarget = !m_targetSerial.isEmpty() ? m_targetSerial
                                : !m_targetNodeName.isEmpty()
                                    ? m_targetNodeName
                                    : QString::number(m_targetNodeId);
    const QByteArray ba = bestTarget.toUtf8();
    pw_properties_set(props, "target.object", ba.constData());

    const QString &legacyTarget = !m_targetNodeName.isEmpty()
                                      ? m_targetNodeName
                                      : QString::number(m_targetNodeId);
    const QByteArray lba = legacyTarget.toUtf8();
    pw_properties_set(props, "node.target", lba.constData());

    qDebug() << "Application audio target — node.target:" << legacyTarget
            << "target.object:" << bestTarget;

  } else if (m_type == SourceType::Microphone && m_targetNodeId != PW_ID_ANY) {
    // Microphone with an explicit device: pin WirePlumber to that node so it
    // cannot reroute to the default input (e.g., EasyEffects source).
    const QString &bestTarget = !m_targetSerial.isEmpty() ? m_targetSerial
                                : !m_targetNodeName.isEmpty()
                                    ? m_targetNodeName
                                    : QString::number(m_targetNodeId);
    const QByteArray ba = bestTarget.toUtf8();
    pw_properties_set(props, "target.object", ba.constData());

    const QString &legacyTarget = !m_targetNodeName.isEmpty()
                                      ? m_targetNodeName
                                      : QString::number(m_targetNodeId);
    pw_properties_set(props, "node.target", legacyTarget.toUtf8().constData());

    qDebug() << "Microphone target pinned — target.object:" << bestTarget;
  }

  d->stream = pw_stream_new(d->core, "trophy-capture", props);
  if (!d->stream) {
    pw_core_disconnect(d->core);
    d->core = nullptr;
    pw_thread_loop_unlock(d->loop);
    pw_thread_loop_stop(d->loop);
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
  pw_stream_add_listener(d->stream, &d->stream_listener, &stream_events,
                         d.get());

  // Build audio format
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod *params[1];

  struct spa_audio_info_raw audio_info = {};
  audio_info.format = SPA_AUDIO_FORMAT_F32;
  audio_info.channels = static_cast<uint32_t>(d->channels);
  audio_info.rate = static_cast<uint32_t>(d->sampleRate);
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

  int ret = pw_stream_connect(d->stream, PW_DIRECTION_INPUT, m_targetNodeId,
                              flags, params, 1);

  pw_thread_loop_unlock(d->loop);

  if (ret < 0) {
    qCritical() << "Failed to connect PipeWire stream:" << strerror(-ret);
    stop();
    return false;
  }

  m_capturing = true;
  emit captureStarted();
  qDebug() << "PipeWire audio capture started:" << m_displayName;
  return true;
}

void PipeWireAudioSource::stop() {
  if (!m_capturing && !d->loop)
    return;

  m_capturing = false;

  if (d->loop) {
    pw_thread_loop_lock(d->loop);

    if (d->stream) {
      pw_stream_destroy(d->stream);
      d->stream = nullptr;
    }
    if (d->core) {
      pw_core_disconnect(d->core);
      d->core = nullptr;
    }

    pw_thread_loop_unlock(d->loop);
    pw_thread_loop_stop(d->loop);

    pw_context_destroy(d->context);
    d->context = nullptr;

    pw_thread_loop_destroy(d->loop);
    d->loop = nullptr;
  }

  emit captureStopped();
  qDebug() << "PipeWire audio capture stopped:" << m_displayName;
}

bool PipeWireAudioSource::isCapturing() const { return m_capturing; }

} // namespace TrophyBackend
