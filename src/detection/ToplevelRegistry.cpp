/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "ToplevelRegistry.h"

#include <QDebug>
#include <QHash>
#include <QSocketNotifier>

#include <algorithm>
#include <cerrno>
#include <cstring>

#ifdef HAVE_WAYLAND_TOPLEVEL
#include <wayland-client.h>

#include "ext-foreign-toplevel-list-client-protocol.h"
#endif

namespace TrophyBackend {

#ifdef HAVE_WAYLAND_TOPLEVEL

class ToplevelRegistry::Private {
public:
  struct HandleData {
    Private *owner = nullptr;
    quintptr id = 0;
  };

  ToplevelRegistry *q = nullptr;
  ToplevelModel model;
  wl_display *display = nullptr;
  wl_registry *registry = nullptr;
  ext_foreign_toplevel_list_v1 *list = nullptr;
  QSocketNotifier *readNotifier = nullptr;
  QSocketNotifier *writeNotifier = nullptr;
  QHash<quintptr, ext_foreign_toplevel_handle_v1 *> handles;
  QHash<quintptr, HandleData *> handleData;
  bool active = false;

  void dispatch();
  void flush();
  void fail(const QString &reason);
  void destroyAll();
};

namespace {

using RegistryPrivate = ToplevelRegistry::Private;
using HandleData = ToplevelRegistry::Private::HandleData;

void onIdentifier(void *data, ext_foreign_toplevel_handle_v1 *,
                  const char *identifier) {
  auto *h = static_cast<HandleData *>(data);
  if (h && h->owner)
    h->owner->model.setIdentifier(h->id,
                                  QString::fromUtf8(identifier ? identifier : ""));
}

void onAppId(void *data, ext_foreign_toplevel_handle_v1 *, const char *appId) {
  auto *h = static_cast<HandleData *>(data);
  if (h && h->owner)
    h->owner->model.setAppId(h->id,
                             QString::fromUtf8(appId ? appId : ""));
}

void onTitle(void *data, ext_foreign_toplevel_handle_v1 *, const char *title) {
  auto *h = static_cast<HandleData *>(data);
  if (h && h->owner)
    h->owner->model.setTitle(h->id,
                             QString::fromUtf8(title ? title : ""));
}

void onDone(void *data, ext_foreign_toplevel_handle_v1 *) {
  auto *h = static_cast<HandleData *>(data);
  if (!h || !h->owner)
    return;
  const ToplevelModel::Change change = h->owner->model.commit(h->id);
  if (change == ToplevelModel::Change::None)
    return;
  const ToplevelInfo info = h->owner->model.info(h->id);
  if (change == ToplevelModel::Change::Opened)
    emit h->owner->q->toplevelOpened(info);
  else
    emit h->owner->q->toplevelUpdated(info);
  emit h->owner->q->toplevelsChanged();
}

void onClosed(void *data, ext_foreign_toplevel_handle_v1 *handle) {
  auto *h = static_cast<HandleData *>(data);
  if (!h || !h->owner)
    return;
  RegistryPrivate *owner = h->owner;
  const QString identifier = owner->model.info(h->id).identifier;
  owner->model.remove(h->id);
  owner->handles.remove(h->id);
  owner->handleData.remove(h->id);
  ext_foreign_toplevel_handle_v1_destroy(handle);
  delete h;
  if (!identifier.isEmpty())
    emit owner->q->toplevelClosed(identifier);
  emit owner->q->toplevelsChanged();
}

const ext_foreign_toplevel_handle_v1_listener &handleListener() {
  static const ext_foreign_toplevel_handle_v1_listener listener = [] {
    ext_foreign_toplevel_handle_v1_listener l{};
    l.identifier = onIdentifier;
    l.app_id = onAppId;
    l.title = onTitle;
    l.done = onDone;
    l.closed = onClosed;
    return l;
  }();
  return listener;
}

void onListToplevel(void *data, ext_foreign_toplevel_list_v1 *,
                    ext_foreign_toplevel_handle_v1 *handle) {
  auto *p = static_cast<RegistryPrivate *>(data);
  if (!p || !handle)
    return;
  const quintptr id = p->model.add();
  auto *h = new HandleData{p, id};
  p->handles.insert(id, handle);
  p->handleData.insert(id, h);
  ext_foreign_toplevel_handle_v1_add_listener(handle, &handleListener(), h);
}

void onListFinished(void *data, ext_foreign_toplevel_list_v1 *) {
  auto *p = static_cast<RegistryPrivate *>(data);
  if (p)
    qInfo() << "ToplevelRegistry: compositor stopped the toplevel list";
}

const ext_foreign_toplevel_list_v1_listener &listListener() {
  static const ext_foreign_toplevel_list_v1_listener listener = [] {
    ext_foreign_toplevel_list_v1_listener l{};
    l.toplevel = onListToplevel;
    l.finished = onListFinished;
    return l;
  }();
  return listener;
}

void onRegistryGlobal(void *data, wl_registry *registry, uint32_t name,
                      const char *interface, uint32_t version) {
  auto *p = static_cast<RegistryPrivate *>(data);
  if (!p || p->list || !interface || version < 1)
    return;
  if (std::strcmp(interface, ext_foreign_toplevel_list_v1_interface.name) != 0)
    return;
  p->list = static_cast<ext_foreign_toplevel_list_v1 *>(wl_registry_bind(
      registry, name, &ext_foreign_toplevel_list_v1_interface,
      std::min<uint32_t>(version, 1)));
  ext_foreign_toplevel_list_v1_add_listener(p->list, &listListener(), p);
}

void onRegistryGlobalRemove(void *, wl_registry *, uint32_t) {}

const wl_registry_listener &registryListener() {
  static const wl_registry_listener listener = [] {
    wl_registry_listener l{};
    l.global = onRegistryGlobal;
    l.global_remove = onRegistryGlobalRemove;
    return l;
  }();
  return listener;
}

} // namespace

void ToplevelRegistry::Private::fail(const QString &reason) {
  if (!active && !display)
    return;
  active = false;
  if (readNotifier)
    readNotifier->setEnabled(false);
  if (writeNotifier)
    writeNotifier->setEnabled(false);
  qWarning() << "ToplevelRegistry:" << reason;
  if (q)
    emit q->connectionLost(reason);
}

void ToplevelRegistry::Private::flush() {
  if (!display || !active)
    return;
  if (wl_display_flush(display) == 0) {
    if (writeNotifier)
      writeNotifier->setEnabled(false);
  } else if (errno == EAGAIN) {
    if (writeNotifier)
      writeNotifier->setEnabled(true);
  }
}

void ToplevelRegistry::Private::dispatch() {
  if (!display || !active)
    return;
  while (wl_display_prepare_read(display) != 0) {
    if (wl_display_dispatch_pending(display) < 0) {
      fail(QStringLiteral("dispatch failed: %1")
               .arg(QString::fromLocal8Bit(std::strerror(errno))));
      return;
    }
  }
  if (wl_display_flush(display) < 0 && errno != EAGAIN) {
    wl_display_cancel_read(display);
    fail(QStringLiteral("flush failed: %1")
             .arg(QString::fromLocal8Bit(std::strerror(errno))));
    return;
  }
  if (wl_display_read_events(display) < 0) {
    fail(QStringLiteral("read failed: %1")
             .arg(QString::fromLocal8Bit(std::strerror(errno))));
    return;
  }
  if (wl_display_dispatch_pending(display) < 0) {
    fail(QStringLiteral("pending dispatch failed: %1")
             .arg(QString::fromLocal8Bit(std::strerror(errno))));
    return;
  }
  flush();
}

void ToplevelRegistry::Private::destroyAll() {
  active = false;
  if (readNotifier) {
    readNotifier->setEnabled(false);
    readNotifier->deleteLater();
    readNotifier = nullptr;
  }
  if (writeNotifier) {
    writeNotifier->setEnabled(false);
    writeNotifier->deleteLater();
    writeNotifier = nullptr;
  }
  for (auto it = handles.begin(); it != handles.end(); ++it) {
    delete handleData.take(it.key());
    ext_foreign_toplevel_handle_v1_destroy(it.value());
  }
  handles.clear();
  handleData.clear();
  if (list) {
    ext_foreign_toplevel_list_v1_destroy(list);
    list = nullptr;
  }
  if (registry) {
    wl_registry_destroy(registry);
    registry = nullptr;
  }
  if (display) {
    wl_display_flush(display);
    wl_display_disconnect(display);
    display = nullptr;
  }
  model = ToplevelModel{};
}

ToplevelRegistry::ToplevelRegistry(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>()) {
  d->q = this;
}

ToplevelRegistry::~ToplevelRegistry() { stop(); }

bool ToplevelRegistry::isAvailable() {
  return !qEnvironmentVariableIsEmpty("WAYLAND_DISPLAY") ||
         !qEnvironmentVariableIsEmpty("WAYLAND_SOCKET");
}

bool ToplevelRegistry::start(QString *error) {
  if (d->display)
    return true;
  if (!isAvailable()) {
    if (error)
      *error = QStringLiteral("No Wayland session in the environment");
    return false;
  }
  d->display = wl_display_connect(nullptr);
  if (!d->display) {
    if (error)
      *error = QStringLiteral("Could not connect to the Wayland display");
    return false;
  }
  d->registry = wl_display_get_registry(d->display);
  wl_registry_add_listener(d->registry, &registryListener(), d.get());
  if (wl_display_roundtrip(d->display) < 0 || !d->list) {
    d->destroyAll();
    if (error)
      *error = QStringLiteral(
          "Compositor does not expose ext-foreign-toplevel-list-v1");
    return false;
  }
  // Second roundtrip receives the initial toplevel handles and properties.
  if (wl_display_roundtrip(d->display) < 0) {
    d->destroyAll();
    if (error)
      *error = QStringLiteral("Wayland roundtrip failed");
    return false;
  }

  const int fd = wl_display_get_fd(d->display);
  if (fd < 0) {
    d->destroyAll();
    if (error)
      *error = QStringLiteral("Wayland display has no file descriptor");
    return false;
  }
  d->active = true;
  d->readNotifier = new QSocketNotifier(fd, QSocketNotifier::Read, this);
  connect(d->readNotifier, &QSocketNotifier::activated, this,
          [this] { d->dispatch(); });
  d->writeNotifier = new QSocketNotifier(fd, QSocketNotifier::Write, this);
  d->writeNotifier->setEnabled(false);
  connect(d->writeNotifier, &QSocketNotifier::activated, this,
          [this] { d->flush(); });
  d->flush();
  qInfo() << "ToplevelRegistry: bound ext-foreign-toplevel-list-v1 with"
          << d->model.size() << "toplevel(s)";
  return true;
}

void ToplevelRegistry::stop() {
  d->active = false;
  d->destroyAll();
}

bool ToplevelRegistry::isActive() const { return d->active; }

QList<ToplevelInfo> ToplevelRegistry::toplevels() const {
  return d->model.snapshot();
}

#else // !HAVE_WAYLAND_TOPLEVEL

class ToplevelRegistry::Private {};

ToplevelRegistry::ToplevelRegistry(QObject *parent)
    : QObject(parent), d(std::make_unique<Private>()) {}
ToplevelRegistry::~ToplevelRegistry() = default;

bool ToplevelRegistry::isAvailable() { return false; }

bool ToplevelRegistry::start(QString *error) {
  if (error)
    *error = QStringLiteral("Wayland client support was not compiled in");
  return false;
}

void ToplevelRegistry::stop() {}
bool ToplevelRegistry::isActive() const { return false; }
QList<ToplevelInfo> ToplevelRegistry::toplevels() const { return {}; }

#endif

} // namespace TrophyBackend
