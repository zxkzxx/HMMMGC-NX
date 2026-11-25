#pragma once

#include "../internal.hpp"

#include <cstring>
#include <functional>

namespace aurora::gx::fifo {

namespace detail {
extern uint8_t* sBufferData;
extern uint32_t sBufferSize;
extern uint32_t sBufferCapacity;
extern bool sInDisplayList;
extern uint8_t* sDlBuffer;
extern uint32_t sDlSize;
extern uint32_t sDlWritePos;
} // namespace detail

enum class ProcessingMode : uint8_t {
  // Process FIFO synchronously at drain()
  Drain,
  // Process FIFO synchronously at publish() (useful for profiling)
  Inline,
  // Process FIFO asynchronously on a worker thread; drain() synchronizes
  Thread
};
ProcessingMode processing_mode() noexcept;

void init();
void shutdown();

void begin_frame() noexcept;
void end_frame() noexcept;

// Asynchronous end of frame (SwitchWaker patch 0018), Thread mode only, off by default. aurora::end_frame
// then does not wait for the worker to drain the frame: end_frame_async publishes it and queues `job` (the
// frame's texture, recording and present work) for the worker to run once it has processed the frame's
// commands; begin_frame_async queues the next frame's gfx::begin_frame right behind it. The game thread
// goes on with the next frame while the worker still translates this one; end_frame_async waits only
// while more than one ended frame is not yet done. Frames stay strictly ordered: the worker runs the jobs
// at their positions in the command stream.
using FrameJob = std::function<void()>;
// Between frames (game thread): waits for the queued frames first (sync).
void set_async_end_frame(bool enabled) noexcept;
bool async_end_frame() noexcept;
void end_frame_async(FrameJob job);
void begin_frame_async(FrameJob job);
// Waits until the worker has run every queued frame job (and so processed the commands before it): what
// the synchronous end of frame guaranteed (copy textures, the frame queued to the render worker). A no-op
// on the FIFO worker and on the render worker (the waits are for them; release_surface runs there too)
// and without frame jobs pending, so the synchronous path never waits here.
void sync() noexcept;
bool is_worker_thread() noexcept;

// Out-of-line slow path: grows internal buffer then appends data
void write_data_grow(const void* data, uint32_t length);

inline void write_data(const void* data, const uint32_t length) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (length <= detail::sBufferCapacity - detail::sBufferSize)
        LIKELY {
          std::memcpy(detail::sBufferData + detail::sBufferSize, data, length);
          detail::sBufferSize += length;
          return;
        }
      write_data_grow(data, length);
    }
  else if (length <= detail::sDlSize - detail::sDlWritePos) {
    std::memcpy(detail::sDlBuffer + detail::sDlWritePos, data, length);
    detail::sDlWritePos += length;
  }
}

inline void write_u8(const uint8_t val) {
  if (!detail::sInDisplayList)
    LIKELY {
      if (detail::sBufferSize < detail::sBufferCapacity)
        LIKELY {
          detail::sBufferData[detail::sBufferSize++] = val;
          return;
        }
      write_data_grow(&val, 1);
    }
  else if (detail::sDlWritePos < detail::sDlSize) {
    detail::sDlBuffer[detail::sDlWritePos++] = val;
  }
}

inline void write_u16(const uint16_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u32(const uint32_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_u64(const uint64_t val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

inline void write_f32(const float val) {
  const auto out = bswap(val);
  write_data(&out, sizeof(out));
}

// Overwrites an unpublished u32 previously written at the given offset.
void patch_u32(uint32_t offset, uint32_t val);

// Marks a complete draw and publishes when the configured draw batch is full.
void finish_draw() noexcept;

// Makes commands written so far available to the FIFO processor.
void publish() noexcept;

using DrawDoneCallback = void (*)();
DrawDoneCallback set_draw_done_callback(DrawDoneCallback callback) noexcept;

// Display list recording
void begin_display_list(uint8_t* buf, uint32_t size);
uint32_t end_display_list();
bool in_display_list();

// Ensure all buffered commands have been processed.
void drain();

// Internal buffer inspection
const uint8_t* get_buffer_data();
uint32_t get_buffer_size();
void clear_buffer();

} // namespace aurora::gx::fifo
