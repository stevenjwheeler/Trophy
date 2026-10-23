/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <pipewire/pipewire.h>
#include <QDebug>
#include <QString>
#include <map>
#include <memory>
#include <set>
#include <utility>
#include "ApplicationAudioIdentity.h"

namespace TrophyBackend {

// Owned by one application capture, used only under its PipeWire loop lock.
// WirePlumber connects the original target (and does its format adaptation).
// Supplement that connection with sibling playback ports. PipeWire sums links
// into the capture ports on the graph clock, not on independent callback clocks.
// In particular, Wine can create a silent stream before its real game stream.
class AppAudioGroup {
public:
  AppAudioGroup(pw_core *core, pw_stream *capture, uint32_t target,
                const QString &serial, const QString &expectedBinary = {})
      : m_core(core), m_capture(capture), m_target(target), m_serial(serial),
        m_expectedBinary(expectedBinary) {
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
      static const pw_core_events coreEvents = [] {
        pw_core_events e{};
        e.version = PW_VERSION_CORE_EVENTS;
        e.done = [](void *data, uint32_t id, int seq) {
          auto *self = static_cast<AppAudioGroup *>(data);
          if (id == PW_ID_CORE && seq == self->m_sync) {
            self->m_ready = true;
            self->refresh();
          }
        };
        return e;
      }();
      pw_core_add_listener(m_core, &m_coreListener, &coreEvents, this);
      m_sync = pw_core_sync(m_core, PW_ID_CORE, 0);
    } else {
      m_rejected = true;
      qWarning() << "Application audio: could not observe sibling streams";
    }
  }

  ~AppAudioGroup() {
    if (m_registry) {
      spa_hook_remove(&m_coreListener);
      spa_hook_remove(&m_listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
    }
    for (auto &[id, node] : m_boundNodes) {
      Q_UNUSED(id);
      spa_hook_remove(&node->listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(node->proxy));
    }
    clearLinks();
  }

  // Called only on the owned PipeWire loop, like registry/process callbacks.
  bool acceptsAudio() const { return m_acceptAudio; }
  bool rejectedRoute() const { return m_rejected; }

  void refresh() {
    refreshForCapture(pw_stream_get_node_id(m_capture));
  }

private:
  friend class AppAudioGroupTestPeer;
  AppAudioGroup() = default; // Daemon-free registry/route tests only.
  void refreshForCapture(uint32_t capture) {
    std::set<std::pair<uint32_t, uint32_t>> desired;
    const auto target = m_nodes.find(m_target);
    const bool verified = target != m_nodes.end() &&
        detail::verifiedAudioTarget(target->second, m_serial, m_expectedBinary);
    m_acceptAudio = false;
    m_rejected = m_ready && (target == m_nodes.end() ||
        (target->second.current && !verified));
    bool incoming = false;
    if (verified && capture != PW_ID_ANY) {
      // Inspect actual native incoming links. Requested target properties do
      // not prove which node WirePlumber connected (or later moved) here.
      for (const auto &[id, link] : m_incoming) {
        Q_UNUSED(id);
        if (link.second != capture) continue;
        const auto source = m_nodes.find(link.first);
        if (source == m_nodes.end() || !source->second.current) {
          m_acceptAudio = false;
          return; // Registry/node-info publication still pending: fail closed.
        }
        if (link.first != m_target && !detail::verifiedAudioSibling(
                target->second, source->second, m_expectedBinary)) {
          if (!m_rejected)
            qWarning() << "Rejecting application audio route from node" << link.first
                       << "to capture" << capture << "pinned target" << m_target
                       << "serial" << m_serial;
          m_rejected = true;
        }
        incoming = true;
      }
      m_acceptAudio = m_ready && incoming && !m_rejected;
      for (const auto &[outId, out] : m_ports) {
        const auto node = m_nodes.find(out.node);
        if (!out.output || out.monitor || out.node == m_target ||
            node == m_nodes.end() || !detail::verifiedAudioSibling(
                target->second, node->second, m_expectedBinary))
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
      pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", key.first);
      pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", in.node);
      pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", key.second);
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
  using Node = detail::ApplicationAudioIdentity;
  struct BoundNode {
    AppAudioGroup *owner = nullptr;
    uint32_t id = PW_ID_ANY;
    pw_node *proxy = nullptr;
    spa_hook listener{};
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
                            property(props, PW_KEY_APP_PROCESS_BINARY),
                            property(props, PW_KEY_APP_PROCESS_ID),
                            property(props, PW_KEY_MEDIA_CLASS) ==
                                QLatin1String("Stream/Output/Audio"), false};
      auto &node = self->m_nodes[id];
      if (node.playback) {
        auto bound = std::make_unique<BoundNode>();
        bound->owner = self;
        bound->id = id;
        bound->proxy = static_cast<pw_node *>(pw_registry_bind(
            self->m_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (bound->proxy) {
          static const pw_node_events nodeEvents = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.info = [](void *data, const pw_node_info *info) {
              auto *bound = static_cast<BoundNode *>(data);
              if (!info || !info->props) return;
              auto &node = bound->owner->m_nodes.at(bound->id);
              const auto update = [&](QString &field, const char *key) {
                if (const char *value = spa_dict_lookup(info->props, key))
                  field = QString::fromUtf8(value);
              };
              update(node.client, PW_KEY_CLIENT_ID);
              update(node.app, PW_KEY_APP_NAME);
              update(node.serial, PW_KEY_OBJECT_SERIAL);
              update(node.binary, PW_KEY_APP_PROCESS_BINARY);
              update(node.pid, PW_KEY_APP_PROCESS_ID);
              node.current = true;
              bound->owner->refresh();
            };
            return e;
          }();
          pw_node_add_listener(bound->proxy, &bound->listener, &nodeEvents, bound.get());
          self->m_boundNodes.emplace(id, std::move(bound));
        }
      } else {
        node.current = true;
      }
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
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
      bool outOk = false, inOk = false;
      const auto out = property(props, PW_KEY_LINK_OUTPUT_NODE).toUInt(&outOk);
      const auto in = property(props, PW_KEY_LINK_INPUT_NODE).toUInt(&inOk);
      if (outOk && inOk) self->m_incoming[id] = {out, in};
    } else {
      return;
    }
    self->refresh();
  }
  static void removed(void *data, uint32_t id) {
    auto *self = static_cast<AppAudioGroup *>(data);
    if (auto it = self->m_boundNodes.find(id); it != self->m_boundNodes.end()) {
      spa_hook_remove(&it->second->listener);
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(it->second->proxy));
      self->m_boundNodes.erase(it);
    }
    self->m_nodes.erase(id);
    self->m_ports.erase(id);
    self->m_incoming.erase(id);
    self->refresh();
  }
  void clearLinks() {
    for (const auto &[key, link] : m_links) {
      Q_UNUSED(key);
      pw_proxy_destroy(link->proxy);
    }
    m_links.clear();
  }
  pw_core *m_core = nullptr;
  pw_stream *m_capture = nullptr;
  uint32_t m_target = PW_ID_ANY;
  QString m_serial;
  QString m_expectedBinary;
  pw_registry *m_registry = nullptr;
  spa_hook m_listener{};
  spa_hook m_coreListener{};
  int m_sync = 0;
  bool m_ready = false, m_acceptAudio = false, m_rejected = false;
  std::map<uint32_t, Node> m_nodes;
  std::map<uint32_t, std::unique_ptr<BoundNode>> m_boundNodes;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> m_incoming;
  std::map<uint32_t, Port> m_ports;
  std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<Link>> m_links;
};
} // namespace TrophyBackend
