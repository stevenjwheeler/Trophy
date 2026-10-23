/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <QString>

namespace TrophyBackend::detail {

// Voice selections are saved process binaries, not fuzzy game-name hints.
inline bool matchesAudioBinary(const QString &binary, const QString &expected) {
  return !expected.isEmpty() &&
         binary.compare(expected, Qt::CaseInsensitive) == 0;
}

struct ApplicationAudioIdentity {
  QString client, app, serial, binary, pid;
  bool playback = false;
  bool current = false;
};

inline bool verifiedAudioTarget(const ApplicationAudioIdentity &target,
                                const QString &serial,
                                const QString &expectedBinary) {
  return target.current && target.playback && !serial.isEmpty() &&
         target.serial == serial &&
         (expectedBinary.isEmpty() ||
          matchesAudioBinary(target.binary, expectedBinary));
}

inline bool verifiedAudioSibling(const ApplicationAudioIdentity &target,
                                 const ApplicationAudioIdentity &sibling,
                                 const QString &expectedBinary) {
  return sibling.current && sibling.playback && !target.client.isEmpty() &&
         !target.app.isEmpty() && sibling.client == target.client &&
         sibling.app == target.app &&
         (target.binary.isEmpty() || sibling.binary.isEmpty() ||
          target.binary.compare(sibling.binary, Qt::CaseInsensitive) == 0) &&
         (target.pid.isEmpty() || sibling.pid.isEmpty() || target.pid == sibling.pid) &&
         (expectedBinary.isEmpty() ||
          matchesAudioBinary(sibling.binary, expectedBinary));
}
} // namespace TrophyBackend::detail
