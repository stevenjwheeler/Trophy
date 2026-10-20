/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QJsonObject>
#include <QList>

namespace TrophyBackend {
// File-wide, conservative history. Mixed captures retain every possibly
// useful lane; this is routing evidence, never silence-based guessing.
struct AudioRoutingInfo {
  bool known = false, system = false, game = false, voice = false, separate = false;
  int primary = -1, voiceTrack = -1, micTrack = -1; // Audio ordinals, not mixer slots.
  QJsonObject toJson() const {
    if (!known) return {};
    return {{"v", 1}, {"system", system}, {"game", game}, {"voice", voice},
            {"separate", separate}, {"primary", primary}, {"voiceTrack", voiceTrack}, {"micTrack", micTrack}};
  }
  static AudioRoutingInfo fromJson(const QJsonObject &object) {
    AudioRoutingInfo result;
    if (object.value("v").toInt() != 1) return result;
    for (const auto *key : {"system", "game", "voice", "separate"})
      if (!object.value(key).isBool()) return result;
    for (const auto *key : {"primary", "voiceTrack", "micTrack"}) {
      const auto value = object.value(key);
      if (!value.isDouble() || value.toDouble() != value.toInt(-2) || value.toInt() < -1 || value.toInt() > 31) return result;
    }
    result.system = object.value("system").toBool(); result.game = object.value("game").toBool();
    result.voice = object.value("voice").toBool(); result.separate = object.value("separate").toBool();
    result.primary = object.value("primary").toInt(); result.voiceTrack = object.value("voiceTrack").toInt();
    result.micTrack = object.value("micTrack").toInt();
    if ((!result.system && !result.game) ||
        (result.primary < 0 && result.voiceTrack < 0 && result.micTrack < 0) ||
        (!result.separate && (result.voiceTrack >= 0 || result.micTrack >= 0)) ||
        (result.primary >= 0 && (result.primary == result.voiceTrack || result.primary == result.micTrack)) ||
        (result.voiceTrack >= 0 && result.voiceTrack == result.micTrack)) return {};
    result.known = true;
    return result;
  }
  static QJsonObject combine(const QList<QJsonObject> &histories) {
    AudioRoutingInfo result;
    for (const auto &object : histories) {
      const auto next = fromJson(object);
      if (!next.known) return {}; // Unknown input must not disappear in a union.
      if (!result.known) { result = next; continue; }
      if (result.separate != next.separate || result.primary != next.primary ||
          result.voiceTrack != next.voiceTrack || result.micTrack != next.micTrack) return {};
      result.system |= next.system; result.game |= next.game; result.voice |= next.voice;
    }
    return result.toJson();
  }
};
}
