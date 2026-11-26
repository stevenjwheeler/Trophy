/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once
#include <QString>
#include <cerrno>
#include <cstring>

namespace TrophyBackend::detail {
// PipeWire loop functions return negative errno values, not a separate errno.
inline bool audioDiscoveryLoopInterrupted(int result) {
  return result == -EINTR;
}
inline QString audioDiscoveryFailure(const char *stage, int result) {
  return QStringLiteral("%1: %2 (code %3)")
      .arg(QString::fromLatin1(stage),
           result < 0 ? QString::fromLocal8Bit(strerror(-result))
                      : QStringLiteral("no native error reported"))
      .arg(result);
}
} // namespace TrophyBackend::detail
