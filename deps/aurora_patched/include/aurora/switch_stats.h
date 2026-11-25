#pragma once

/* Frame timing counters of the Switch build (switch/native/aurora/patches): where the game thread
 * waits in aurora_begin_frame, what the render worker spends its time on, and pipeline creation.
 * Every field is a running total since start; callers diff two reads. */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint64_t frameSlotWaitNs;   /* aurora_begin_frame: waiting for a free frame slot */
  uint64_t stagingWaitNs;     /* aurora_begin_frame: waiting for a mapped staging buffer */
  uint64_t queueFullWaitNs;   /* any producer: waiting for room in the render worker's queue */
  uint64_t workerBusyNs;      /* render worker: running queue items */
  uint64_t workerEncodeNs;    /* render worker: EncodePass items (GX passes into the encoder) */
  uint64_t workerEndFrameNs;  /* render worker: EndFrame items (unmap, present pass, submit, present, remap) */
  uint64_t workerUnmapNs;     /* EndFrame: staging buffer Unmap */
  uint64_t workerAcquireNs;   /* EndFrame: surface GetCurrentTexture */
  uint64_t workerSubmitNs;    /* EndFrame: Queue::Submit of the frame's command buffer */
  uint64_t workerPresentNs;   /* EndFrame: Surface::Present */
  uint64_t workerEventsNs;    /* render worker: Instance::ProcessEvents */
  uint64_t workerFrames;      /* EndFrame items run */
  uint64_t pipelineCompiles;  /* pipelines created (compile thread or inline) */
  uint64_t pipelineCompileNs; /* time spent creating them */
  uint64_t pipelineCompileMaxNs; /* longest single creation so far */
} AuroraSwitchStats;

void aurora_switch_get_stats(AuroraSwitchStats* out);

#ifdef __cplusplus
}

#include <atomic>
#include <chrono>

namespace aurora::switch_stats {
struct Counters {
  std::atomic<uint64_t> frameSlotWaitNs{0}, stagingWaitNs{0}, queueFullWaitNs{0}, workerBusyNs{0},
      workerEncodeNs{0}, workerEndFrameNs{0}, workerUnmapNs{0}, workerAcquireNs{0}, workerSubmitNs{0},
      workerPresentNs{0}, workerEventsNs{0}, workerFrames{0}, pipelineCompiles{0}, pipelineCompileNs{0},
      pipelineCompileMaxNs{0};
};
Counters& counters() noexcept;

inline uint64_t now_ns() noexcept {
  return static_cast<uint64_t>(
      std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
          .count());
}

// Adds the time from construction to destruction to a counter.
class Scope {
public:
  explicit Scope(std::atomic<uint64_t>& counter) noexcept : m_counter(counter), m_start(now_ns()) {}
  ~Scope() { m_counter.fetch_add(now_ns() - m_start, std::memory_order_relaxed); }
  Scope(const Scope&) = delete;
  Scope& operator=(const Scope&) = delete;

private:
  std::atomic<uint64_t>& m_counter;
  uint64_t m_start;
};

inline void add_pipeline_compile(uint64_t ns) noexcept {
  auto& c = counters();
  c.pipelineCompiles.fetch_add(1, std::memory_order_relaxed);
  c.pipelineCompileNs.fetch_add(ns, std::memory_order_relaxed);
  uint64_t max = c.pipelineCompileMaxNs.load(std::memory_order_relaxed);
  while (ns > max && !c.pipelineCompileMaxNs.compare_exchange_weak(max, ns, std::memory_order_relaxed)) {
  }
}
} // namespace aurora::switch_stats
#endif
