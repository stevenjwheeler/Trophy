/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <pipewire/pipewire.h>
#include <QDebug>
#include <QString>
#include <map>
#include <memory>
#include <set>
#include <utility>

namespace TrophyBackend {

// Owned by one application capture, used only under its PipeWire loop lock.
// WirePlumber connects the original target (and does its format adaptation).
// Supplement that connection with sibling playback ports. PipeWire sums links
// into the capture ports on the graph clock, not on independent callback clocks.
// In particular, Wine can create a silent stream before its real game stream.
class AppAudioGroup {
public:
  AppAudioGroup(pw_core *core, pw_stream *capture, uint32_t target,
                const QString &serial)
      : m_core(core), m_capture(capture), m_target(target), m_serial(serial) {
    m_registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
    if (m_registry) {
      static const pw_registry_events events = [] {
        pw_registry_events e{};
        e.version = PW_VERSION_REGISTRY_EVENTS;
        e.global = global;
        e.global_remove = removed;
        return e;
      }();
      pw_registry_add_listener(m_registry, &m_listener, &events, this);
    } else {
      qWarning() << "Application audio: could not observe sibling streams";
    }
  }

  ~AppAudioGroup() {
    if (m_registry) {
      spa_hook_remove(&m_listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
    }
    clearLinks();
  }

  void refresh() {
    std::set<std::pair<uint32_t, uint32_t>> desired;
    const auto target = m_nodes.find(m_target);
    const uint32_t capture = pw_stream_get_node_id(m_capture);
    if (target != m_nodes.end() && capture != PW_ID_ANY &&
        (m_serial.isEmpty() || target->second.serial == m_serial) &&
        target->second.playback && !target->second.client.isEmpty() &&
        !target->second.app.isEmpty()) {
      for (const auto &[outId, out] : m_ports) {
        const auto node = m_nodes.find(out.node);
        if (!out.output || out.monitor || out.node == m_target ||
            node == m_nodes.end() || !node->second.playback ||
            node->second.client != target->second.client ||
            node->second.app != target->second.app)
          continue;
        for (const auto &[inId, in] : m_ports) {
          if (in.node != capture || in.output || in.monitor)
            continue;
          // Match the negotiated graph channels, not the interleaved output
          // format. Mono playback is duplicated into stereo inputs.
          if ((!out.channel.isEmpty() && out.channel == in.channel) ||
              (out.channel == QLatin1String("MONO") &&
               (in.channel == QLatin1String("FL") ||
                in.channel == QLatin1String("FR"))) ||
              (out.channel.isEmpty() && in.channel.isEmpty() &&
               out.index == in.index))
            desired.emplace(outId, inId);
        }
      }
    }
    for (auto it = m_links.begin(); it != m_links.end();) {
      if (!desired.contains(it->first)) {
        pw_proxy_destroy(it->second->proxy);
        it = m_links.erase(it);
      } else {
        ++it;
      }
    }
    for (const auto &key : desired) {
      if (m_links.contains(key))
        continue;
      const auto &out = m_ports.at(key.first);
      const auto &in = m_ports.at(key.second);
      auto *props = pw_properties_new(nullptr, nullptr);
      if (!props)
        continue;
      pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", out.node);
      pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out.index);
      pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", in.node);
      pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", in.index);
      // No linger: all links disappear with this capture's connection.
      auto *link = static_cast<pw_proxy *>(pw_core_create_object(
          m_core, "link-factory", PW_TYPE_INTERFACE_Link, PW_VERSION_LINK,
          &props->dict, 0));
      pw_properties_free(props);
      if (link) {
        auto owned = std::make_unique<Link>();
        owned->proxy = link;
        static const pw_proxy_events events = [] {
          pw_proxy_events e{};
          e.version = PW_VERSION_PROXY_EVENTS;
          e.error = [](void *, int, int result, const char *message) {
            qWarning() << "Application sibling audio link failed:" << result
                       << (message ? message : "unknown error");
          };
          return e;
        }();
        pw_proxy_add_listener(link, &owned->listener, &events, owned.get());
        m_links.emplace(key, std::move(owned));
        qDebug() << "Application audio: including sibling playback node"
                 << out.node << "channel" << out.channel;
      }
    }
  }

private:
  struct Node {
    QString client, app, serial;
    bool playback = false;
  };
  struct Link {
    pw_proxy *proxy = nullptr;
    spa_hook listener{};
  };
  struct Port {
    uint32_t node = PW_ID_ANY, index = PW_ID_ANY;
    QString channel;
    bool output = false, monitor = false;
  };
  static QString property(const spa_dict *props, const char *key) {
    const char *value = spa_dict_lookup(props, key);
    return value ? QString::fromUtf8(value) : QString();
  }
  static void global(void *data, uint32_t id, uint32_t, const char *type,
                     uint32_t, const spa_dict *props) {
    auto *self = static_cast<AppAudioGroup *>(data);
    if (!props)
      return;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      self->m_nodes[id] = {property(props, PW_KEY_CLIENT_ID),
                           property(props, PW_KEY_APP_NAME),
                           property(props, PW_KEY_OBJECT_SERIAL),
                           property(props, PW_KEY_MEDIA_CLASS) ==
                               QLatin1String("Stream/Output/Audio")};
    } else if (strcmp(type, PW_TYPE_INTERFACE_Port) == 0) {
      bool nodeOk = false, indexOk = false;
      const uint32_t node = property(props, PW_KEY_NODE_ID).toUInt(&nodeOk);
      const uint32_t index = property(props, PW_KEY_PORT_ID).toUInt(&indexOk);
      if (!nodeOk || !indexOk)
        return;
      self->m_ports[id] = {node, index, property(props, "audio.channel"),
                          property(props, PW_KEY_PORT_DIRECTION) ==
                              QLatin1String("out"),
                          property(props, PW_KEY_PORT_MONITOR) ==
                              QLatin1String("true")};
    } else {
      return;
    }
    self->refresh();
  }
  static void removed(void *data, uint32_t id) {
    auto *self = static_cast<AppAudioGroup *>(data);
    self->m_nodes.erase(id);
    self->m_ports.erase(id);
    self->refresh();
  }
  void clearLinks() {
    for (const auto &[key, link] : m_links) {
      Q_UNUSED(key);
      pw_proxy_destroy(link->proxy);
    }
    m_links.clear();
  }
  pw_core *m_core;
  pw_stream *m_capture;
  uint32_t m_target;
  QString m_serial;
  pw_registry *m_registry = nullptr;
  spa_hook m_listener{};
  std::map<uint32_t, Node> m_nodes;
  std::map<uint32_t, Port> m_ports;
  std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<Link>> m_links;
};
} // namespace TrophyBackend
