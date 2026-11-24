/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#include <gtest/gtest.h>
#include <vector>
#include "capture/PipeWireBufferLease.h"

// A real local PipeWire event loop, no daemon, portal, screen or GPU.
// Only stream queue/update calls are replaced with counters. Buffer retirement,
// eventfd signalling and the production lease callbacks are exercised normally.
struct PipeWireLeaseFixture {
  static inline thread_local PipeWireLeaseFixture *active = nullptr;
  pw_loop *loop = nullptr;
  std::shared_ptr<TrophyBackend::PipeWireBufferLease> leases;
  std::vector<pw_buffer *> returned;
  int fallbacks = 0;

  PipeWireLeaseFixture() {
    pw_init(nullptr, nullptr);
    loop = pw_loop_new(nullptr);
    if (!loop) return;
    pw_loop_enter(loop);
    active = this;
    leases = std::make_shared<TrophyBackend::PipeWireBufferLease>(
        TrophyBackend::PipeWireBufferLease::Ops{queue, update});
    if (!leases->initialize(loop, nullptr)) leases.reset();
  }
  ~PipeWireLeaseFixture() {
    if (leases) leases->deactivate();
    leases.reset();
    if (loop) {
      pw_loop_leave(loop);
      pw_loop_destroy(loop);
    }
    active = nullptr;
    pw_deinit();
  }
  void drain() { for (int i = 0; i < 4; ++i) pw_loop_iterate(loop, 0); }

private:
  static int queue(pw_stream *, pw_buffer *buffer) {
    EXPECT_NE(active, nullptr);
    if (active) active->returned.push_back(buffer);
    return 0;
  }
  static int update(pw_stream *, const spa_pod **params, uint32_t count) {
    EXPECT_NE(active, nullptr);
    EXPECT_EQ(count, 1u);
    if (active) ++active->fallbacks;
    EXPECT_FALSE(TrophyBackend::pipeWireVideoHasModifier(params[0]));
    return 0;
  }
};
