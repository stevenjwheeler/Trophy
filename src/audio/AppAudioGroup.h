/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include "ApplicationAudioIdentity.h"
#include "detection/ProcessGames.h"
#include <QDebug>
#include <QString>
#include <algorithm>
#include <cerrno>
#include <map>
#include <memory>
#include <pipewire/pipewire.h>
#include <set>
#include <spa/utils/result.h>
#include <utility>
#include <vector>

namespace TrophyBackend {

// Owned by one application capture, used only under its PipeWire loop lock.
// WirePlumber connects the original target (and does its format adaptation).
// Supplement that connection with sibling playback ports. PipeWire sums links
// into the capture ports on the graph clock, not on independent callback
// clocks. In particular, Wine can create a silent stream before its real game
// stream.
class AppAudioGroup {
public:
  AppAudioGroup(pw_core *core, pw_stream *capture, uint32_t target,
                const QString &serial, const QString &expectedBinary = {},
                const QString &ownerIdentity = {})
      : m_core(core), m_capture(capture), m_target(target), m_serial(serial),
        m_expectedBinary(expectedBinary), m_gameInstance(ownerIdentity) {
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
          if (id == PW_ID_CORE)
            self->settleCleanup(seq);
          if (id == PW_ID_CORE && seq == self->m_sync) {
            if (!self->m_boundSync) {
              self->m_boundSync = true;
              self->m_sync = pw_core_sync(self->m_core, PW_ID_CORE, seq);
              return; // Flush info for clients/nodes bound during enumeration.
            }
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
    quiesce();
    for (auto &[id, client] : m_boundClients) {
      Q_UNUSED(id);
      release(*client);
    }
    if (m_registry) {
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(m_registry));
    }
    for (auto &[id, node] : m_boundNodes) {
      Q_UNUSED(id);
      release(*node);
    }
    clearLinks();
    for (auto &node : m_retiredNodes)
      release(*node);
    for (auto &client : m_retiredClients)
      release(*client);
    for (auto &link : m_retiredLinks)
      release(*link);
  }

  // Called only on the owned PipeWire loop, like registry/process callbacks.
  bool acceptsAudio() const { return m_acceptAudio; }
  bool rejectedRoute() const { return m_rejected; }
  bool targetGone() const {
    return m_targetRemoved || (m_ready && !m_nodes.contains(m_target));
  }
  void quiesce() {
    if (m_quiesced)
      return;
    m_quiesced = true;
    m_acceptAudio = false;
    // Stop generating binds, syncs and sibling links immediately. Keep proxy
    // removal listeners alive until native teardown so local objects can drain.
    detach(m_coreListener);
    detach(m_listener);
    for (auto &[id, node] : m_boundNodes) {
      Q_UNUSED(id);
      detach(node->listener);
    }
    for (auto &[id, client] : m_boundClients) {
      Q_UNUSED(id);
      detach(client->listener);
    }
    for (auto &[key, link] : m_links) {
      Q_UNUSED(key);
      detach(link->listener);
    }
  }
  bool handlesCleanupError(uint32_t id, int seq, int result) const {
    // Core errors can report an object-local destroy race. Only accept ENOENT
    // for an exact destroy request we issued, until its following sync fence.
    return id == PW_ID_CORE && seq >= 0 && result == -ENOENT &&
           m_cleanupDestroySeqs.contains(SPA_RESULT_ASYNC_SEQ(seq));
  }

  void refresh() { refreshForCapture(pw_stream_get_node_id(m_capture)); }

private:
  friend class AppAudioGroupTestPeer;
  AppAudioGroup() = default; // Daemon-free registry/route tests only.
  void refreshForCapture(uint32_t capture) {
    if (m_quiesced)
      return;
    collectRetired();
    std::set<std::pair<uint32_t, uint32_t>> desired;
    const auto target = m_nodes.find(m_target);
    const auto effective = [&](Node node) {
      const auto owner = m_clients.find(node.client.toUInt());
      if (owner == m_clients.end() || !owner->second.current)
        return node;
      const auto &client = owner->second;
      if ((!node.pid.isEmpty() && !client.pid.isEmpty() &&
           node.pid != client.pid) ||
          (!node.binary.isEmpty() && !client.binary.isEmpty() &&
           node.binary.compare(client.binary, Qt::CaseInsensitive) != 0))
        return node;
      if (node.pid.isEmpty())
        node.pid = client.pid;
      if (node.binary.isEmpty())
        node.binary = client.binary;
      if (node.app.isEmpty())
        node.app = client.app;
      return node;
    };
    const auto instance = [&](const Node &node) {
      const auto identity = effective(node);
      return ProcessGames::associate(detail::audioProcessId(identity.pid),
                                     identity.binary, identity.app);
    };
    const auto belongs = [&](const Node &node) {
      const auto identity = effective(node);
      return ProcessGames::belongsTo(m_gameInstance,
                                     detail::audioProcessId(identity.pid),
                                     identity.binary, identity.app);
    };
    const bool game = m_expectedBinary.isEmpty();
    if (game && m_gameInstance.isEmpty() && target != m_nodes.end())
      m_gameInstance = instance(target->second);
    const auto siblingMatches = [&](const Node &node) {
      return game ? node.current && node.playback && belongs(node)
                  : detail::verifiedAudioSibling(target->second, node,
                                                 m_expectedBinary);
    };
    const bool verified = target != m_nodes.end() &&
                          detail::verifiedAudioTarget(target->second, m_serial,
                                                      m_expectedBinary) &&
                          (!game || belongs(target->second));
    m_acceptAudio = false;
    m_rejected = m_ready && (target == m_nodes.end() ||
                             (target->second.current &&
                              !detail::verifiedAudioTarget(
                                  target->second, m_serial, m_expectedBinary)));
    bool incoming = false;
    if (verified && capture != PW_ID_ANY) {
      // Inspect actual native incoming links. Requested target properties do
      // not prove which node WirePlumber connected (or later moved) here.
      for (const auto &[id, link] : m_incoming) {
        Q_UNUSED(id);
        if (link.second != capture)
          continue;
        const auto source = m_nodes.find(link.first);
        if (source == m_nodes.end() || !source->second.current) {
          m_acceptAudio = false;
          return; // Registry/node-info publication still pending: fail closed.
        }
        if (link.first != m_target && !siblingMatches(source->second)) {
          if (!m_rejected)
            qWarning() << "Rejecting application audio route from node"
                       << link.first << "to capture" << capture
                       << "pinned target" << m_target << "serial" << m_serial;
          m_rejected = true;
        }
        incoming = true;
      }
      m_acceptAudio = m_ready && incoming && !m_rejected;
      for (const auto &[outId, out] : m_ports) {
        const auto node = m_nodes.find(out.node);
        if (!out.output || out.monitor || out.node == m_target ||
            node == m_nodes.end() || !siblingMatches(node->second))
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
        retireLink(std::move(it->second));
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
      auto *link = static_cast<pw_proxy *>(
          pw_core_create_object(m_core, "link-factory", PW_TYPE_INTERFACE_Link,
                                PW_VERSION_LINK, &props->dict, 0));
      pw_properties_free(props);
      if (link) {
        auto owned = std::make_unique<Link>();
        owned->proxy = link;
        watchRemoval(*owned);
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
    spa_hook proxyListener{};
    bool destroyRequested = false;
  };
  struct BoundClient {
    AppAudioGroup *owner = nullptr;
    uint32_t id = PW_ID_ANY;
    pw_client *proxy = nullptr;
    spa_hook listener{};
    spa_hook proxyListener{};
    bool destroyRequested = false;
  };
  struct Link {
    pw_proxy *proxy = nullptr;
    spa_hook listener{};
    spa_hook proxyListener{};
    bool destroyRequested = false;
  };
  static void detach(spa_hook &hook) {
    spa_hook_remove(&hook);
    hook = {};
  }
  template <typename Binding> static void release(Binding &bound) {
    detach(bound.listener);
    detach(bound.proxyListener);
    auto *proxy = std::exchange(bound.proxy, nullptr);
    if (proxy && !bound.destroyRequested)
      pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy));
    // Pending explicit destroys are freed by the immediately following core
    // disconnect. Their callbacks are detached; never issue a second destroy.
  }
  template <typename Binding> static void watchRemoval(Binding &bound) {
    static const pw_proxy_events events = [] {
      pw_proxy_events e{};
      e.version = PW_VERSION_PROXY_EVENTS;
      e.removed = [](void *data) {
        auto &bound = *static_cast<Binding *>(data);
        detach(bound.listener);
        detach(bound.proxyListener);
        auto *proxy = std::exchange(bound.proxy, nullptr);
        // The proxy is now marked removed by PipeWire: this frees it locally
        // without sending core.destroy for an already vanished resource.
        if (proxy)
          pw_proxy_destroy(reinterpret_cast<pw_proxy *>(proxy));
      };
      return e;
    }();
    pw_proxy_add_listener(reinterpret_cast<pw_proxy *>(bound.proxy),
                          &bound.proxyListener, &events, &bound);
  }
  void retireLink(std::unique_ptr<Link> link) {
    if (!link->proxy)
      return;
    detach(link->listener);
    link->destroyRequested = true;
    // pw_core_destroy() discards the protocol request sequence. Call the same
    // public method retaining its result so only this teardown's ENOENT can
    // be classified as harmless (never an arbitrary core error).
    int seq = -EIO;
    // Use the longstanding interface dispatcher, not spa_api_method_r,
    // which is unavailable in the Ubuntu 24.04 PipeWire/SPA headers.
    spa_interface_call_res(reinterpret_cast<spa_interface *>(m_core),
                           struct pw_core_methods, seq, destroy, 0, link->proxy);
    if (seq >= 0) {
      const int request = SPA_RESULT_ASYNC_SEQ(seq);
      m_cleanupDestroySeqs.insert(request);
      const int fence = pw_core_sync(m_core, PW_ID_CORE, 0);
      if (fence >= 0)
        m_cleanupFences.emplace(fence, request);
    }
    m_retiredLinks.push_back(std::move(link));
  }
  void settleCleanup(int seq) {
    const auto it = m_cleanupFences.find(seq);
    if (it != m_cleanupFences.end()) {
      m_cleanupDestroySeqs.erase(it->second);
      m_cleanupFences.erase(it);
    }
    collectRetired();
  }
  void collectRetired() {
    const auto freed = [](const auto &bound) { return !bound->proxy; };
    std::erase_if(m_retiredNodes, freed);
    std::erase_if(m_retiredClients, freed);
    std::erase_if(m_retiredLinks, freed);
  }
  struct Port {
    uint32_t node = PW_ID_ANY, index = PW_ID_ANY;
    QString channel;
    bool output = false, monitor = false;
  };
  static QString property(const spa_dict *props, const char *key) {
    const char *value = props ? spa_dict_lookup(props, key) : nullptr;
    return value ? QString::fromUtf8(value) : QString();
  }
  static void global(void *data, uint32_t id, uint32_t, const char *type,
                     uint32_t, const spa_dict *props) {
    auto *self = static_cast<AppAudioGroup *>(data);
    if (!props)
      return;
    if (strcmp(type, PW_TYPE_INTERFACE_Client) == 0) {
      auto bound = std::make_unique<BoundClient>();
      bound->owner = self;
      bound->id = id;
      bound->proxy = static_cast<pw_client *>(
          pw_registry_bind(self->m_registry, id, PW_TYPE_INTERFACE_Client,
                           PW_VERSION_CLIENT, 0));
      if (!bound->proxy)
        return;
      watchRemoval(*bound);
      static const pw_client_events events = [] {
        pw_client_events e{};
        e.version = PW_VERSION_CLIENT_EVENTS;
        e.info = [](void *data, const pw_client_info *info) {
          auto *bound = static_cast<BoundClient *>(data);
          if (!info || !(info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS))
            return;
          auto &identity = bound->owner->m_clients[bound->id];
          identity.app = property(info->props, PW_KEY_APP_NAME);
          identity.binary = property(info->props, PW_KEY_APP_PROCESS_BINARY);
          identity.pid = property(info->props, PW_KEY_APP_PROCESS_ID);
          identity.current = true;
          bound->owner->refresh();
        };
        return e;
      }();
      pw_client_add_listener(bound->proxy, &bound->listener, &events,
                             bound.get());
      self->m_boundClients.emplace(id, std::move(bound));
    } else if (strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
      self->m_nodes[id] = {property(props, PW_KEY_CLIENT_ID),
                           property(props, PW_KEY_APP_NAME),
                           property(props, PW_KEY_OBJECT_SERIAL),
                           property(props, PW_KEY_APP_PROCESS_BINARY),
                           property(props, PW_KEY_APP_PROCESS_ID),
                           property(props, PW_KEY_MEDIA_CLASS) ==
                               QLatin1String("Stream/Output/Audio"),
                           false};
      auto &node = self->m_nodes[id];
      if (node.playback) {
        auto bound = std::make_unique<BoundNode>();
        bound->owner = self;
        bound->id = id;
        bound->proxy = static_cast<pw_node *>(pw_registry_bind(
            self->m_registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
        if (bound->proxy) {
          watchRemoval(*bound);
          static const pw_node_events nodeEvents = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.info = [](void *data, const pw_node_info *info) {
              auto *bound = static_cast<BoundNode *>(data);
              if (!info || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS))
                return;
              auto &node = bound->owner->m_nodes.at(bound->id);
              const auto update = [&](QString &field, const char *key) {
                field = property(info->props, key);
              };
              update(node.client, PW_KEY_CLIENT_ID);
              update(node.app, PW_KEY_APP_NAME);
              update(node.serial, PW_KEY_OBJECT_SERIAL);
              update(node.binary, PW_KEY_APP_PROCESS_BINARY);
              update(node.pid, PW_KEY_APP_PROCESS_ID);
              node.playback = property(info->props, PW_KEY_MEDIA_CLASS) ==
                              QLatin1String("Stream/Output/Audio");
              node.current = true;
              bound->owner->refresh();
            };
            return e;
          }();
          pw_node_add_listener(bound->proxy, &bound->listener, &nodeEvents,
                               bound.get());
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
      self->m_ports[id] = {
          node, index, property(props, "audio.channel"),
          property(props, PW_KEY_PORT_DIRECTION) == QLatin1String("out"),
          property(props, PW_KEY_PORT_MONITOR) == QLatin1String("true")};
    } else if (strcmp(type, PW_TYPE_INTERFACE_Link) == 0) {
      bool outOk = false, inOk = false;
      const auto out = property(props, PW_KEY_LINK_OUTPUT_NODE).toUInt(&outOk);
      const auto in = property(props, PW_KEY_LINK_INPUT_NODE).toUInt(&inOk);
      if (outOk && inOk)
        self->m_incoming[id] = {out, in};
    } else {
      return;
    }
    self->refresh();
  }
  static void removed(void *data, uint32_t id) {
    auto *self = static_cast<AppAudioGroup *>(data);
    if (id == self->m_target)
      self->m_targetRemoved = true;
    if (auto it = self->m_boundClients.find(id);
        it != self->m_boundClients.end()) {
      detach(it->second->listener);
      self->m_retiredClients.push_back(std::move(it->second));
      self->m_boundClients.erase(it);
    }
    self->m_clients.erase(id);
    if (auto it = self->m_boundNodes.find(id); it != self->m_boundNodes.end()) {
      detach(it->second->listener);
      self->m_retiredNodes.push_back(std::move(it->second));
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
      release(*link);
    }
    m_links.clear();
  }
  pw_core *m_core = nullptr;
  pw_stream *m_capture = nullptr;
  uint32_t m_target = PW_ID_ANY;
  QString m_serial;
  QString m_expectedBinary;
  QString m_gameInstance;
  pw_registry *m_registry = nullptr;
  spa_hook m_listener{};
  spa_hook m_coreListener{};
  int m_sync = 0;
  bool m_boundSync = false;
  bool m_ready = false, m_acceptAudio = false, m_rejected = false;
  bool m_quiesced = false, m_targetRemoved = false;
  std::map<uint32_t, Node> m_nodes;
  std::map<uint32_t, Node> m_clients;
  std::map<uint32_t, std::unique_ptr<BoundClient>> m_boundClients;
  std::map<uint32_t, std::unique_ptr<BoundNode>> m_boundNodes;
  std::map<uint32_t, std::pair<uint32_t, uint32_t>> m_incoming;
  std::map<uint32_t, Port> m_ports;
  std::map<std::pair<uint32_t, uint32_t>, std::unique_ptr<Link>> m_links;
  std::vector<std::unique_ptr<BoundNode>> m_retiredNodes;
  std::vector<std::unique_ptr<BoundClient>> m_retiredClients;
  std::vector<std::unique_ptr<Link>> m_retiredLinks;
  std::set<int> m_cleanupDestroySeqs;
  std::map<int, int> m_cleanupFences;
};
} // namespace TrophyBackend
