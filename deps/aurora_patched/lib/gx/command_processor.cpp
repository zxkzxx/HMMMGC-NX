#include "command_processor.hpp"

#include "../gfx/census.hpp"
#include "../gfx/depth_peek.hpp"
#include "../gfx/hash.hpp"
#include "../gfx/recording.hpp"
#include "../internal.hpp"
#include "dolphin/gd/GDGeometry.h"
#include "dolphin/gx/GXAurora.h"
#include "fifo_profile.hpp"
#include "gx.hpp"
#include "pipeline.hpp"
#include "regs.hpp"
#include "shader_info.hpp"
#include "texture.hpp"

#include <tracy/Tracy.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <span>
#include <unordered_map>
#include <vector>

namespace aurora::gx::fifo {
namespace {
constexpr Module Log{"aurora::gx::fifo"};

u16 prepare_idx_buffer(ByteBuffer& buf, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  u16 numIndices = 0;
  if (prim == GX_QUADS) {
    buf.reserve_extra(((vtxCount / 4) * 6 + 3) * sizeof(u16));

    // Only whole quads; the hardware draws three vertices left over as a triangle (the game
    // draws its stars and sun rays as GX_QUADS of 3 vertices; Dolphin's IndexGenerator AddQuads
    // does the same) and ignores one or two
    u16 v = 0;
    for (; v + 4 <= vtxCount; v += 4) {
      u16 idx0 = vtxStart + v;
      u16 idx1 = vtxStart + v + 1;
      u16 idx2 = vtxStart + v + 2;
      u16 idx3 = vtxStart + v + 3;

      buf.append(idx0);
      buf.append(idx1);
      buf.append(idx2);
      numIndices += 3;

      buf.append(idx2);
      buf.append(idx3);
      buf.append(idx0);
      numIndices += 3;
    }
    if (vtxCount - v == 3) {
      buf.append(static_cast<u16>(vtxStart + v));
      buf.append(static_cast<u16>(vtxStart + v + 1));
      buf.append(static_cast<u16>(vtxStart + v + 2));
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLES) {
    buf.reserve_extra(vtxCount * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      buf.append(idx);
      ++numIndices;
    }
  } else if (prim == GX_TRIANGLEFAN) {
    buf.reserve_extra(((u32(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      buf.append(std::array{vtxStart, static_cast<u16>(idx - 1), idx});
      numIndices += 3;
    }
  } else if (prim == GX_TRIANGLESTRIP) {
    buf.reserve_extra(((static_cast<u32>(vtxCount) - 3) * 3 + 3) * sizeof(u16));
    for (u16 v = 0; v < vtxCount; ++v) {
      const u16 idx = vtxStart + v;
      if (v < 3) {
        buf.append(idx);
        ++numIndices;
        continue;
      }
      if ((v & 1) == 0) {
        buf.append(std::array{static_cast<u16>(idx - 2), static_cast<u16>(idx - 1), idx});
      } else {
        buf.append(std::array{static_cast<u16>(idx - 1), static_cast<u16>(idx - 2), idx});
      }
      numIndices += 3;
    }
  } else if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
    buf.reserve_extra(6 * sizeof(u16));
    buf.append<u16>(0);
    buf.append<u16>(1);
    buf.append<u16>(3);
    buf.append<u16>(3);
    buf.append<u16>(2);
    buf.append<u16>(0);
    numIndices = 6;
  } else
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  return numIndices;
}

// SwitchWaker patch 0021: indices a primitive of `vtxCount` vertices adds (as prepare_idx_buffer appends).
u32 idx_count(GXPrimitive prim, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_QUADS:
    return (vtxCount / 4) * 6 + (vtxCount % 4 == 3 ? 3 : 0);
  case GX_TRIANGLES:
    return vtxCount;
  case GX_TRIANGLEFAN:
  case GX_TRIANGLESTRIP:
    return vtxCount <= 3 ? vtxCount : 3 + (static_cast<u32>(vtxCount) - 3) * 3;
  case GX_LINES:
  case GX_LINESTRIP:
  case GX_POINTS:
    return 6;
  default:
    UNLIKELY FATAL("unsupported primitive type {}", static_cast<u32>(prim));
  }
}

// prepare_idx_buffer's indices written to `out` (idx_count(prim, vtxCount) of them).
void write_idx(u16* out, GXPrimitive prim, u16 vtxStart, u16 vtxCount) noexcept {
  switch (prim) {
  case GX_QUADS: {
    u16 v = 0;
    for (; v + 4 <= vtxCount; v += 4) {
      const u16 i0 = vtxStart + v;
      out[0] = i0;
      out[1] = i0 + 1;
      out[2] = i0 + 2;
      out[3] = i0 + 2;
      out[4] = i0 + 3;
      out[5] = i0;
      out += 6;
    }
    if (vtxCount - v == 3) {
      out[0] = vtxStart + v;
      out[1] = vtxStart + v + 1;
      out[2] = vtxStart + v + 2;
    }
    break;
  }
  case GX_TRIANGLES:
    for (u16 v = 0; v < vtxCount; ++v) {
      out[v] = vtxStart + v;
    }
    break;
  case GX_TRIANGLEFAN:
  case GX_TRIANGLESTRIP: {
    const u16 head = vtxCount < 3 ? vtxCount : 3;
    for (u16 v = 0; v < head; ++v) {
      *out++ = vtxStart + v;
    }
    if (prim == GX_TRIANGLEFAN) {
      for (u16 v = 3; v < vtxCount; ++v) {
        const u16 idx = vtxStart + v;
        out[0] = vtxStart;
        out[1] = idx - 1;
        out[2] = idx;
        out += 3;
      }
    } else {
      for (u16 v = 3; v < vtxCount; ++v) {
        const u16 idx = vtxStart + v;
        const bool even = (v & 1) == 0;
        out[0] = even ? idx - 2 : idx - 1;
        out[1] = even ? idx - 1 : idx - 2;
        out[2] = idx;
        out += 3;
      }
    }
    break;
  }
  default: {
    static constexpr u16 kQuad[6] = {0, 1, 3, 3, 2, 0};
    std::memcpy(out, kQuad, sizeof(kQuad));
    break;
  }
  }
}

bool sFastPaths = true;

// Pipeline lookups of this frame by (pipeline config, render target layout): a draw state seen earlier in
// the frame reuses its shader info and pipeline ref. Valid for one frame (clear_draw_cache) so each state
// still goes through pipeline_ref once a frame (remembered for the cache, promoted while it compiles).
struct PipelineMemoEntry {
  uint64_t frame = 0;
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
};
std::unordered_map<HashType, PipelineMemoEntry> sPipelineMemo;
uint64_t sPipelineMemoFrame = 1;

// Texture bind groups of this frame by what build_bind_groups reads: each sampled slot's texture view and
// sampler descriptor. Skips the per-slot sampler cache lookups (hash, lock, refcounts) and the bind group
// descriptor's hash and lock for a texture set seen earlier in the frame. Valid for one frame, as the
// pipeline memo (bind_group_ref marks a bind group used once a frame); the entry holds the slots' texture
// handles so no view address is reused while it can match.
struct BindMemoEntry {
  uint64_t frame = 0;
  GXBindGroups bindGroups{};
  std::array<decltype(gfx::TextureBind::ref), MaxTextures> textures{};
};
std::unordered_map<HashType, BindMemoEntry> sBindMemo;

constexpr size_t kSamplerKeyOffset = offsetof(wgpu::SamplerDescriptor, addressModeU);
constexpr size_t kSamplerKeySize = sizeof(wgpu::SamplerDescriptor) - kSamplerKeyOffset - 2;

HashType bind_memo_key(const ShaderInfo& info) noexcept {
  std::array<u8, MaxTextures*(sizeof(void*) + kSamplerKeySize)> key{};
  u8* out = key.data();
  for (u32 i = 0; i < MaxTextures; ++i, out += sizeof(void*) + kSamplerKeySize) {
    const auto& tex = g_gxState.textures[i];
    if (!tex || !(info.sampledTextures[i] || info.sampledIndTextures[i])) {
      continue;
    }
    const void* view = tex.ref->sampleTextureView.Get();
    std::memcpy(out, &view, sizeof(void*));
    const wgpu::SamplerDescriptor desc = tex.get_descriptor();
    std::memcpy(out + sizeof(void*), reinterpret_cast<const u8*>(&desc) + kSamplerKeyOffset, kSamplerKeySize);
  }
  return xxh3_hash_s(key.data(), key.size(), info.sampledTextures.any() || info.sampledIndTextures.any());
}

// GX FIFO opcodes - use CP_ prefix to avoid clashing with GXCommandList.h macros
constexpr u8 CP_CMD_NOP = GX_NOP;
constexpr u8 CP_CMD_LOAD_CP_REG = GX_LOAD_CP_REG;
constexpr u8 CP_CMD_LOAD_XF_REG = GX_LOAD_XF_REG;
constexpr u8 CP_CMD_LOAD_INDX_A = GX_LOAD_INDX_A;
constexpr u8 CP_CMD_LOAD_INDX_B = GX_LOAD_INDX_B;
constexpr u8 CP_CMD_LOAD_INDX_C = GX_LOAD_INDX_C;
constexpr u8 CP_CMD_LOAD_INDX_D = GX_LOAD_INDX_D;
constexpr u8 CP_CMD_CALL_DL = GX_CMD_CALL_DL;
constexpr u8 CP_CMD_INVAL_VTX = GX_CMD_INVL_VC;
constexpr u8 CP_CMD_LOAD_BP_REG = GX_LOAD_BP_REG & GX_OPCODE_MASK;

// Primitive type mask
constexpr u8 CP_OPCODE_MASK = GX_OPCODE_MASK;
constexpr u8 CP_VAT_MASK = GX_VAT_MASK;

struct FogRangeLutKey {
  std::array<u16, 10> rangeK;
  f32 rangeCenter;
  f32 renderWidth;
  u32 targetWidth;

  bool operator==(const FogRangeLutKey&) const = default;
};

struct FogRangeLutEntry {
  FogRangeLutKey key;
  std::vector<f32> factors;
};

constexpr size_t MaxFogRangeLuts = 32;
std::vector<FogRangeLutEntry> sFogRangeLuts;

struct DrawCache {
  uint64_t targetLayoutKey = 0;
  PipelineConfig config{};
  ShaderInfo shaderInfo{};
  gfx::PipelineRef pipelineRef{};
  GXBindGroups bindGroups{};
  uint64_t bindGeneration = 0;
  GXVtxFmt fmt = GX_MAX_VTXFMT;
  u8 lineMode = 0;
  bool hasPipeline = false;
  gfx::Range uniformRange{};
  gfx::Range fogRange{};
  FogRangeLutKey fogRangeKey{};
  bool hasFogRange = false;
  GXVtxFmt lastDrawFmt = GX_MAX_VTXFMT;
};
DrawCache sDrawCache;

FogRangeLutKey fog_range_lut_key() noexcept {
  const auto& state = g_gxState.fog;
  const f32 logicalWidth = std::max(g_gxState.logicalViewport.width, 1.f);
  const f32 renderWidth = std::max(g_gxState.renderViewport.width, 1.f);
  return {
      .rangeK = state.rangeK,
      .rangeCenter = ((static_cast<f32>(state.rangeCenter) - g_gxState.logicalViewport.left) / logicalWidth) * 2.f -
                     1.f + (g_gxState.renderViewport.left / renderWidth) * 2.f,
      .renderWidth = renderWidth,
      .targetWidth = gfx::get_render_target_size().x,
  };
}

std::vector<f32> build_fog_range_lut(const FogRangeLutKey& key) {
  std::array<f32, 10> rangeK;
  for (u32 i = 0; i < rangeK.size(); ++i) {
    const u32 source = (i & ~1u) | (1u - (i & 1u));
    rangeK[i] = static_cast<f32>(key.rangeK[source]) / 64.f;
  }

  std::vector<f32> lut(key.targetWidth);
  for (u32 x = 0; x < key.targetWidth; ++x) {
    const f32 screenX = ((static_cast<f32>(x) + 0.5f) / key.renderWidth) * 2.f - 1.f;
    const f32 offset = screenX - key.rangeCenter;
    const f32 rangeIndex = std::clamp(9.f - std::abs(offset) * 9.f, 0.f, 9.f);
    const u32 lower = static_cast<u32>(rangeIndex);
    const u32 upper = std::min(lower + 1, 9u);
    const f32 fraction = rangeIndex - static_cast<f32>(lower);
    const f32 k = std::max(rangeK[lower] * (1.f - fraction) + rangeK[upper] * fraction, 0.000001f);
    lut[x] = std::sqrt(offset * offset + k * k) / k;
  }
  return lut;
}

const std::vector<f32>& resolve_fog_range_lut(const FogRangeLutKey& key) {
  for (const auto& entry : sFogRangeLuts) {
    if (entry.key == key) {
      return entry.factors;
    }
  }
  if (sFogRangeLuts.size() == MaxFogRangeLuts) {
    sFogRangeLuts.erase(sFogRangeLuts.begin());
  }
  sFogRangeLuts.emplace_back(FogRangeLutEntry{key, build_fog_range_lut(key)});
  return sFogRangeLuts.back().factors;
}

gfx::Range push_fog_range_lut(const FogRangeLutKey& key) {
  const auto& lut = resolve_fog_range_lut(key);
  return gfx::push_storage(reinterpret_cast<const u8*>(lut.data()), lut.size() * sizeof(f32));
}

u8 line_mode_for_prim(GXPrimitive prim) noexcept {
  switch (prim) {
  case GX_LINES:
    return 1;
  case GX_LINESTRIP:
    return 2;
  case GX_POINTS:
    return 3;
  default:
    return 0;
  }
}
} // namespace

static void handle_draw(u8 cmd, ByteReader& reader) noexcept;
static void handle_aurora(ByteReader& reader) noexcept;

ProcessResult process(const u8* data, u32 size) noexcept {
  ZoneScoped;
  ByteReader reader{{data, size}};

  while (!reader.empty()) {
    const u8 cmd = reader.read<u8>();
    u8 opcode = cmd & CP_OPCODE_MASK;

    switch (opcode) {
    case CP_CMD_NOP:
      continue;

    case CP_CMD_LOAD_BP_REG: {
      const u32 value = reader.read<u32>();
      profile::count(AURORA_FIFO_PROFILE_BP_LOADS);
      if (!sFastPaths || !bp_redundant(value)) {
        profile::Scope prof{AURORA_FIFO_PROFILE_BP};
        handle_bp(value);
      }
      if (reg_get(value, 8, 24) == GX_BP_REG_DRAWDONE) {
        return {static_cast<u32>(reader.offset()), true};
      }
      break;
    }

    case CP_CMD_LOAD_CP_REG: {
      const u8 addr = reader.read<u8>();
      profile::Scope prof{AURORA_FIFO_PROFILE_CP};
      handle_cp(addr, reader.read<u32>());
      break;
    }

    case CP_CMD_LOAD_XF_REG: {
      const u32 header = reader.read<u32>();
      const u32 count = ((header >> 16) & 0xFFFF) + 1;
      const u16 addr = header & 0xFFFF;
      profile::Scope prof{AURORA_FIFO_PROFILE_XF};
      profile::count(AURORA_FIFO_PROFILE_XF_LOADS);
      handle_xf(addr, reader.take(count * sizeof(u32)));
      break;
    }

    case CP_CMD_LOAD_INDX_A:
    case CP_CMD_LOAD_INDX_B:
    case CP_CMD_LOAD_INDX_C:
    case CP_CMD_LOAD_INDX_D: {
      ZoneScopedN("LOAD_INDX");
      profile::Scope prof{AURORA_FIFO_PROFILE_XF};
      profile::count(AURORA_FIFO_PROFILE_XF_LOADS);
      const u32 arrayType = GX_POS_MTX_ARRAY + (opcode - CP_CMD_LOAD_INDX_A) / 0x08;
      const u16 srcArrayIdx = reader.read<u16>();
      const u16 addrLen = reader.read<u16>();

      const u16 len = (addrLen >> 12) + 1;
      const u16 dstAddr = addrLen & 0x0FFF;
      auto const& array = g_gxState.arrays[arrayType];
      const u32 srcOffset = static_cast<u32>(srcArrayIdx) * array.stride;
      const u32 srcSize = static_cast<u32>(len) * sizeof(u32);
      AURORA_ASSERT(array.data != nullptr, "indexed XF load from unmapped array {}", arrayType);
      AURORA_ASSERT(srcOffset <= array.size && srcSize <= array.size - srcOffset,
                    "indexed XF load outside array {}: offset={}, size={}, array size={}", arrayType, srcOffset,
                    srcSize, array.size);
      auto const* srcData = static_cast<const u8*>(array.data) + srcOffset;
      if (!copy_xf_data(dstAddr, srcData, len, array.le ? std::endian::little : std::endian::big)) {
#ifndef NDEBUG
        Log.debug("Unimplemented indexed XF load (opcode 0x{:02X}, dstAddr=%04x)", opcode, dstAddr);
#endif
      }
      break;
    }

    case CP_CMD_CALL_DL: {
      // Call display list: 8 bytes (address + size)
      Log.warn("Ignoring nested GX_CMD_CALL_DL");
      reader.skip(8);
      break;
    }

    case CP_CMD_INVAL_VTX: {
      for (auto& array : g_gxState.arrays) {
        array.cachedRange = {};
      }
      g_gxState.dirty |= DirtyImmediates;
      break;
    }

    case GX_AURORA: {
      handle_aurora(reader);
      break;
    }

    // Draw commands: 0x80-0xBF
    case GX_DRAW_QUADS:
    case GX_DRAW_TRIANGLES:
    case GX_DRAW_TRIANGLE_STRIP:
    case GX_DRAW_TRIANGLE_FAN:
    case GX_DRAW_LINES:
    case GX_DRAW_LINE_STRIP:
    case GX_DRAW_POINTS: {
      handle_draw(cmd, reader);
      break;
    }

    default:
      // Check if it's a draw command (0x80-0xBF range)
      if (cmd >= 0x80) {
        handle_draw(cmd, reader);
      } else {
        // Hex dump surrounding bytes for debugging
        {
          const size_t pos = reader.offset();
          size_t dumpStart = (pos > 17) ? pos - 17 : 0;
          size_t dumpEnd = (pos + 16 < size) ? pos + 16 : size;
          std::string hex;
          for (size_t i = dumpStart; i < dumpEnd; i++) {
            if (i == pos - 1)
              hex += fmt::format("[{:02x}]", data[i]);
            else
              hex += fmt::format(" {:02x}", data[i]);
          }
          Log.error("  hex dump (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
        }
        FATAL("command_processor: unknown opcode 0x{:02X} at pos {}", cmd, reader.offset() - 1);
      }
      break;
    }
  }
  return {size, false};
}

[[noreturn]] static void handle_draw_overrun(size_t totalVtxBytes, const ByteReader& reader) noexcept {
  // Hex dump around the draw command for debugging
  const size_t pos = reader.offset();
  const size_t size = reader.size();
  const u8* data = reader.data();
  size_t cmdPos = pos - 2 - 1; // opcode byte position (before vtxCount and pos++)
  size_t dumpStart = (cmdPos > 16) ? cmdPos - 16 : 0;
  size_t dumpEnd = (cmdPos + 32 < size) ? cmdPos + 32 : size;
  std::string hex;
  for (size_t i = dumpStart; i < dumpEnd; i++) {
    if (i == cmdPos)
      hex += fmt::format("[{:02x}]", data[i]);
    else
      hex += fmt::format(" {:02x}", data[i]);
  }
  Log.error("  hex dump around draw cmd (pos {}-{}):{}", dumpStart, dumpEnd - 1, hex);
  FATAL("draw vertex data overrun: need {} bytes at pos {}, have {}", totalVtxBytes, pos, reader.remaining());
}

static u32 calc_vtx_size(GXVtxFmt fmt) noexcept {
  u32 vtxSize = 0;
  const auto& vtxFmt = g_gxState.vtxFmts[fmt];
  for (int i = GX_VA_PNMTXIDX; i <= GX_VA_TEX7; ++i) {
    const auto& attrFmt = vtxFmt.attrs[i];
    switch (g_gxState.vtxDesc[i]) {
    case GX_NONE:
      break;
    case GX_DIRECT: {
      const auto attr = static_cast<GXAttr>(i);
      vtxSize += comp_type_size(attr, attrFmt.type) * comp_cnt_count(attr, attrFmt.cnt);
      break;
    }
    case GX_INDEX8:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 3 : 1;
      break;
    case GX_INDEX16:
      vtxSize += i == GX_VA_NRM && attrFmt.cnt == GX_NRM_NBT3 ? 6 : 2;
      break;
    }
  }
  g_gxState.lastVtxFmt = fmt;
  g_gxState.lastVtxSize = vtxSize;
  return vtxSize;
}

// Draw census (native/patches/aurora/0008, lib/gfx/census.hpp): what the draw just pushed is.
static void census_record(GXPrimitive prim, u16 vtxCount, const PipelineConfig& config,
                          const ShaderInfo& info) noexcept {
  auto* capture = gfx::census::recording();
  if (capture == nullptr || capture->draws.size() >= capture->capacity) {
    return;
  }
  const auto& state = g_gxState;
  const auto& sc = config.shaderConfig;
  gfx::census::DrawInfo d{};
  d.pass = gfx::current_pass_index();
  d.marker = capture->currentMarker;
  d.prim = prim;
  d.vtxCount = vtxCount;
  d.pipelineHash = sDrawCache.pipelineRef;
  d.tevStages = static_cast<uint8_t>(sc.tevStageCount);
  d.indStages = static_cast<uint8_t>(sc.numIndStages);
  d.alphaTest = sc.alphaCompare ? 1 : 0;
  d.alphaComp0 = sc.alphaCompare.comp0;
  d.alphaRef0 = static_cast<uint8_t>(sc.alphaCompare.ref0);
  d.alphaOp = sc.alphaCompare.op;
  d.alphaComp1 = sc.alphaCompare.comp1;
  d.alphaRef1 = static_cast<uint8_t>(sc.alphaCompare.ref1);
  d.blendMode = config.blendMode;
  d.blendSrc = config.blendFacSrc;
  d.blendDst = config.blendFacDst;
  d.depthCompare = config.depthCompare;
  d.depthFunc = config.depthFunc;
  d.depthUpdate = config.depthUpdate;
  d.colorUpdate = config.colorUpdate;
  d.alphaUpdate = config.alphaUpdate;
  d.cull = config.cullMode;
  d.fog = sc.fogType;
  d.dstAlpha = config.dstAlpha != UINT32_MAX ? 1 : 0;
  d.numChans = state.numChans;
  d.vpWidth = state.renderViewport.width;
  d.vpHeight = state.renderViewport.height;
  for (u32 i = 0; i < MaxTextures && d.texCount < 8; ++i) {
    if (!info.sampledTextures.test(i)) {
      continue;
    }
    auto& t = d.tex[d.texCount++];
    const auto& bind = state.textures[i];
    if (bind.ref) {
      t.width = static_cast<uint16_t>(bind.ref->size.width);
      t.height = static_cast<uint16_t>(bind.ref->size.height);
      t.mips = static_cast<uint8_t>(bind.ref->mipCount);
      t.gxFormat = static_cast<uint8_t>(bind.ref->gxFormat);
    }
    t.minFilter = static_cast<uint8_t>(state.loadedTextures[i].min_filter());
    t.isCopy = state.copyTextures.contains(state.loadedTextures[i].data) ? 1 : 0;
  }
  const auto index = static_cast<uint32_t>(capture->draws.size());
  capture->draws.push_back(d);
  gfx::set_last_command_census(index);
}

static void push_gx_draw(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange, gfx::Range idxRange,
                         u32 numIndices) noexcept {
  auto& state = g_gxState;
  auto& cache = sDrawCache;

  DrawImmediateData immediates{.vtxStart = vertRange.offset, .currentPnMtx = state.currentPnMtx};
  if (auto* capture = gfx::census::recording())
    UNLIKELY {
      // Draw census fragment counters: this draw's slot (census_record below appends it).
      if (gfx::census::counters_enabled() && capture->draws.size() < capture->capacity) {
        immediates._pad = static_cast<u32>(capture->draws.size()) + 1;
      }
    }
  profile::count(AURORA_FIFO_PROFILE_DRAWS);
  for (int i = GX_VA_POS; i <= GX_VA_TEX7; ++i) {
    if (state.vtxDesc[i] != GX_INDEX8 && state.vtxDesc[i] != GX_INDEX16) {
      continue;
    }
    auto& array = state.arrays[i];
    if (array.cachedRange.size == 0) {
      profile::Scope prof{AURORA_FIFO_PROFILE_ARRAYS};
      profile::count(AURORA_FIFO_PROFILE_ARRAY_BYTES, array.size);
      array.cachedRange = gfx::push_storage(static_cast<const uint8_t*>(array.data), array.size);
    }
    immediates.arrayStart[i - GX_VA_POS] = array.cachedRange.offset;
  }

  const u8 lineMode = line_mode_for_prim(prim);
  const auto targetLayoutKey = gfx::get_render_target_layout().key;
  const bool pipelineValid = cache.hasPipeline && (state.dirty & DirtyPipeline) == 0 && cache.fmt == fmt &&
                             cache.lineMode == lineMode && cache.targetLayoutKey == targetLayoutKey;
  if (!pipelineValid) {
    profile::Scope prof{AURORA_FIFO_PROFILE_PIPELINE};
    profile::count(AURORA_FIFO_PROFILE_PIPELINES);
    const bool hadPipeline = cache.hasPipeline;
    const auto prevSampledTextures = cache.shaderInfo.sampledTextures;
    const auto prevSampledIndTextures = cache.shaderInfo.sampledIndTextures;
    populate_pipeline_config(cache.config, prim, fmt);
    if (sFastPaths) {
      const auto key = xxh3_hash(cache.config, targetLayoutKey);
      auto [it, inserted] = sPipelineMemo.try_emplace(key);
      auto& memo = it->second;
      if (!inserted && memo.frame == sPipelineMemoFrame) {
        profile::count(AURORA_FIFO_PROFILE_PIPELINE_MEMO_HITS);
        cache.shaderInfo = memo.shaderInfo;
        cache.pipelineRef = memo.pipelineRef;
      } else {
        cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
        {
          profile::Scope profLookup{AURORA_FIFO_PROFILE_PIPELINE_LOOKUP};
          cache.pipelineRef = gfx::pipeline_ref(cache.config);
        }
        memo.frame = sPipelineMemoFrame;
        memo.shaderInfo = cache.shaderInfo;
        memo.pipelineRef = cache.pipelineRef;
      }
    } else {
      cache.shaderInfo = build_shader_info(cache.config.shaderConfig);
      profile::Scope profLookup{AURORA_FIFO_PROFILE_PIPELINE_LOOKUP};
      cache.pipelineRef = gfx::pipeline_ref(cache.config);
    }
    cache.targetLayoutKey = targetLayoutKey;
    cache.fmt = fmt;
    cache.lineMode = lineMode;
    cache.hasPipeline = true;
    state.dirty = (state.dirty & ~DirtyPipeline) | DirtyUniform;
    if (!hadPipeline || prevSampledTextures != cache.shaderInfo.sampledTextures ||
        prevSampledIndTextures != cache.shaderInfo.sampledIndTextures) {
      cache.bindGeneration = 0;
    }
  }

  const bool bindGroupsValid =
      (state.dirty & DirtyTextures) == 0 && cache.bindGeneration == texture::current_bind_generation();
  if (!bindGroupsValid) {
    profile::Scope prof{AURORA_FIFO_PROFILE_BIND};
    profile::count(AURORA_FIFO_PROFILE_BINDS);
    const auto prevBindGroup = cache.bindGroups.textureBindGroup;
    {
      profile::Scope profTextures{AURORA_FIFO_PROFILE_TEXTURES};
      resolve_sampled_textures(cache.shaderInfo);
    }
    if (sFastPaths) {
      auto [it, inserted] = sBindMemo.try_emplace(bind_memo_key(cache.shaderInfo));
      auto& memo = it->second;
      if (!inserted && memo.frame == sPipelineMemoFrame) {
        profile::count(AURORA_FIFO_PROFILE_BIND_MEMO_HITS);
        cache.bindGroups = memo.bindGroups;
      } else {
        cache.bindGroups = build_bind_groups(cache.shaderInfo);
        memo.frame = sPipelineMemoFrame;
        memo.bindGroups = cache.bindGroups;
        for (u32 i = 0; i < MaxTextures; ++i) {
          memo.textures[i] = g_gxState.textures[i].ref;
        }
      }
    } else {
      cache.bindGroups = build_bind_groups(cache.shaderInfo);
    }
    cache.bindGeneration = texture::current_bind_generation();
    state.dirty &= ~DirtyTextures;
    // For texture_size_bias uniform
    if (cache.bindGroups.textureBindGroup != prevBindGroup) {
      state.dirty |= DirtyUniform;
    }
  }

  const bool uniformValid = (state.dirty & DirtyUniform) == 0 && cache.uniformRange.size != 0;
  if (!uniformValid) {
    profile::Scope prof{AURORA_FIFO_PROFILE_UNIFORM};
    profile::count(AURORA_FIFO_PROFILE_UNIFORMS);
    cache.uniformRange = build_uniform(cache.shaderInfo);
    profile::count(AURORA_FIFO_PROFILE_UNIFORM_BYTES, cache.uniformRange.size);
    state.dirty &= ~DirtyUniform;
  }
  if (cache.config.shaderConfig.fogRangeEnabled) {
    const auto key = fog_range_lut_key();
    if (!cache.hasFogRange || cache.fogRangeKey != key) {
      cache.fogRange = push_fog_range_lut(key);
      cache.fogRangeKey = key;
      cache.hasFogRange = true;
    }
  }
  immediates.fogRangeBase = cache.fogRange.offset / sizeof(u32);

  state.dirty &= ~DirtyImmediates;

  uint32_t instanceCount = 1;
  if (prim == GX_LINES) {
    instanceCount = vtxCount / 2;
  } else if (prim == GX_LINESTRIP) {
    instanceCount = vtxCount - 1;
  } else if (prim == GX_POINTS) {
    instanceCount = vtxCount;
  }
  cache.lastDrawFmt = fmt;
  profile::Scope profCommand{AURORA_FIFO_PROFILE_COMMAND};
  gfx::push_draw_command(DrawData{
      .pipeline = cache.pipelineRef,
      .bindGroups = cache.bindGroups,
      .vertRange = vertRange,
      .idxRange = idxRange,
      .uniformRange = cache.uniformRange,
      .immediateData = immediates,
      .vtxCount = vtxCount,
      .indexCount = numIndices,
      .instanceCount = instanceCount,
      .dstAlpha = state.dstAlpha,
  });
  if (gfx::census::recording() != nullptr)
    UNLIKELY { census_record(prim, vtxCount, cache.config, cache.shaderInfo); }
}

static void handle_draw_unmerged(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, gfx::Range vertRange) noexcept {
  ZoneScoped;
  u32 numIndices = 0;
  gfx::Range idxRange;

  if (prim != GX_TRIANGLES && sFastPaths) {
    numIndices = idx_count(prim, vtxCount);
    if (auto* out = gfx::map_indices(numIndices * sizeof(u16), 4, idxRange)) {
      write_idx(reinterpret_cast<u16*>(out), prim, 0, vtxCount);
    }
  } else if (prim != GX_TRIANGLES) {
    ZoneScopedN("build idx buffer");
    static ByteBuffer idxBuf;
    numIndices = prepare_idx_buffer(idxBuf, prim, 0, vtxCount);
    idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), 4);
    idxBuf.clear();
  }

  push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, numIndices);
}

static void draw_prim_one(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept;

// SwitchWaker patch 0021: the draw commands that follow a draw in the stream with the same vertex format
// (display lists are runs of short strips), merged into the last draw command in one go: what
// draw_prim_one's merge does per primitive (same vertex bytes, same indices), with one look-up of the
// last command, one vertex and one index reservation and no per-primitive checks. Stops at anything
// else, at line/point primitives, past 0xFFFF vertices in the draw, or while the draw census records.
static void merge_run(GXVtxFmt fmt, u32 vtxSize, ByteReader& reader) noexcept {
  struct Prim {
    GXPrimitive prim;
    u16 vtxCount;
    const u8* data;
  };
  static std::vector<Prim> run;
  if (g_gxState.dirty != 0 || fmt != sDrawCache.lastDrawFmt || sDrawCache.lineMode != 0 ||
      gfx::census::recording() != nullptr) {
    return;
  }
  auto* lastDraw = gfx::get_last_draw_command<DrawData>();
  if (lastDraw == nullptr || lastDraw->instanceCount != 1) {
    return;
  }
  run.clear();
  u32 runVerts = 0;
  bool anyIndexed = false;
  ByteReader scan = reader;
  while (scan.remaining() >= 3) {
    const u8 cmd = scan.data()[scan.offset()];
    if (cmd < 0x80 || cmd >= 0xC0 || static_cast<GXVtxFmt>(cmd & CP_VAT_MASK) != fmt) {
      break;
    }
    const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (prim == GX_LINES || prim == GX_LINESTRIP || prim == GX_POINTS) {
      break;
    }
    ByteReader peek = scan;
    peek.skip(1);
    const u16 count = peek.read<u16>();
    const u32 bytes = count * vtxSize;
    if (bytes > peek.remaining() || lastDraw->vtxCount + runVerts + count > 0xFFFF) {
      break;
    }
    run.push_back({prim, count, peek.data() + peek.offset()});
    runVerts += count;
    anyIndexed |= prim != GX_TRIANGLES;
    peek.skip(bytes);
    scan = peek;
  }
  if (run.empty()) {
    return;
  }
  reader = scan;

  gfx::Range vertRange;
  if (auto* out = gfx::map_verts(runVerts * vtxSize, 0, vertRange)) {
    for (const auto& p : run) {
      std::memcpy(out, p.data, p.vtxCount * vtxSize);
      out += p.vtxCount * vtxSize;
    }
  }
  CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
        "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
        vertRange.offset);

  // Indices: as draw_prim_one makes them one primitive at a time. An unindexed draw stays so while only
  // triangle lists join it; the first other primitive indexes everything before it as a triangle list.
  if (lastDraw->indexCount != 0 || anyIndexed) {
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    const u32 prevCount = lastDraw->indexCount == 0 ? idx_count(GX_TRIANGLES, lastDraw->vtxCount) : 0;
    u32 runIndices = 0;
    for (const auto& p : run) {
      runIndices += idx_count(p.prim, p.vtxCount);
    }
    gfx::Range idxRange;
    if (auto* out = reinterpret_cast<u16*>(
            gfx::map_indices((prevCount + runIndices) * sizeof(u16), hadIndexRange ? 0 : 4, idxRange))) {
      if (prevCount != 0) {
        write_idx(out, GX_TRIANGLES, 0, lastDraw->vtxCount);
        out += prevCount;
      }
      u16 start = lastDraw->vtxCount;
      for (const auto& p : run) {
        write_idx(out, p.prim, start, p.vtxCount);
        out += idx_count(p.prim, p.vtxCount);
        start += p.vtxCount;
      }
    }
    if (hadIndexRange) {
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
      lastDraw->idxRange.size += idxRange.size;
    } else {
      lastDraw->idxRange = idxRange;
    }
    lastDraw->indexCount += prevCount + runIndices;
  }
  lastDraw->vertRange.size += vertRange.size;
  lastDraw->vtxCount += runVerts;
  gfx::detail::add_merged_draw_count(static_cast<u32>(run.size()));
  profile::count(AURORA_FIFO_PROFILE_MERGED, run.size());
  profile::count(AURORA_FIFO_PROFILE_VERTEX_BYTES, runVerts * vtxSize);
}

static void draw_prim(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {
  draw_prim_one(prim, fmt, vtxCount, reader);
  if (sFastPaths) {
    merge_run(fmt, g_gxState.lastVtxSize, reader);
  }
}

static void draw_prim_one(GXPrimitive prim, GXVtxFmt fmt, u16 vtxCount, ByteReader& reader) noexcept {
  ZoneScoped;
  u32 vtxSize;
  if (g_gxState.lastVtxFmt == fmt)
    LIKELY { vtxSize = g_gxState.lastVtxSize; }
  else
    UNLIKELY { vtxSize = calc_vtx_size(fmt); }

  u32 totalVtxBytes = vtxCount * vtxSize;
  if (totalVtxBytes > reader.remaining())
    UNLIKELY { handle_draw_overrun(totalVtxBytes, reader); }

  const bool cleanState = g_gxState.dirty == 0 && fmt == sDrawCache.lastDrawFmt && sDrawCache.lineMode == 0 &&
                          prim != GX_LINES && prim != GX_LINESTRIP && prim != GX_POINTS;
  auto* lastDraw = cleanState ? gfx::get_last_draw_command<DrawData>() : nullptr;
  const bool canMerge = lastDraw != nullptr && lastDraw->instanceCount == 1;

  // Push raw vertex data to buffer. Merged draws must remain contiguous with the previous range.
  const auto vertexData = reader.take(totalVtxBytes);
  profile::count(AURORA_FIFO_PROFILE_VERTEX_BYTES, totalVtxBytes);
  gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), canMerge ? 0 : 4);

  // Try to merge with previous draw call (patch 0021's fast path: the indices written in place)
  if (canMerge && sFastPaths) {
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    const bool indexPrev = lastDraw->indexCount == 0 && prim != GX_TRIANGLES;
    u32 numIndices = 0;
    if (indexPrev || lastDraw->indexCount != 0) {
      const u32 prevCount = indexPrev ? idx_count(GX_TRIANGLES, lastDraw->vtxCount) : 0;
      numIndices = idx_count(prim, vtxCount);
      gfx::Range idxRange;
      auto* out = reinterpret_cast<u16*>(
          gfx::map_indices((prevCount + numIndices) * sizeof(u16), hadIndexRange ? 0 : 4, idxRange));
      if (out != nullptr) {
        if (indexPrev) {
          write_idx(out, GX_TRIANGLES, 0, lastDraw->vtxCount);
          lastDraw->indexCount = prevCount;
        }
        write_idx(out + prevCount, prim, lastDraw->vtxCount, vtxCount);
      }
      if (hadIndexRange) {
        CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
              "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
              idxRange.offset);
        lastDraw->idxRange.size += idxRange.size;
      } else {
        lastDraw->idxRange = idxRange;
      }
    }
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          vertRange.offset);
    lastDraw->vertRange.size += vertRange.size;
    lastDraw->vtxCount += vtxCount;
    lastDraw->indexCount += numIndices;
    if (auto* capture = gfx::census::recording())
      UNLIKELY {
        const auto index = gfx::last_command_census();
        if (index < capture->draws.size()) {
          capture->draws[index].vtxCount += vtxCount;
          capture->draws[index].merges++;
        }
      }
    gfx::detail::increment_merged_draw_count();
    profile::count(AURORA_FIFO_PROFILE_MERGED);
    return;
  }
  if (canMerge) {
    u32 numIndices = 0;
    gfx::Range idxRange;
    static ByteBuffer idxBuf;
    const bool hadIndexRange = lastDraw->idxRange.size != 0;
    if (lastDraw->indexCount == 0 && prim != GX_TRIANGLES) {
      // Generate triangle index buffer for previous draw
      lastDraw->indexCount = prepare_idx_buffer(idxBuf, GX_TRIANGLES, 0, lastDraw->vtxCount);
    }
    if (lastDraw->indexCount != 0) {
      numIndices += prepare_idx_buffer(idxBuf, prim, lastDraw->vtxCount, vtxCount);
      idxRange = gfx::push_indices(idxBuf.data(), idxBuf.size(), hadIndexRange ? 0 : 4);
      idxBuf.clear();
    }
    CHECK(lastDraw->vertRange.offset + lastDraw->vertRange.size == vertRange.offset,
          "Non-consecutive vertex ranges ({} < {})", lastDraw->vertRange.offset + lastDraw->vertRange.size,
          vertRange.offset);
    if (hadIndexRange) {
      CHECK(lastDraw->idxRange.offset + lastDraw->idxRange.size == idxRange.offset,
            "Non-consecutive index ranges ({} < {})", lastDraw->idxRange.offset + lastDraw->idxRange.size,
            idxRange.offset);
    }
    lastDraw->vertRange.size += vertRange.size;
    if (lastDraw->idxRange.size == 0) {
      lastDraw->idxRange = idxRange;
    } else {
      lastDraw->idxRange.size += idxRange.size;
    }
    lastDraw->vtxCount += vtxCount;
    lastDraw->indexCount += numIndices;
    if (auto* capture = gfx::census::recording())
      UNLIKELY {
        const auto index = gfx::last_command_census();
        if (index < capture->draws.size()) {
          capture->draws[index].vtxCount += vtxCount;
          capture->draws[index].merges++;
        }
      }
    gfx::detail::increment_merged_draw_count();
    profile::count(AURORA_FIFO_PROFILE_MERGED);
    return;
  }

  handle_draw_unmerged(prim, fmt, vtxCount, vertRange);
}

static void handle_draw(u8 cmd, ByteReader& reader) noexcept {
  const auto fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
  const auto prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
  profile::Scope prof{AURORA_FIFO_PROFILE_DRAW};
  draw_prim(prim, fmt, reader.read<u16>(), reader);
}

void handle_aurora(ByteReader& reader) noexcept {
  ZoneScoped;
  const u16 subCmd = reader.read<u16>();
  const bool isDraw = subCmd == GX_AURORA_DRAW_SIZED || subCmd == GX_AURORA_DRAW_INDEXED;
  profile::Scope prof{isDraw ? AURORA_FIFO_PROFILE_DRAW : AURORA_FIFO_PROFILE_AURORA};

  if (subCmd == GX_AURORA_LOAD_VIEWPORT_RENDER) {
    const f32 left = reader.read<f32>();
    const f32 top = reader.read<f32>();
    const f32 width = reader.read<f32>();
    const f32 height = reader.read<f32>();
    const f32 nearZ = reader.read<f32>();
    const f32 farZ = reader.read<f32>();
    set_render_viewport({
        .left = left,
        .top = top,
        .width = width,
        .height = height,
        .znear = nearZ,
        .zfar = farZ,
    });
  } else if (subCmd == GX_AURORA_LOAD_SCISSOR_RENDER) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    set_render_scissor({left, top, width, height});
  } else if (subCmd == GX_AURORA_LOAD_PROJECTION_FULL) {
    auto& proj = g_gxState.proj;
    for (int r = 0; r < 4; ++r) {
      for (int c = 0; c < 4; ++c) {
        proj[r][c] = reader.read<f32>();
      }
    }
    // Invalidate projection XF regs
    for (u32 reg = 0x20; reg <= 0x26; ++reg) {
      g_gxState.xfRegValid.reset(reg);
    }
    g_gxState.dirty |= DirtyUniform;
  } else if (subCmd >= GX_AURORA_LOAD_ARRAYBASE && subCmd <= (GX_AURORA_LOAD_ARRAYBASE | 0x0f)) {
    const u32 attrIdx = subCmd - GX_AURORA_LOAD_ARRAYBASE + GX_VA_POS;
    const u64 arrayAddr = reader.read<u64>();
    const u32 arraySize = reader.read<u32>();
    const bool le = reader.read<u8>() == 1;

    auto& array = g_gxState.arrays[attrIdx];
    const auto newData = reinterpret_cast<void*>(arrayAddr);
    if (array.data != newData || array.size != arraySize || array.le != le) {
      if (array.le != le) {
        // Endianness is baked into the shader
        g_gxState.dirty |= DirtyPipeline;
      }
      array.data = newData;
      array.size = arraySize;
      array.le = le;
      array.cachedRange = {};
      g_gxState.dirty |= DirtyImmediates;
    }
  } else if (subCmd == GX_AURORA_LOAD_TEXOBJ) {
    const auto texMapId = reader.read<u8>();
    CHECK(texMapId < MaxTextures, "invalid texture map id {}", texMapId);
    auto& slot = g_gxState.loadedTextures[texMapId];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const u32 newWidth = reader.read<u32>();
    const u32 newHeight = reader.read<u32>();
    const auto newFormat = static_cast<GXTexFmt>(reader.read<u32>());
    const auto newTlut = static_cast<GXTlut>(reader.read<u32>());
    u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (reader.read<u8>() != 0) {
      newFlags |= 1u;
    } else {
      newFlags &= ~1u;
    }
    const u32 newTexObjId = reader.read<u32>();
    const u32 newTexDataVersion = reader.read<u32>();
    if (slot.data != newData || slot.mWidth != newWidth || slot.mHeight != newHeight ||
        slot.mFormat != static_cast<u32>(newFormat) || slot.tlut != newTlut || slot.flags != newFlags ||
        slot.texObjId != newTexObjId || slot.texDataVersion != newTexDataVersion) {
      slot.data = newData;
      slot.mWidth = newWidth;
      slot.mHeight = newHeight;
      slot.mFormat = newFormat;
      slot.tlut = newTlut;
      slot.flags = newFlags;
      slot.texObjId = newTexObjId;
      slot.texDataVersion = newTexDataVersion;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX_AURORA_LOAD_TLUT) {
    const auto idx = reader.read<u8>();
    CHECK(idx < MaxTluts, "invalid tlut slot {}", idx);
    auto& slot = g_gxState.loadedTluts[idx];
    const auto newData = reinterpret_cast<const void*>(reader.read<u64>());
    const auto newFormat = static_cast<GXTlutFmt>(reader.read<u32>());
    const u16 newNumEntries = reader.read<u16>();
    const u32 newTlutObjId = reader.read<u32>();
    const u32 newTlutDataVersion = reader.read<u32>();
    const u8 newFlags = slot.flags & ~0x80u; // Reset no-cache flag
    if (slot.data != newData || slot.format != newFormat || slot.numEntries != newNumEntries ||
        slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion || slot.flags != newFlags) {
      if (slot.tlutObjId != newTlutObjId || slot.tlutDataVersion != newTlutDataVersion) {
        texture::invalidate_bindings();
      }
      slot.data = newData;
      slot.format = newFormat;
      slot.numEntries = newNumEntries;
      slot.tlutObjId = newTlutObjId;
      slot.tlutDataVersion = newTlutDataVersion;
      slot.flags = newFlags;
      g_gxState.dirty |= DirtyTextures;
    }
  } else if (subCmd == GX2_SET_POLYGON_OFFSET) {
    const f32 frontOffset = reader.read<f32>();
    const f32 frontScale = reader.read<f32>();
    const f32 backOffset = reader.read<f32>();
    const f32 backScale = reader.read<f32>();
    const f32 clamp = reader.read<f32>();
    if (g_gxState.frontOffset != frontOffset || g_gxState.frontScale != frontScale ||
        g_gxState.backOffset != backOffset || g_gxState.backScale != backScale || g_gxState.clamp != clamp) {
      g_gxState.frontOffset = frontOffset;
      g_gxState.frontScale = frontScale;
      g_gxState.backOffset = backOffset;
      g_gxState.backScale = backScale;
      g_gxState.clamp = clamp;
      g_gxState.dirty |= DirtyPipeline;
    }
  } else if (subCmd == GX_AURORA_LOAD_COPY_SRC) {
    const s32 left = reader.read<s32>();
    const s32 top = reader.read<s32>();
    const s32 width = reader.read<s32>();
    const s32 height = reader.read<s32>();
    g_gxState.texCopySrc = {left, top, width, height};
  } else if (subCmd == GX_AURORA_LOAD_COPY_DST) {
    g_gxState.texCopyDstWidth = reader.read<u32>();
    g_gxState.texCopyDstHeight = reader.read<u32>();
    g_gxState.texCopyFmt = static_cast<GXTexFmt>(reader.read<u32>());
    reader.skip(1); // mipmap is not implemented, but remains part of the command payload
    g_gxState.texCopyDstWide = true;
  } else if (subCmd == GX_AURORA_LOAD_COPY_DEST) {
    g_gxState.texCopyDest = reinterpret_cast<const void*>(reader.read<u64>());
  } else if (subCmd == GX_AURORA_REQUEST_DEPTH_SNAPSHOT) {
    gfx::depth_peek::request_snapshot();
  } else if (subCmd == GX_AURORA_SET_CONTENT_SCALE) {
    // native/patches/aurora/0009: the following drawing maps to the top-left part of the EFB.
    gfx::set_content_scale(reader.read<f32>());
    set_logical_viewport(g_gxState.logicalViewport);
    set_logical_scissor(g_gxState.logicalScissor);
  } else if (subCmd == GX_AURORA_SET_VIEW_DELTA) {
    // native/patches/aurora/0019: applied when the uniform is built (shader_info.cpp fill_uniform).
    auto& vd = g_gxState.viewDelta;
    const bool active = reader.read<u8>() != 0;
    if (active) {
      f32* flat = reinterpret_cast<f32*>(&vd.delta);
      for (int i = 0; i < 12; ++i) {
        flat[i] = reader.read<f32>();
      }
      for (auto& v : vd.projFrom) {
        v = reader.read<f32>();
      }
      for (auto& v : vd.projTo) {
        v = reader.read<f32>();
      }
    }
    if (active || vd.active) {
      g_gxState.dirty |= DirtyUniform;
    }
    vd.active = active;
  } else if (subCmd == GX_AURORA_BEGIN_OFFSCREEN) {
    const u32 width = reader.read<u32>();
    const u32 height = reader.read<u32>();
    gfx::begin_offscreen(width, height);
  } else if (subCmd == GX_AURORA_END_OFFSCREEN) {
    gfx::end_offscreen();
  } else if (subCmd == GX_AURORA_DESTROY_TEXOBJ) {
    evict_texture_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_TLUT) {
    evict_tlut_object(reader.read<u32>());
  } else if (subCmd == GX_AURORA_DESTROY_COPY_TEX) {
    evict_copy_texture(reinterpret_cast<const void*>(reader.read<u64>()));
  } else if (subCmd == GX_AURORA_DRAW_SIZED) {
    const u8 cmd = reader.read<u8>();
    const u32 byteLen = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    if (byteLen != 0) {
      u32 vtxSize;
      if (g_gxState.lastVtxFmt == fmt) {
        vtxSize = g_gxState.lastVtxSize;
      } else {
        vtxSize = calc_vtx_size(fmt);
      }
      AURORA_ASSERT(vtxSize != 0 && byteLen % vtxSize == 0,
                    "GX_AURORA_DRAW_SIZED: {} bytes is not a whole number of size-{} vertices", byteLen, vtxSize);
      u32 vtxCount = byteLen / vtxSize;
      AURORA_ASSERT(vtxCount <= 0xFFFF, "GX_AURORA_DRAW_SIZED: too many vertices ({})", vtxCount);
      draw_prim(prim, fmt, static_cast<u16>(vtxCount), reader);
    }
  } else if (subCmd == GX_AURORA_DRAW_INDEXED) {
    ZoneScopedN("DRAW_INDEXED");
    const u8 cmd = reader.read<u8>();
    const u16 vtxCount = reader.read<u16>();
    const u32 indexCount = reader.read<u32>();
    const GXVtxFmt fmt = static_cast<GXVtxFmt>(cmd & CP_VAT_MASK);
    const GXPrimitive prim = static_cast<GXPrimitive>(cmd & CP_OPCODE_MASK);
    AURORA_ASSERT(prim == GX_TRIANGLES, "GX_AURORA_DRAW_INDEXED: primitive must be GX_TRIANGLES, got {}",
                  static_cast<u32>(prim));
    const size_t idxBytes = static_cast<size_t>(indexCount) * sizeof(u16);
    // Index data is always host-endian; push it to the GPU buffer as-is
    const auto indexData = reader.take(idxBytes);
    const gfx::Range idxRange = gfx::push_indices(indexData.data(), indexData.size(), 4);
    u32 vtxSize;
    if (g_gxState.lastVtxFmt == fmt) {
      vtxSize = g_gxState.lastVtxSize;
    } else {
      vtxSize = calc_vtx_size(fmt);
    }
    const u32 totalVtxBytes = vtxCount * vtxSize;
    const auto vertexData = reader.take(totalVtxBytes);
    const gfx::Range vertRange = gfx::push_verts(vertexData.data(), vertexData.size(), 4);
    if (indexCount != 0) {
      push_gx_draw(prim, fmt, vtxCount, vertRange, idxRange, indexCount);
    }
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_PUSH) {
    auto label = reader.read_string();
    gfx::push_debug_group(std::move(label));
  } else if (subCmd == GX_AURORA_DEBUG_GROUP_POP) {
    pop_debug_group();
  } else if (subCmd == GX_AURORA_DEBUG_MARKER_INSERT) {
    auto label = reader.read_string();
    gfx::insert_debug_marker(std::move(label));
  }

  else {
    Log.error("Unknown Aurora subcommand: {:04X}", subCmd);
  }
}

void set_fast_paths(bool enabled) noexcept { sFastPaths = enabled; }

void clear_draw_cache() noexcept {
  if (++sPipelineMemoFrame % 600 == 0 && sPipelineMemo.size() > 4096) {
    sPipelineMemo.clear(); // states of scenes gone
  }
  // the memo's texture handles are kept only for their frame
  for (auto it = sBindMemo.begin(); it != sBindMemo.end();) {
    it = it->second.frame + 1 < sPipelineMemoFrame ? sBindMemo.erase(it) : std::next(it);
  }
  g_gxState.viewDelta.active = false; // native/patches/aurora/0019: never past a frame
  sDrawCache.bindGeneration = 0;
  sDrawCache.uniformRange = {};
  sDrawCache.fogRange = {};
  sDrawCache.hasFogRange = false;
}

} // namespace aurora::gx::fifo
