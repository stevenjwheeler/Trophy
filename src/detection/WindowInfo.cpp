/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "WindowInfo.h"

namespace TrophyBackend {

WindowInfo WindowInfo::fromJson(const QJsonObject &o) {
  WindowInfo w;
  // String-encoded numbers are real (compositor JSON is loose); toDouble
  // also rejects them and loses precision past 2^53.
  w.id = o.value("id").toVariant().toULongLong();
  w.key = o.value("key").toString();
  w.appId = o.value("appId").toString();
  w.title = o.value("title").toString();
  w.pid = o.value("pid").toVariant().toLongLong();
  w.fullscreen = o.value("fullscreen").toBool();
  w.focused = o.value("focused").toBool();
  w.minimized = o.value("minimized").toBool();
  w.x = o.value("x").toInt();
  w.y = o.value("y").toInt();
  w.width = o.value("width").toInt();
  w.height = o.value("height").toInt();
  w.outputWidth = o.value("outputWidth").toInt();
  w.outputHeight = o.value("outputHeight").toInt();
  return w;
}

} // namespace TrophyBackend
