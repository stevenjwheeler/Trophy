/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "AudioDeviceEnumerator.h"
#include "PipeWireAudioSource.h"
#include <QDebug>

#ifdef HAVE_PIPEWIRE
#include <pipewire/pipewire.h>
#include <spa/param/audio/format-utils.h>
#endif

namespace TrophyBackend {

AudioDeviceEnumerator::AudioDeviceEnumerator(QObject *parent)
    : QObject(parent) {
  refresh();
}

AudioDeviceEnumerator::~AudioDeviceEnumerator() = default;

void AudioDeviceEnumerator::refresh() {
  m_inputDevices.clear();
  m_outputDevices.clear();

  enumeratePipeWire();

  // Add default devices if none found
  if (m_inputDevices.isEmpty()) {
    AudioDeviceInfo defaultInput;
    defaultInput.id = "default";
    defaultInput.name = "Default Microphone";
    defaultInput.description = "System default input device";
    defaultInput.isInput = true;
    defaultInput.isDefault = true;
    m_inputDevices.append(defaultInput);
  }

  if (m_outputDevices.isEmpty()) {
    AudioDeviceInfo defaultOutput;
    defaultOutput.id = "default";
    defaultOutput.name = "Default Output";
    defaultOutput.description = "System default output device";
    defaultOutput.isInput = false;
    defaultOutput.isDefault = true;
    m_outputDevices.append(defaultOutput);
  }

  emit devicesChanged();
}

void AudioDeviceEnumerator::refreshAudioApps() {
  m_audioApps = PipeWireAudioSource::enumerateAudioApps();
  emit audioAppsChanged();
  qInfo() << "Audio apps enumeration: found" << m_audioApps.size() << "apps";
}

QString AudioDeviceEnumerator::defaultInputDevice() const {
  for (const auto &device : m_inputDevices) {
    if (device.isDefault) {
      return device.id;
    }
  }
  return m_inputDevices.isEmpty() ? "default" : m_inputDevices.first().id;
}

QString AudioDeviceEnumerator::defaultOutputDevice() const {
  for (const auto &device : m_outputDevices) {
    if (device.isDefault) {
      return device.id;
    }
  }
  return m_outputDevices.isEmpty() ? "default" : m_outputDevices.first().id;
}

namespace {

#ifdef HAVE_PIPEWIRE

struct DeviceEnumData {
  QList<AudioDeviceInfo> *inputs = nullptr;
  QList<AudioDeviceInfo> *outputs = nullptr;
  int pendingSync = 0;
  struct pw_main_loop *loop = nullptr;
  struct spa_hook registryListener = {};
  struct spa_hook coreListener = {};
};

static void enumRegistryGlobal(void *userdata, uint32_t id,
                               uint32_t /*permissions*/, const char *type,
                               uint32_t /*version*/,
                               const struct spa_dict *props) {
  auto *data = static_cast<DeviceEnumData *>(userdata);
  if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0)
    return;
  if (!props)
    return;

  const char *mediaClass = spa_dict_lookup(props, "media.class");
  const char *nodeName = spa_dict_lookup(props, PW_KEY_NODE_NAME);
  const char *nodeDesc = spa_dict_lookup(props, PW_KEY_NODE_DESCRIPTION);
  const char *appName = spa_dict_lookup(props, PW_KEY_APP_NAME);
  const char *nodeId = spa_dict_lookup(props, "node.id");

  // Skip nodes that are clearly not audio devices
  if (mediaClass) {
    // Skip video and other non-audio
    if (strstr(mediaClass, "Video") != nullptr ||
        strstr(mediaClass, "Midi") != nullptr)
      return;
    // Only include Audio/Source and Audio/Sink (actual devices)
    if (strstr(mediaClass, "Audio/Source") == nullptr &&
        strstr(mediaClass, "Audio/Sink") == nullptr)
      return;
  } else {
    // No media.class - only include if it looks like a real device or known virtual audio device
    if (!nodeName)
      return;

    QString nameStr = QString::fromUtf8(nodeName).toLower();

    // Include real device patterns
    bool isRealDevice = nameStr.startsWith("alsa_") || nameStr.startsWith("bluez_") ||
                        nameStr.startsWith("jack_") || nameStr.contains("usb-") ||
                        nameStr.startsWith("hw:");

    // Include known virtual audio device patterns
    bool isVirtualAudio = nameStr.contains("easyeffects") || nameStr.contains("obs") ||
                          nameStr.contains("loopback") || nameStr.contains("virtual") ||
                          nameStr.contains("cable");

    // Exclude obvious application nodes
    bool isApplication =
        nameStr == "firefox" || nameStr == "chromium" || nameStr == "chrome" ||
        nameStr.contains("webrtc") || nameStr.contains("meter") || nameStr.contains("probe") ||
        nameStr.contains("plugin") || nodeId == nullptr;

    if (!isRealDevice && !isVirtualAudio)
      return;
    if (isApplication)
      return;
  }

  // Determine if this is an input or output device
  bool isInput = false;
  bool isOutput = false;

  if (mediaClass) {
    if (strstr(mediaClass, "Source") != nullptr) {
      isInput = true;
    } else if (strstr(mediaClass, "Sink") != nullptr) {
      isOutput = true;
    }
  } else if (nodeName) {
    QString nameStr = QString::fromUtf8(nodeName).toLower();
    if (nameStr.contains("source") || nameStr.contains("input") ||
        nameStr.contains("mic") || nameStr.contains("capture")) {
      isInput = true;
    } else if (nameStr.contains("sink") || nameStr.contains("output") ||
               nameStr.contains("speaker") || nameStr.contains("playback")) {
      isOutput = true;
    }
  }

  // Only add if we determined it's audio
  if (!isInput && !isOutput)
    return;

  // Get device name
  const char *name = nodeDesc ? nodeDesc : nodeName;
  if (!name)
    name = appName;
  if (!name)
    name = "Unknown Device";

  const char *objectPath = spa_dict_lookup(props, "object.path");

  AudioDeviceInfo device;
  device.id = nodeName ? QString::fromUtf8(nodeName) : QString::number(id);
  device.name = QString::fromUtf8(name);
  device.description = device.name;
  device.isDefault = objectPath && QString(objectPath).contains("default");
  device.isInput = isInput;

  if (isInput) {
    data->inputs->append(device);
  } else {
    data->outputs->append(device);
  }
}

static void enumCoreDone(void *userdata, uint32_t id, int seq) {
  auto *data = static_cast<DeviceEnumData *>(userdata);
  if (id == PW_ID_CORE && seq == data->pendingSync)
    pw_main_loop_quit(data->loop);
}

#endif

} // namespace

void AudioDeviceEnumerator::enumeratePipeWire() {
#ifdef HAVE_PIPEWIRE
  pw_init(nullptr, nullptr);

  DeviceEnumData data;
  data.inputs = &m_inputDevices;
  data.outputs = &m_outputDevices;

  data.loop = pw_main_loop_new(nullptr);
  if (!data.loop)
    return;

  struct pw_context *ctx =
      pw_context_new(pw_main_loop_get_loop(data.loop), nullptr, 0);
  if (!ctx) {
    pw_main_loop_destroy(data.loop);
    return;
  }

  struct pw_core *core = pw_context_connect(ctx, nullptr, 0);
  if (!core) {
    pw_context_destroy(ctx);
    pw_main_loop_destroy(data.loop);
    return;
  }

  // Core listener
  static const struct pw_core_events core_events = [] {
    struct pw_core_events e = {};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done = enumCoreDone;
    return e;
  }();
  pw_core_add_listener(core, &data.coreListener, &core_events, &data);

  // Registry listener
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

  // Trigger sync
  data.pendingSync = pw_core_sync(core, PW_ID_CORE, 0);

  // Run event loop until sync completes
  pw_main_loop_run(data.loop);

  // Prepend "Default" entries
  AudioDeviceInfo defaultInput;
  defaultInput.id = "default";
  defaultInput.name = "Default Microphone";
  defaultInput.description = "System default input device";
  defaultInput.isInput = true;
  defaultInput.isDefault = true;
  m_inputDevices.prepend(defaultInput);

  AudioDeviceInfo defaultOutput;
  defaultOutput.id = "default";
  defaultOutput.name = "Default Output";
  defaultOutput.description = "System default output device";
  defaultOutput.isInput = false;
  defaultOutput.isDefault = true;
  m_outputDevices.prepend(defaultOutput);

  // Cleanup
  pw_proxy_destroy(reinterpret_cast<struct pw_proxy *>(registry));
  pw_core_disconnect(core);
  pw_context_destroy(ctx);
  pw_main_loop_destroy(data.loop);

  qDebug() << "Detected" << m_outputDevices.size() - 1
           << "output devices:";
  for (const auto &device : m_outputDevices) {
    if (device.id == "default")
      continue;
    qDebug() << "  -" << device.name << "(" << device.id << ")";
  }
  qDebug() << "Detected" << m_inputDevices.size() - 1 << "input devices:";
  for (const auto &device : m_inputDevices) {
    if (device.id == "default")
      continue;
    qDebug() << "  -" << device.name << "(" << device.id << ")";
  }
#else
  qDebug() << "PipeWire not available, no devices enumerated";
#endif
}


} // namespace TrophyBackend
