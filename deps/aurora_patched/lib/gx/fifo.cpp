#include "fifo.hpp"

#include "../thread.hpp"
#include "../gfx/render_worker.hpp"
#include "command_processor.hpp"
#include "fifo_profile.hpp"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <limits>
#include <mutex>
#include <thread>

#include <tracy/Tracy.hpp>

namespace aurora::gx::fifo {
namespace detail {
uint8_t* sBufferData = nullptr;
uint32_t sBufferSize = 0;
uint32_t sBufferCapacity = 0;
bool sInDisplayList = false;
uint8_t* sDlBuffer = nullptr;
uint32_t sDlSize = 0;
uint32_t sDlWritePos = 0;
} // namespace detail

namespace {
constexpr Module Log{"aurora::gx::fifo"};
constexpr auto kProcessingMode = ProcessingMode::Thread;
constexpr uint32_t kDrawBatchSize = 16;

bool sFrameActive = false;
uint32_t sPendingDraws = 0;
std::atomic<uint64_t> sPublished{0};
std::atomic<uint64_t> sProcessed{0};
uint64_t sStreamBase = 0;
std::mutex sBufferMutex;
std::atomic<uint32_t> sWorkerWake{0};
thread::Thread sWorkerThread;
std::atomic<DrawDoneCallback> sDrawDoneCallback{nullptr};

// Async end of frame (patch 0018). Frame jobs run on the worker once it has processed the command stream
// up to their position (stream offsets, as sPublished/sProcessed). The game thread queues them and is the
// only writer of the stream; the counters let it wait for them.
struct Job {
  uint64_t pos;
  FrameJob fn;
  bool frameEnd;
};
// More ended frames than this not yet done make end_frame_async wait: the game thread runs at most one
// frame ahead of the worker.
constexpr uint64_t kMaxFramesInFlight = 1;
// A command buffer this large at an async end of frame is drained (reset): it is otherwise only reset when
// the worker has caught up (a drain, GXDrawDone, or an end of frame finding it idle).
constexpr uint32_t kAsyncBufferLimit = 64u << 20;
std::atomic<bool> sAsync{false};
std::mutex sJobMutex;
std::deque<Job> sJobs;
std::atomic<uint64_t> sJobsQueued{0};
std::atomic<uint64_t> sJobsDone{0};
std::atomic<uint64_t> sFramesEnded{0};
std::atomic<uint64_t> sFramesDone{0};
std::atomic<std::thread::id> sWorkerId{};

void dispatch_draw_done() noexcept {
  if (const auto callback = sDrawDoneCallback.load(std::memory_order_acquire); callback != nullptr) {
    callback();
  }
}

void wake_worker() noexcept {
  sWorkerWake.fetch_add(1, std::memory_order_release);
  sWorkerWake.notify_all();
}

void process_to(uint64_t target, std::memory_order order) noexcept {
  uint64_t processed = sProcessed.load(std::memory_order_relaxed);
  while (processed < target) {
    ProcessResult result{};
    {
      std::lock_guard lock{sBufferMutex};
      AURORA_ASSERT(processed >= sStreamBase && target <= sStreamBase + detail::sBufferSize,
                    "FIFO processing range [{}, {}) is outside buffered range [{}, {})", processed, target, sStreamBase,
                    sStreamBase + detail::sBufferSize);
      const auto start = static_cast<uint32_t>(processed - sStreamBase);
      const auto size = static_cast<uint32_t>(target - processed);
      profile::Scope prof{AURORA_FIFO_PROFILE_PROCESS};
      result = process(detail::sBufferData + start, size);
    }
    AURORA_ASSERT(result.bytesProcessed > 0 && result.bytesProcessed <= target - processed,
                  "FIFO processor made invalid progress: processed {} of {} remaining bytes", result.bytesProcessed,
                  target - processed);
    if (result.drawDone) {
      dispatch_draw_done();
    }
    processed += result.bytesProcessed;
    profile::count(AURORA_FIFO_PROFILE_STREAM_BYTES, result.bytesProcessed);
    sProcessed.store(processed, order);
    sProcessed.notify_all();
  }
}

// Runs the first frame job if the stream has been processed up to its position.
bool run_due_job(uint64_t processed) noexcept {
  Job job;
  {
    std::lock_guard lock{sJobMutex};
    if (sJobs.empty() || sJobs.front().pos > processed) {
      return false;
    }
    job = std::move(sJobs.front());
    sJobs.pop_front();
  }
  if (job.fn) {
    profile::Scope prof{AURORA_FIFO_PROFILE_JOB};
    job.fn();
  }
  if (job.frameEnd) {
    sFramesDone.fetch_add(1, std::memory_order_release);
    sFramesDone.notify_all();
  }
  sJobsDone.fetch_add(1, std::memory_order_release);
  sJobsDone.notify_all();
  return true;
}

uint64_t next_job_pos() noexcept {
  std::lock_guard lock{sJobMutex};
  return sJobs.empty() ? std::numeric_limits<uint64_t>::max() : sJobs.front().pos;
}

void queue_job(uint64_t pos, FrameJob fn, bool frameEnd) {
  {
    std::lock_guard lock{sJobMutex};
    sJobs.push_back(Job{.pos = pos, .fn = std::move(fn), .frameEnd = frameEnd});
  }
  sJobsQueued.fetch_add(1, std::memory_order_release);
}

void wait_at_least(std::atomic<uint64_t>& counter, uint64_t target) noexcept {
  uint64_t value = counter.load(std::memory_order_acquire);
  while (value < target) {
    counter.wait(value, std::memory_order_acquire);
    value = counter.load(std::memory_order_acquire);
  }
}

// Game thread: the buffer starts over once the worker has processed all of it. Never waits: the worker
// holds sBufferMutex while it translates a published range (a whole frame when it is behind), and has
// released it by the time sProcessed reaches the end.
void reset_buffer_if_processed() noexcept {
  const uint64_t end = sStreamBase + detail::sBufferSize;
  if (sProcessed.load(std::memory_order_acquire) != end) {
    return;
  }
  std::unique_lock lock{sBufferMutex, std::try_to_lock};
  if (lock.owns_lock()) {
    sStreamBase = end;
    detail::sBufferSize = 0;
  }
}

void worker_main(std::stop_token token) noexcept {
  std::stop_callback wakeOnStop{token, wake_worker};
  sWorkerId.store(std::this_thread::get_id(), std::memory_order_release);
  while (true) {
    const uint32_t event = sWorkerWake.load(std::memory_order_acquire);
    // sPublished before the jobs: the game thread queues a frame's begin job before it publishes any of
    // the frame's commands, so commands seen here never overtake a job not yet seen.
    const uint64_t published = sPublished.load(std::memory_order_acquire);
    const uint64_t processed = sProcessed.load(std::memory_order_relaxed);
    if (run_due_job(processed)) {
      continue;
    }
    const uint64_t limit = std::min(published, next_job_pos());
    if (limit > processed) {
      process_to(limit, std::memory_order_release);
      continue;
    }

    if (token.stop_requested()) {
      break;
    }
    profile::set_idle(true);
    sWorkerWake.wait(event, std::memory_order_acquire);
    profile::set_idle(false);
  }
}

void start_worker() {
  if (kProcessingMode != ProcessingMode::Thread || sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread = thread::Thread{{
                                     .name = "Aurora FIFO processor",
                                     .affinity = thread::Affinity::SharedCache,
                                 },
                                 worker_main};
}

void stop_worker() {
  if (!sWorkerThread.joinable()) {
    return;
  }
  sWorkerThread.request_stop();
  sWorkerThread.join();
}
} // namespace

ProcessingMode processing_mode() noexcept { return kProcessingMode; }

void init() {
  stop_worker();

  constexpr uint32_t initialCapacity = 64 * 1024;
  free(detail::sBufferData);
  detail::sBufferData = static_cast<uint8_t*>(malloc(initialCapacity));
  AURORA_ASSERT(detail::sBufferData != nullptr, "fifo::init: failed to allocate {} bytes", initialCapacity);
  detail::sBufferSize = 0;
  detail::sBufferCapacity = initialCapacity;
  detail::sInDisplayList = false;
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;

  sFrameActive = false;
  sPendingDraws = 0;
  sStreamBase = 0;
  sPublished.store(0, std::memory_order_relaxed);
  sProcessed.store(0, std::memory_order_relaxed);
  sWorkerWake.store(0, std::memory_order_relaxed);

  start_worker();
}

void shutdown() {
  sync();
  stop_worker();
  sAsync.store(false, std::memory_order_relaxed);
}

void begin_frame() noexcept { sFrameActive = true; }

void end_frame() noexcept {
  sFrameActive = false;
  clear_draw_cache(); // command_processor
}

bool is_worker_thread() noexcept { return sWorkerId.load(std::memory_order_acquire) == std::this_thread::get_id(); }

void set_async_end_frame(bool enabled) noexcept {
  enabled = enabled && kProcessingMode == ProcessingMode::Thread;
  if (enabled == sAsync.load(std::memory_order_relaxed)) {
    return;
  }
  sync();
  sAsync.store(enabled, std::memory_order_release);
  Log.info("Async end of frame {}", enabled ? "on" : "off");
}

bool async_end_frame() noexcept { return sAsync.load(std::memory_order_relaxed); }

void end_frame_async(FrameJob job) {
  ZoneScoped;
  const uint64_t target = sStreamBase + detail::sBufferSize;
  queue_job(
      target,
      [job = std::move(job)] {
        clear_draw_cache(); // command_processor (end_frame's, on the worker)
        job();
      },
      true);
  const uint64_t ended = sFramesEnded.fetch_add(1, std::memory_order_relaxed) + 1;
  if (target > sPublished.load(std::memory_order_relaxed)) {
    sPublished.store(target, std::memory_order_release);
  }
  sPendingDraws = 0;
  sFrameActive = false;
  wake_worker();
  if (ended > kMaxFramesInFlight) {
    ZoneScopedN("Wait for frames in flight");
    wait_at_least(sFramesDone, ended - kMaxFramesInFlight);
  }
  reset_buffer_if_processed();
  if (detail::sBufferSize > kAsyncBufferLimit) {
    drain();
  }
}

void begin_frame_async(FrameJob job) {
  // At the stream's published end: commands written since the end of the last frame (outside a frame,
  // unpublished) are processed after it, in this frame, as in the synchronous path.
  queue_job(sPublished.load(std::memory_order_relaxed), std::move(job), false);
  wake_worker();
}

void sync() noexcept {
  if (!sWorkerThread.joinable() || is_worker_thread() || gfx::render_worker::is_worker_thread()) {
    return;
  }
  const uint64_t queued = sJobsQueued.load(std::memory_order_acquire);
  if (sJobsDone.load(std::memory_order_acquire) >= queued) {
    return;
  }
  ZoneScoped;
  wait_at_least(sJobsDone, queued);
}

void write_data_grow(const void* data, uint32_t length) {
  const uint64_t needed64 = static_cast<uint64_t>(detail::sBufferSize) + length;
  AURORA_ASSERT(needed64 <= std::numeric_limits<uint32_t>::max(), "fifo::write_data: buffer size overflow");
  const auto needed = static_cast<uint32_t>(needed64);
  const auto doubledCapacity = static_cast<uint64_t>(detail::sBufferCapacity) * 2;
  const auto newCapacity = static_cast<uint32_t>(
      std::min<uint64_t>(std::max(doubledCapacity, needed64), std::numeric_limits<uint32_t>::max()));
  const auto grow = [newCapacity] {
    auto* resized = static_cast<uint8_t*>(realloc(detail::sBufferData, newCapacity));
    AURORA_ASSERT(resized != nullptr, "fifo::write_data: failed to allocate {} bytes", newCapacity);
    detail::sBufferData = resized;
  };
  if (sWorkerThread.joinable()) {
    std::lock_guard lock{sBufferMutex};
    grow();
  } else {
    grow();
  }
  std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
  detail::sBufferSize = needed;
  detail::sBufferCapacity = newCapacity;
}

void publish() noexcept {
  if (!sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }

  const uint64_t target = sStreamBase + detail::sBufferSize;
  if (target > sPublished.load(std::memory_order_relaxed)) {
    sPendingDraws = 0;
    sPublished.store(target, std::memory_order_release);
    if (kProcessingMode == ProcessingMode::Thread) {
      wake_worker();
    } else {
      process_to(target, std::memory_order_relaxed);
    }
  }
}

DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept {
  return sDrawDoneCallback.exchange(callback, std::memory_order_acq_rel);
}

void finish_draw() noexcept {
  if (!sFrameActive || kProcessingMode == ProcessingMode::Drain || detail::sInDisplayList) {
    return;
  }
  if (++sPendingDraws >= kDrawBatchSize) {
    publish();
  }
}

void patch_u32(uint32_t offset, uint32_t val) {
  AURORA_ASSERT(!detail::sInDisplayList && offset <= detail::sBufferSize &&
                    sizeof(uint32_t) <= detail::sBufferSize - offset,
                "fifo::patch_u32: invalid patch offset {} (buffer size {})", offset, detail::sBufferSize);
  AURORA_ASSERT(sStreamBase + offset >= sPublished.load(std::memory_order_relaxed),
                "fifo::patch_u32: offset {} is below the published watermark", offset);
  const auto out = bswap(val);
  std::memcpy(detail::sBufferData + offset, &out, sizeof(out));
}

void begin_display_list(uint8_t* buf, uint32_t size) {
  detail::sInDisplayList = true;
  detail::sDlBuffer = buf;
  detail::sDlSize = size;
  detail::sDlWritePos = 0;
}

uint32_t end_display_list() {
  detail::sInDisplayList = false;
  const uint32_t bytesWritten = detail::sDlWritePos;
  const uint32_t padded = (bytesWritten + 31) & ~31u;
  while (detail::sDlWritePos < padded && detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = 0;
  }
  detail::sDlBuffer = nullptr;
  detail::sDlSize = 0;
  detail::sDlWritePos = 0;
  return padded;
}

bool in_display_list() { return detail::sInDisplayList; }

void drain() {
  if (detail::sBufferSize == 0) {
    return;
  }

  ZoneScoped;
  const uint64_t target = sStreamBase + detail::sBufferSize;

  switch (kProcessingMode) {
  case ProcessingMode::Drain:
  case ProcessingMode::Inline:
    sPublished.store(target, std::memory_order_relaxed);
    process_to(target, std::memory_order_relaxed);
    break;
  case ProcessingMode::Thread: {
    sPublished.store(target, std::memory_order_release);
    wake_worker();

    uint64_t processed = sProcessed.load(std::memory_order_acquire);
    if (processed < target) {
      do {
        sProcessed.wait(processed, std::memory_order_acquire);
        processed = sProcessed.load(std::memory_order_acquire);
      } while (processed < target);
    }
    break;
  }
  }

  {
    std::lock_guard lock{sBufferMutex};
    sStreamBase = target;
    detail::sBufferSize = 0;
  }
  sPendingDraws = 0;
}

const uint8_t* get_buffer_data() { return detail::sBufferData; }
uint32_t get_buffer_size() { return detail::sBufferSize; }

void clear_buffer() {
  const uint64_t processed = sProcessed.load(std::memory_order_acquire);
  AURORA_ASSERT(sPublished.load(std::memory_order_acquire) == processed,
                "fifo::clear_buffer: published commands are still pending");
  std::lock_guard lock{sBufferMutex};
  sStreamBase = processed;
  detail::sBufferSize = 0;
  sPendingDraws = 0;
}

} // namespace aurora::gx::fifo
