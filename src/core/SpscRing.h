#pragma once

#include <atomic>
#include <cstddef>
#include <vector>
#include <optional>

// Single-producer single-consumer bounded ring buffer. Lock-free for SPSC.
template <typename T>
class SpscRing {
public:
  explicit SpscRing(size_t capacity)
      : m_capacity(nextPowerOfTwo(capacity)), m_mask(m_capacity - 1), m_buf(m_capacity) {}

  bool try_push(T &&item) {
    size_t head = m_head.load(std::memory_order_relaxed);
    size_t next = head + 1;
    if (next - m_tail.load(std::memory_order_acquire) > m_capacity)
      return false; // full
    m_buf[head & m_mask] = std::move(item);
    m_head.store(next, std::memory_order_release);
    return true;
  }

  bool try_pop(T &out) {
    size_t tail = m_tail.load(std::memory_order_relaxed);
    if (tail == m_head.load(std::memory_order_acquire))
      return false; // empty
    out = std::move(m_buf[tail & m_mask]);
    m_tail.store(tail + 1, std::memory_order_release);
    return true;
  }

  // Call only when stopped, with no concurrent push/pop in flight.
  // (Kept deliberately for future reuse; not currently called.)
  void reset() {
    m_head.store(0);
    m_tail.store(0);
  }

private:
  static size_t nextPowerOfTwo(size_t v) {
    size_t n = 1;
    while (n < v)
      n <<= 1;
    return n;
  }

  const size_t m_capacity;
  const size_t m_mask;
  std::vector<T> m_buf;
  std::atomic<size_t> m_head{0};
  std::atomic<size_t> m_tail{0};
};
