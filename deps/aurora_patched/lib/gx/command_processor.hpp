#pragma once

#include "../internal.hpp"

#include <cstdint>

namespace aurora::gx::fifo {

struct ProcessResult {
  uint32_t bytesProcessed;
  bool drawDone;
};

// Process GX FIFO commands until the next draw done event or end of buffer
ProcessResult process(const uint8_t* data, uint32_t size) noexcept;
void clear_draw_cache() noexcept;
// SwitchWaker patch 0021: the translation's fast paths (on by default), switchable for A/B checks.
void set_fast_paths(bool enabled) noexcept;

} // namespace aurora::gx::fifo
