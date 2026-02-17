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

#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#include <spa/param/audio/raw.h>

namespace GameRecorder {

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
      uint32_t totalSamples =
          buf->datas[0].chunk->size / sizeof(float);
      uint32_t frameCount = totalSamples / static_cast<uint32_t>(self->channels);

      // Update peak level
      float peak = 0.0f;
      for (uint32_t i = 0; i < totalSamples; ++i) {
        float abs = std::fabs(samples[i]);
        if (abs > peak)
          peak = abs;
      }
      float cur = self->q->m_peakLevel.load();
      self->q->m_peakLevel.store(peak > cur ? peak : cur * 0.95f);

      AudioFrame frame;
      frame.samples = samples;
      frame.frameCount = static_cast<int>(frameCount);
      frame.channels = self->channels;
      frame.sampleRate = self->sampleRate;
      frame.timestamp =
          b->buffer->datas[0].chunk->offset; // best-effort PTS

      self->q->m_callback(frame);
    }

    pw_stream_queue_buffer(self->stream, b);
  }

  static void onStateChanged(void *userdata, enum pw_stream_state /*old*/,
                             enum pw_stream_state state, const char *error) {
    auto *self = static_cast<Private *>(userdata);
    qInfo() << "PipeWire stream state:" << pw_stream_state_as_string(state);
    if (state == PW_STREAM_STATE_ERROR) {
      qWarning() << "PipeWire stream error:" << (error ? error : "unknown");
      emit self->q->captureError(QString(error ? error : "PipeWire error"));
    }
  }
};

// ---------------------------------------------------------------------------
// Node enumeration helper (one-shot, synchronous)
// ---------------------------------------------------------------------------

namespace {

struct NodeEnumData {
  QString processName;
  QString nodeName;  // For sink node lookup
  uint32_t nodeId = PW_ID_ANY;
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

  // If searching by node name (e.g., "easyeffects_sink" for sink monitor)
  if (!data->nodeName.isEmpty() && pwNodeName) {
    if (QString(pwNodeName) == data->nodeName) {
      qInfo() << "Found PipeWire sink node:" << data->nodeName << "-> ID:" << id;
      data->nodeId = id;
    }
    return;
  }

  // Otherwise search by process name
  QString target = data->processName.toLower();

  auto matches = [&](const char *s) {
    return s && QString(s).toLower().contains(target);
  };

  if (matches(binary) || matches(appName) || matches(pwNodeName)) {
    qInfo() << "Found PipeWire node for" << data->processName << "-> ID:" << id
            << "binary:" << (binary ? binary : "?");
    data->nodeId = id;
  }
}

static void enumCoreDone(void *userdata, uint32_t id, int seq) {
  auto *data = static_cast<NodeEnumData *>(userdata);
  if (id == PW_ID_CORE && seq == data->pendingSync)
    pw_main_loop_quit(data->loop);
}

/**
 * @brief Find a PipeWire sink node by name
 * @param sinkName The sink node name (e.g., "easyeffects_sink")
 * @return PipeWire node ID, or PW_ID_ANY if not found
 */
static uint32_t findSinkByName(const QString &sinkName) {
  if (sinkName.isEmpty())
    return PW_ID_ANY;

  pw_init(nullptr, nullptr);

  NodeEnumData data;
  data.nodeName = sinkName;
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

  static const struct pw_core_events core_events = {
      .version = PW_VERSION_CORE_EVENTS,
      .done = enumCoreDone,
  };
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

  static const struct pw_registry_events registry_events = {
      PW_VERSION_REGISTRY_EVENTS,
      enumRegistryGlobal,
      nullptr,
  };
  pw_registry_add_listener(registry, &data.registryListener, &registry_events,
                           &data);

  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);
  pw_main_loop_run(data.loop);

  uint32_t result = data.nodeId;

  pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(registry));
  pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(data.loop);

  return result;
}

} // namespace

uint32_t PipeWireAudioSource::findNodeByProcessName(const QString &processName) {
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

  // Core listener to detect sync completion
  static const struct pw_core_events core_events = {
      .version = PW_VERSION_CORE_EVENTS,
      .done = enumCoreDone,
  };
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  // Registry listener
  struct pw_registry *registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);

  static const struct pw_registry_events registry_events = {
      PW_VERSION_REGISTRY_EVENTS,
      enumRegistryGlobal,
      nullptr, // global_remove
  };
  pw_registry_add_listener(registry, &data.registryListener, &registry_events,
                           &data);

  // Trigger sync so we know when the initial dump is done
  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);

  // Run until sync completes (or short timeout via signal in production)
  pw_main_loop_run(data.loop);

  uint32_t result = data.nodeId;

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

  // Resolve sink name to node ID for actual PipeWire connection
  if (!sinkName.isEmpty() && sinkName != "default") {
    uint32_t sinkNodeId = findSinkByName(sinkName);
    if (sinkNodeId != PW_ID_ANY) {
      src->m_targetNodeId = sinkNodeId;
      qInfo() << "Monitor source will capture from sink" << sinkName
              << "(node ID:" << sinkNodeId << ")";
    } else {
      qWarning() << "Could not find sink node:" << sinkName
                 << "— will use default";
    }
  }

  return src;
}

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createApplicationSource(uint32_t nodeId) {
  auto src =
      std::make_unique<PipeWireAudioSource>(SourceType::ApplicationAudio);
  src->m_targetNodeId = nodeId;
  src->m_identifier = QString::number(nodeId);
  return src;
}

std::unique_ptr<PipeWireAudioSource>
PipeWireAudioSource::createInputSource(const QString &deviceName) {
  auto src = std::make_unique<PipeWireAudioSource>(SourceType::Microphone);
  src->m_identifier = deviceName;
  src->m_displayName = deviceName.isEmpty() ? "Microphone" : deviceName;
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

  qInfo() << "PipeWire audio source initialized:" << m_displayName;
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
      PW_KEY_APP_NAME, "Trophy",
      PW_KEY_MEDIA_TYPE, "Audio",
      PW_KEY_MEDIA_CATEGORY, "Capture",
      PW_KEY_MEDIA_ROLE, "Screen",
      nullptr);

  // For desktop audio: capture from the sink monitor (all playing audio)
  if (m_type == SourceType::DesktopAudio) {
    pw_properties_set(props, PW_KEY_STREAM_CAPTURE_SINK, "true");
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
  pw_stream_add_listener(d->stream, &d->stream_listener, &stream_events, d.get());

  // Build audio format
  uint8_t buffer[1024];
  struct spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof(buffer));
  const struct spa_pod *params[1];

  struct spa_audio_info_raw audio_info = {};
  audio_info.format = SPA_AUDIO_FORMAT_F32;
  audio_info.channels = static_cast<uint32_t>(d->channels);
  audio_info.rate = static_cast<uint32_t>(d->sampleRate);
  audio_info.position[0] = SPA_AUDIO_CHANNEL_FL;
  audio_info.position[1] = SPA_AUDIO_CHANNEL_FR;

  params[0] = spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &audio_info);

  // Connect the stream
  int ret = pw_stream_connect(
      d->stream,
      PW_DIRECTION_INPUT,
      m_targetNodeId, // PW_ID_ANY for default, specific ID for app capture
      static_cast<enum pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT |
                                        PW_STREAM_FLAG_MAP_BUFFERS),
      params, 1);

  pw_thread_loop_unlock(d->loop);

  if (ret < 0) {
    qCritical() << "Failed to connect PipeWire stream:" << strerror(-ret);
    stop();
    return false;
  }

  m_capturing = true;
  emit captureStarted();
  qInfo() << "PipeWire audio capture started:" << m_displayName;
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
  qInfo() << "PipeWire audio capture stopped:" << m_displayName;
}

bool PipeWireAudioSource::isCapturing() const { return m_capturing; }

} // namespace GameRecorder
