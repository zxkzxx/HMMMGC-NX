#pragma once

// GX FIFO worker profile (SwitchWaker patch 0020): where the worker's time goes while it translates the
// stream, for aurora_fifo_profile_get. A sampling profile: the worker stores what it is doing (a category,
// one relaxed byte store per scope) and a sampler thread counts the category it finds every 250 us, so
// the worker is barely slowed down (per-scope clock reads tripled its time on the Mac). Counts and byte
// totals are plain relaxed adds. Everything is off (no sampler thread, no stores) until
// aurora_fifo_profile_enable(true).

#include <aurora/aurora.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <thread>

namespace aurora::gx::fifo::profile {
inline std::atomic<bool> g_enabled{false};
inline std::atomic<uint8_t> g_category{AURORA_FIFO_PROFILE_IDLE};
inline std::atomic<uint64_t> g_values[AURORA_FIFO_PROFILE_COUNT]{};

inline bool enabled() noexcept { return g_enabled.load(std::memory_order_relaxed); }

inline void add(AuroraFifoProfileValue which, uint64_t value) noexcept {
  g_values[which].fetch_add(value, std::memory_order_relaxed);
}

inline void count(AuroraFifoProfileValue which, uint64_t n = 1) noexcept {
  if (enabled()) {
    add(which, n);
  }
}

// Marks the translating thread as doing `which` (a category < AURORA_FIFO_PROFILE_CATEGORIES) until the
// scope ends, then as doing what it did before.
class Scope {
public:
  explicit Scope(AuroraFifoProfileValue which) noexcept {
    if (enabled()) {
      // one writer (the translating thread): a plain load and store, no atomic read-modify-write
      m_prev = g_category.load(std::memory_order_relaxed);
      g_category.store(static_cast<uint8_t>(which), std::memory_order_relaxed);
      m_active = true;
    }
  }
  ~Scope() {
    if (m_active) {
      g_category.store(m_prev, std::memory_order_relaxed);
    }
  }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  uint8_t m_prev = 0;
  bool m_active = false;
};

inline void set_idle(bool idle) noexcept {
  if (enabled()) {
    g_category.store(idle ? AURORA_FIFO_PROFILE_IDLE : AURORA_FIFO_PROFILE_PROCESS, std::memory_order_relaxed);
  }
}

// The sampler thread, started on the first enable (never stopped: it sleeps while the profile is off).
inline void start_sampler() {
  static std::once_flag once;
  std::call_once(once, [] {
    std::thread([] {
      using namespace std::chrono;
      auto last = steady_clock::now();
      while (true) {
        std::this_thread::sleep_for(microseconds(250));
        const auto now = steady_clock::now();
        if (!enabled()) {
          last = now;
          std::this_thread::sleep_for(milliseconds(50));
          continue;
        }
        // Weighted by the real interval (the sleep is often longer than asked): ns per category.
        const auto ns = static_cast<uint64_t>(duration_cast<nanoseconds>(now - last).count());
        last = now;
        const uint8_t category = g_category.load(std::memory_order_relaxed);
        if (category < AURORA_FIFO_PROFILE_CATEGORIES) {
          add(static_cast<AuroraFifoProfileValue>(category), ns);
        }
        add(AURORA_FIFO_PROFILE_SAMPLES, 1);
      }
    }).detach();
  });
}
} // namespace aurora::gx::fifo::profile
