#pragma once

// Draw census (native port, native/patches/aurora/0008): for one requested frame, every GX draw
// with its render pass, the debug marker it follows, a summary of its pipeline and textures, and
// the samples it wrote (an occlusion query per draw: fragments that passed the depth test and the
// alpha compare), plus every render pass and copy of the frame. Written as CSV files after the
// frame's GPU work completed. Off unless aurora_draw_census_request() was called; then it costs
// one occlusion query per draw for that frame only. A diagnostic for GPU cost per pixel: which
// draws cover how much of the screen, with what shader.

#include <webgpu/webgpu_cpp.h>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace aurora::gfx::census {

struct TexInfo {
  uint16_t width = 0;
  uint16_t height = 0;
  uint8_t mips = 0;
  uint8_t gxFormat = 0;
  uint8_t minFilter = 0;
  uint8_t isCopy = 0;
};

struct DrawInfo {
  uint32_t pass = 0;        // index of the render pass when recorded
  int32_t marker = -1;      // index into Capture::markers of the last marker before the draw
  uint32_t prim = 0;
  uint32_t vtxCount = 0;
  uint32_t merges = 0;
  uint64_t pipelineHash = 0;
  uint8_t tevStages = 0;
  uint8_t indStages = 0;
  uint8_t alphaTest = 0;    // the alpha compare can discard
  uint8_t alphaComp0 = 0, alphaRef0 = 0, alphaOp = 0, alphaComp1 = 0, alphaRef1 = 0;
  uint8_t blendMode = 0, blendSrc = 0, blendDst = 0;
  uint8_t depthCompare = 0, depthFunc = 0, depthUpdate = 0;
  uint8_t colorUpdate = 0, alphaUpdate = 0;
  uint8_t cull = 0;
  uint8_t fog = 0;
  uint8_t dstAlpha = 0;     // the draw sets destination alpha (may need an alpha prepass)
  uint8_t numChans = 0;
  uint8_t texCount = 0;
  float vpWidth = 0.f, vpHeight = 0.f;
  TexInfo tex[8]{};
};

struct Capture {
  std::string path;          // CSV path prefix: <path>-draws.csv, <path>-passes.csv
  uint32_t frame = 0;
  uint32_t capacity = 0;
  wgpu::QuerySet querySet;
  std::vector<DrawInfo> draws;           // recording thread
  std::vector<std::string> markers;      // recording thread
  int32_t currentMarker = -1;            // recording thread
  std::vector<std::string> passes;       // render worker: one CSV row per pass or copy
  wgpu::Buffer resolveBuffer;
  wgpu::Buffer readbackBuffer;
  uint32_t resolved = 0;
  wgpu::Buffer counterReadback;
};

// Fragment counters (AURORA_DRAW_CENSUS=1 in the environment at start-up, Metal's occlusion
// queries being boolean): every GX fragment shader counts its invocations and the fragments that
// survive its alpha compare into a storage buffer (group 0 binding 2), slot = the draw immediate's
// census field (0 outside census frames). Shaders, layouts and pipelines change, so only for
// diagnostic runs; off, nothing changes.
bool counters_enabled() noexcept;
constexpr uint32_t kCounterSlots = 16385; // slot 0 is the sink for draws outside a census frame
wgpu::Buffer counter_buffer();

// The capture of the frame being recorded, or nullptr.
Capture* recording() noexcept;
// Called by the recorder at the start and end of each frame.
std::shared_ptr<Capture> begin_frame(uint32_t frameIndex);
void end_frame() noexcept;
// Render worker: after the frame's last pass, resolve the queries and read them back after submit.
void encode_resolve(const wgpu::CommandEncoder& cmd, const std::shared_ptr<Capture>& capture);
void after_submit(const std::shared_ptr<Capture>& capture);

} // namespace aurora::gfx::census

extern "C" {
// Capture the next frame recorded; `pathPrefix` gets "-draws.csv" and "-passes.csv" appended.
void aurora_draw_census_request(const char* pathPrefix);
// 1 while a requested capture has not been written yet.
int aurora_draw_census_pending(void);
}
