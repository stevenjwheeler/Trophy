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
  w.id = (quint64)o.value("id").toDouble();
  w.appId = o.value("appId").toString();
  w.title = o.value("title").toString();
  w.pid = (qint64)o.value("pid").toDouble();
  w.fullscreen = o.value("fullscreen").toBool();
  w.focused = o.value("focused").toBool();
  w.x = o.value("x").toInt();
  w.y = o.value("y").toInt();
  w.width = o.value("width").toInt();
  w.height = o.value("height").toInt();
  w.outputWidth = o.value("outputWidth").toInt();
  w.outputHeight = o.value("outputHeight").toInt();
  return w;
}

} // namespace TrophyBackend
