/* SPDX-License-Identifier: GPL-3.0-or-later */
#pragma once

#ifdef HAVE_EGL
#include <EGL/egl.h>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <utility>

namespace TrophyBackend {

// EGL returns the same display handle for the same native display/device.
// Repeated eglInitialize calls are NOT reference-counted by EGL. All Trophy
// users must therefore share ownership, including short capability queries.
class EglDisplayRegistry {
  using Initialize = std::function<EGLBoolean(EGLDisplay, EGLint *, EGLint *)>;
  using Terminate = std::function<EGLBoolean(EGLDisplay)>;
  struct State {
    State(Initialize init, Terminate term)
        : initialize(std::move(init)), terminate(std::move(term)) {}
    std::mutex mutex;
    std::unordered_map<EGLDisplay, size_t> owners;
    Initialize initialize;
    Terminate terminate;
  };

public:
  class Lease {
  public:
    Lease() = default;
    ~Lease() { reset(); }
    Lease(const Lease &) = delete;
    Lease &operator=(const Lease &) = delete;
    Lease(Lease &&other) noexcept
        : m_state(std::move(other.m_state)),
          m_display(std::exchange(other.m_display, EGL_NO_DISPLAY)) {}
    Lease &operator=(Lease &&other) noexcept {
      if (this != &other) {
        reset();
        m_state = std::move(other.m_state);
        m_display = std::exchange(other.m_display, EGL_NO_DISPLAY);
      }
      return *this;
    }
    EGLDisplay get() const { return m_display; }
    explicit operator bool() const { return m_display != EGL_NO_DISPLAY; }

    // Call only after this owner's images/contexts and GPU consumers settle.
    // A lease may move between threads; the native display is not made current.
    void reset() noexcept {
      if (!m_state) return;
      auto state = std::move(m_state);
      const auto display = std::exchange(m_display, EGL_NO_DISPLAY);
      std::lock_guard lock(state->mutex);
      const auto found = state->owners.find(display);
      if (found != state->owners.end() && --found->second == 0) {
        // Keep the lock through termination: otherwise a new acquisition can
        // initialize the handle just before this retirement terminates it.
        state->terminate(display);
        state->owners.erase(found);
      }
    }

  private:
    friend class EglDisplayRegistry;
    Lease(std::shared_ptr<State> state, EGLDisplay display)
        : m_state(std::move(state)), m_display(display) {}
    std::shared_ptr<State> m_state;
    EGLDisplay m_display = EGL_NO_DISPLAY;
  };

  explicit EglDisplayRegistry(Initialize initialize = eglInitialize,
                              Terminate terminate = eglTerminate)
      : m_state(std::make_shared<State>(std::move(initialize), std::move(terminate))) {}

  Lease acquire(EGLDisplay display) const {
    if (display == EGL_NO_DISPLAY) return {};
    std::lock_guard lock(m_state->mutex);
    // Reserve the entry before the native call, so allocation failure cannot
    // leave a successfully initialized display without a tracked owner.
    auto [entry, inserted] = m_state->owners.try_emplace(display, 0);
    if (inserted && m_state->initialize(display, nullptr, nullptr) != EGL_TRUE) {
      m_state->owners.erase(entry);
      return {};
    }
    ++entry->second;
    return Lease(m_state, display);
  }

private:
  // Leases retain state even if a registry wrapper is destroyed first.
  std::shared_ptr<State> m_state;
};

using EglDisplayLease = EglDisplayRegistry::Lease;

inline EglDisplayLease acquireEglDisplay(EGLDisplay display) {
  static EglDisplayRegistry registry;
  return registry.acquire(display);
}

} // namespace TrophyBackend
#endif
