/*
 * Trophy - Game recording and clipping
 * Copyright (C) 2026
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "audio/AudioDriftCorrector.h"

#include <algorithm>
#include <algorithm>
#include <cmath>

extern "C" {
#include <libswresample/swresample.h>
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
}

namespace TrophyBackend {

AudioDriftCorrector::AudioDriftCorrector(int sampleRate, int channels,
                                         Clock clock)
    : m_sampleRate(sampleRate > 0 ? sampleRate : 48000),
      m_channels(channels > 0 ? channels : 2), m_clock(std::move(clock)) {
  if (!m_clock) {
    m_clock = [] { return steadyNowUs(); };
  }

  AVChannelLayout layout;
  av_channel_layout_default(&layout, m_channels);
  // Same rate in and out: the resampler acts purely as a fractional
  // rate servo driven by swr_set_compensation below.
  if (swr_alloc_set_opts2(&m_swr, &layout, AV_SAMPLE_FMT_FLT,
                           m_sampleRate, &layout, AV_SAMPLE_FMT_FLT,
                           m_sampleRate, 0, nullptr) < 0 ||
      !m_swr || swr_init(m_swr) < 0) {
    swr_free(&m_swr);
    m_swr = nullptr;
  }
  av_channel_layout_uninit(&layout);
}

AudioDriftCorrector::~AudioDriftCorrector() { swr_free(&m_swr); }

void AudioDriftCorrector::reset() {
  m_startUs = -1;
  m_lastUs = -1;
  m_totalOut = 0;
  m_errorSmooth = 0;
  if (m_swr) {
    swr_close(m_swr);
    if (swr_init(m_swr) < 0) swr_free(&m_swr);
  }
}

int64_t AudioDriftCorrector::steadyNowUs() {
  return std::chrono::duration_cast<std::chrono::microseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

int AudioDriftCorrector::process(const float *samples, int frameCount,
                                 const float *&out) {
  out = samples;
  if (!samples || frameCount <= 0) {
    return 0;
  }
  if (!m_swr) {
    return frameCount; // pass-through: SWR unavailable, out == samples
  }

  const int64_t nowUs = m_clock();
  if (m_startUs < 0) {
    // Prime to our own arrival; startup offset vs video is handled by
    // the session (audio gate + encoder start offsets), not here.
    m_startUs = nowUs;
    m_totalOut = 0;
    m_errorSmooth = 0.0;
  } else if (m_lastUs >= 0 && nowUs - m_lastUs > kResetGapUs) {
    // Resuming after a long silence (mute gap, source rebind):
    // re-baseline instead of "correcting" the gap away.
    m_startUs = nowUs;
    m_totalOut = 0;
    m_errorSmooth = 0.0;
  }

  // Absolute bookkeeping (no integrator windup): outputs produced so
  // far vs outputs the wall clock demands. Positive error = ahead. The
  // decision uses a low-passed copy: raw callback arrival time jitters
  // with scheduler quanta, and the servo must track drift, not noise.
  const double expectedTotal =
      (static_cast<double>(nowUs - m_startUs) * m_sampleRate) / 1000000.0;
  const double error = static_cast<double>(m_totalOut) - expectedTotal;
  m_errorSmooth += (error - m_errorSmooth) * 0.1;

  int delta = 0;
  if (std::fabs(m_errorSmooth) > kDeadbandSamples) {
    const double want = -m_errorSmooth * 0.02;
    const double cap = frameCount * kMaxRateShift;
    delta = static_cast<int>(std::clamp(want, -cap, cap));
    if (delta == 0)
      delta = (m_errorSmooth > 0) ? -1 : 1;
  }
  if (swr_set_compensation(m_swr, delta, frameCount) < 0) {
    out = samples;
    m_lastUs = nowUs;
    return frameCount;
  }
  const int need =
      frameCount + 64 + static_cast<int>(swr_get_delay(m_swr, m_sampleRate));
  if (static_cast<int>(m_out.size()) < need * m_channels)
    m_out.resize(static_cast<size_t>(need) * static_cast<size_t>(m_channels));

  const uint8_t *inPtr = reinterpret_cast<const uint8_t *>(samples);
  uint8_t *outPtr = reinterpret_cast<uint8_t *>(m_out.data());
  const int got = swr_convert(m_swr, &outPtr, need, &inPtr, frameCount);
  if (got < 0) {
    out = samples;
    m_lastUs = nowUs;
    return frameCount;
  }

  m_totalOut += got;
  m_lastUs = nowUs;
  out = m_out.data();
  return got;
}

} // namespace TrophyBackend
