/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "AudioApplicationRegistry.h"
#include "ApplicationAudioLookup.h"
#include "AudioDiscoveryDiagnostics.h"
#include <QDebug>
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <condition_variable>
#include <map>
#include <mutex>
#include <thread>
#include <pipewire/pipewire.h>

namespace TrophyBackend {
struct AudioApplicationRegistry::Private {
  std::atomic_bool stopping{false};
  std::atomic_uint connectionAttempts{0};
  std::mutex mutex;
  std::condition_variable wake;
  std::thread worker;
  pw_loop *activeLoop = nullptr;
  spa_source *activeWake = nullptr;

  struct Session {
    struct Entry {
      Session *owner = nullptr;
      uint32_t id = 0;
      bool node = false, current = false;
      detail::AudioLookupProperties properties;
      pw_proxy *proxy = nullptr;
      spa_hook listener{};
      ~Entry() {
        if (proxy) {
          spa_hook_remove(&listener);
          pw_proxy_destroy(proxy);
        }
      }
    };
    pw_main_loop *loop = nullptr;
    pw_context *context = nullptr;
    pw_core *core = nullptr;
    pw_registry *registry = nullptr;
    spa_hook coreListener{}, registryListener{};
    spa_source *wakeEvent = nullptr;
    std::map<uint32_t, std::unique_ptr<Entry>> entries;
    bool failed = false, pending = false, again = false, ready = false, dirty = true;
    int sequence = -1;
    QString failure;

    bool fail(const char *stage, int result) {
      if (!failed)
        failure = detail::audioDiscoveryFailure(stage, result);
      failed = true;
      return false;
    }

    ~Session() {
      entries.clear();
      if (registry) {
        spa_hook_remove(&registryListener);
        pw_proxy_destroy(reinterpret_cast<pw_proxy *>(registry));
      }
      if (core) {
        spa_hook_remove(&coreListener);
        pw_core_disconnect(core);
      }
      if (context) pw_context_destroy(context);
      if (wakeEvent) pw_loop_destroy_source(pw_main_loop_get_loop(loop), wakeEvent);
      if (loop) pw_main_loop_destroy(loop);
    }
    void changed() {
      dirty = true;
      if (pending) {
        again = true;
        return;
      }
      pending = true;
      sequence = pw_core_sync(core, PW_ID_CORE, 0);
      if (sequence < 0) fail("registry sync", sequence);
    }
    static void nodeInfo(void *data, const pw_node_info *info) {
      auto &entry = *static_cast<Entry *>(data);
      if (!info || !(info->change_mask & PW_NODE_CHANGE_MASK_PROPS))
        return;
      entry.properties.updateInfo(info->props, true);
      entry.current = true;
      entry.owner->changed();
    }
    static void clientInfo(void *data, const pw_client_info *info) {
      auto &entry = *static_cast<Entry *>(data);
      if (!info || !(info->change_mask & PW_CLIENT_CHANGE_MASK_PROPS))
        return;
      entry.properties.updateInfo(info->props, true);
      entry.current = true;
      entry.owner->changed();
    }
    static void global(void *data, uint32_t id, uint32_t, const char *type,
                       uint32_t version, const spa_dict *props) {
      auto &self = *static_cast<Session *>(data);
      const bool node = strcmp(type, PW_TYPE_INTERFACE_Node) == 0;
      const bool client = strcmp(type, PW_TYPE_INTERFACE_Client) == 0;
      if ((!node && !client) || self.entries.size() >= 4096 || self.entries.contains(id))
        return;
      auto entry = std::make_unique<Entry>();
      entry->owner = &self;
      entry->id = id;
      entry->node = node;
      entry->properties.update(props);
      entry->proxy = static_cast<pw_proxy *>(pw_registry_bind(self.registry, id, type,
          std::min(version, uint32_t(node ? PW_VERSION_NODE : PW_VERSION_CLIENT)), 0));
      if (!entry->proxy)
        return;
      static const pw_node_events nodes = [] {
        pw_node_events events{};
        events.version = PW_VERSION_NODE_EVENTS;
        events.info = nodeInfo;
        return events;
      }();
      static const pw_client_events clients = [] {
        pw_client_events events{};
        events.version = PW_VERSION_CLIENT_EVENTS;
        events.info = clientInfo;
        return events;
      }();
      if (node)
        pw_node_add_listener(reinterpret_cast<pw_node *>(entry->proxy), &entry->listener,
                             &nodes, entry.get());
      else
        pw_client_add_listener(reinterpret_cast<pw_client *>(entry->proxy), &entry->listener,
                               &clients, entry.get());
      self.entries.emplace(id, std::move(entry));
      self.changed();
    }
    static void removed(void *data, uint32_t id) {
      auto &self = *static_cast<Session *>(data);
      if (self.entries.erase(id))
        self.changed();
    }
    static void done(void *data, uint32_t id, int sequence) {
      auto &self = *static_cast<Session *>(data);
      if (id != PW_ID_CORE || sequence != self.sequence)
        return;
      self.pending = false;
      if (self.again) {
        self.again = false;
        self.changed();
      } else {
        self.ready = true;
      }
    }
    static void error(void *data, uint32_t id, int sequence, int result, const char *message) {
      if (id == PW_ID_CORE || result == -EPIPE) {
        auto &self = *static_cast<Session *>(data);
        if (!self.failed) {
          self.fail("PipeWire core error", result);
          self.failure += QStringLiteral("; object %1, sequence %2; %3")
              .arg(id).arg(sequence)
              .arg(QString::fromUtf8(message ? message : "").left(512)
                       .replace('\n', ' ').replace('\r', ' '));
        }
      }
    }
    bool connect() {
      loop = pw_main_loop_new(nullptr);
      if (!loop) return fail("create main loop", -errno);
      context = pw_context_new(pw_main_loop_get_loop(loop), nullptr, 0);
      if (!context) return fail("create context", -errno);
      core = pw_context_connect(context, pw_properties_new(
          PW_KEY_APP_NAME, "Trophy audio application discovery", nullptr), 0);
      if (!core) return fail("connect to PipeWire", -errno);
      static const pw_core_events cores = [] {
        pw_core_events events{};
        events.version = PW_VERSION_CORE_EVENTS;
        events.done = done;
        events.error = error;
        return events;
      }();
      pw_core_add_listener(core, &coreListener, &cores, this);
      registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
      if (!registry) return fail("get registry", -errno);
      static const pw_registry_events globals = [] {
        pw_registry_events events{};
        events.version = PW_VERSION_REGISTRY_EVENTS;
        events.global = global;
        events.global_remove = removed;
        return events;
      }();
      pw_registry_add_listener(registry, &registryListener, &globals, this);
      wakeEvent = pw_loop_add_event(pw_main_loop_get_loop(loop), [](void *, uint64_t) {}, nullptr);
      changed();
      return true;
    }
    QVariantList snapshot() const {
      QVariantList result;
      for (const auto &[id, entry] : entries) {
        if (!entry->node || !entry->current ||
            entry->properties.get(PW_KEY_MEDIA_CLASS) != "Stream/Output/Audio")
          continue;
        auto properties = entry->properties;
        bool valid = false;
        const auto owner = properties.get(PW_KEY_CLIENT_ID).toUInt(&valid);
        const auto client = entries.find(owner);
        if (valid && client != entries.end() && !client->second->node && client->second->current)
          properties = detail::gameAudioLookupProperties(properties, client->second->properties);
        auto row = detail::audioApplicationRow(properties, id);
        if (!row.isEmpty())
          result.append(row);
      }
      return result;
    }
  };

  void run(AudioApplicationRegistry *owner, quint64 generation) {
    pw_init(nullptr, nullptr);
    auto send = [owner, generation](QVariantList apps, bool available) {
      QMetaObject::invokeMethod(owner, [owner, generation, apps = std::move(apps), available]() mutable {
        owner->publish(generation, std::move(apps), available);
      }, Qt::QueuedConnection);
    };
    bool warned = false;
    int retryMs = 250;
    while (!stopping) {
      QString failure;
      {
        Session session;
        ++connectionAttempts;
        if (session.connect()) {
          {
            std::lock_guard lock(mutex);
            activeLoop = pw_main_loop_get_loop(session.loop);
            activeWake = session.wakeEvent;
          }
          const auto started = std::chrono::steady_clock::now();
          QVariantList previous;
          bool published = false;
          while (!stopping && !session.failed) {
            const int result = pw_loop_iterate(pw_main_loop_get_loop(session.loop),
                                               session.ready && session.wakeEvent ? -1 : 100);
            // A signal interrupting the wait is not a lost connection.
            if (detail::audioDiscoveryLoopInterrupted(result))
              continue;
            if (result < 0) {
              session.fail("event-loop iteration", result);
              break;
            }
            if (!session.ready && std::chrono::steady_clock::now() - started >
                                      std::chrono::seconds(5)) {
              session.fail("initial registry sync exceeded 5 seconds", -ETIMEDOUT);
              break;
            }
            if (session.ready && !session.pending && session.dirty) {
              auto apps = session.snapshot();
              if (!published || apps != previous) {
                previous = apps;
                send(std::move(apps), true);
                published = true;
              }
              session.dirty = false;
              retryMs = 250;
              warned = false;
            }
          }
          {
            std::lock_guard lock(mutex);
            activeLoop = nullptr;
            activeWake = nullptr;
          }
        }
        failure = session.failure;
      } // Tear down all native proxies before reconnecting.
      if (stopping) break;
      send({}, false); // Never retain stale node IDs across a service restart.
      if (!warned) {
        qWarning().noquote() << "Audio application discovery disconnected; retrying in the background:"
                             << failure;
        warned = true;
      }
      std::unique_lock lock(mutex);
      wake.wait_for(lock, std::chrono::milliseconds(retryMs), [&] { return stopping.load(); });
      retryMs = std::min(retryMs * 2, 5000);
    }
  }
};

AudioApplicationRegistry::AudioApplicationRegistry(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>()) {}
AudioApplicationRegistry::~AudioApplicationRegistry() { stop(); }
unsigned AudioApplicationRegistry::connectionAttempts() const {
  return d->connectionAttempts.load();
}
void AudioApplicationRegistry::start() {
  if (m_running) return;
  m_running = true;
  d->stopping = false;
  const auto generation = ++m_generation;
  d->worker = std::thread([this, generation] { d->run(this, generation); });
}
void AudioApplicationRegistry::stop() {
  m_running = false;
  ++m_generation;
  d->stopping = true;
  {
    std::lock_guard lock(d->mutex);
    if (d->activeWake)
      pw_loop_signal_event(d->activeLoop, d->activeWake);
  }
  d->wake.notify_all();
  if (d->worker.joinable()) d->worker.join();
  if (!m_applications.isEmpty()) {
    m_applications.clear();
    emit applicationsChanged();
  }
  if (m_available) {
    m_available = false;
    emit availabilityChanged();
  }
}
void AudioApplicationRegistry::publish(quint64 generation, QVariantList applications, bool available) {
  if (!m_running || generation != m_generation)
    return;
  if (m_available != available) {
    m_available = available;
    emit availabilityChanged();
  }
  if (!m_running || generation != m_generation)
    return;
  if (m_applications == applications)
    return;
  m_applications = std::move(applications);
  emit applicationsChanged();
}
} // namespace TrophyBackend
