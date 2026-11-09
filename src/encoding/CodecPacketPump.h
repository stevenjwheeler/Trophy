/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

extern "C" {
#include <libavutil/error.h>
}

namespace TrophyBackend {

// send must retry the identical frame until accepted; receive publishes each
// successful packet before returning zero. FFmpeg forbids both directions
// returning EAGAIN without progress. Treat that as failure, never spin forever.
template <typename Send, typename Receive>
int pumpCodecPackets(Send send, Receive receive, bool eof = false) {
  int result = send();
  if (result == AVERROR(EAGAIN)) {
    bool progress = false;
    while ((result = receive()) == 0) progress = true;
    if (result != AVERROR(EAGAIN)) return result;
    if (!progress) return AVERROR_BUG;
    result = send();
    if (result == AVERROR(EAGAIN)) return AVERROR_BUG;
  }
  if (result < 0 && !(eof && result == AVERROR_EOF)) return result;
  while ((result = receive()) == 0) {}
  if (eof) return result == AVERROR_EOF ? 0 :
                   (result == AVERROR(EAGAIN) ? AVERROR_BUG : result);
  return result == AVERROR(EAGAIN) ? 0 : result;
}

} // namespace TrophyBackend
