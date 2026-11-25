#pragma once

#include "gx.hpp"

#include <array>
#include <bit>
#include <cstring>
#include <span>

namespace aurora::gx::fifo {

constexpr u32 reg_get(const u32 reg, const u32 size, const u32 shift) noexcept {
  return reg >> shift & (1u << size) - 1;
}

void handle_bp(u32 value) noexcept;

// SwitchWaker patch 0021: true when handle_bp(value) would return at once (the register already holds
// the value, no BP mask pending, no side effects): most BP loads in a frame (display lists send their
// materials' whole state), skipped by the command processor without the call.
extern const std::array<bool, 0x100> g_bpAlwaysHandle;
inline bool bp_redundant(u32 value) noexcept {
  const u32 regId = value >> 24;
  const auto& state = g_gxState;
  return regId != 0xFE && state.bpRegCache[0xFE] == 0x00FFFFFF && state.bpRegValid.test(regId) &&
         state.bpRegCache[regId] == value && !g_bpAlwaysHandle[regId];
}
void handle_cp(u8 addr, u32 value) noexcept;
void handle_xf(u16 addr, std::span<const u8> data) noexcept;

bool copy_xf_data(u32 addr, const u8* data, u32 len, std::endian e) noexcept;

} // namespace aurora::gx::fifo
