#pragma once

#include "types.hpp"

namespace aurora::gfx::clear {
struct PipelineConfig;
} // namespace aurora::gfx::clear

namespace aurora::gx {
struct PipelineConfig;
} // namespace aurora::gx

namespace aurora::rmlui {
struct PipelineConfig;
} // namespace aurora::rmlui

namespace aurora::gfx {

enum class ShaderType : uint8_t {
  Clear = 0,
  GX = 1,
  Rml = 2,
};

struct CompiledPipeline {
  wgpu::RenderPipeline main;
  wgpu::RenderPipeline prepass;
#if defined(AURORA_GFX_DEKO3D)
  // The deko3d NRO's pipelines (switch/deko/aurora/dk_gfx.hpp); main and prepass stay null there.
  const void* dkMain = nullptr;
  const void* dkPrepass = nullptr;
#endif

  [[nodiscard]] uint32_t pipeline_count() const {
#if defined(AURORA_GFX_DEKO3D)
    return static_cast<uint32_t>(dkMain != nullptr) + static_cast<uint32_t>(dkPrepass != nullptr);
#else
    return static_cast<uint32_t>(static_cast<bool>(main)) + static_cast<uint32_t>(static_cast<bool>(prepass));
#endif
  }
};

void initialize_pipeline_cache();
void shutdown_pipeline_cache();
// aurora_save_unbuilt_pipelines (include/aurora/pipeline_cache_save.h)
void save_unbuilt_pipelines();
void begin_pipeline_frame();
void end_pipeline_frame();
void rebuild_pipeline_cache();

PipelineRef find_pipeline(const gx::PipelineConfig& config, const RenderTargetLayout& layout);
PipelineRef find_pipeline(const clear::PipelineConfig& config, const RenderTargetLayout& layout);
PipelineRef find_pipeline(const rmlui::PipelineConfig& config);

bool get_pipeline(PipelineRef ref, CompiledPipeline& pipeline);

// Switch: a frame was presented (aurora.cpp, after Present); the warm-up throttle
// (aurora_switch_set_warmup_throttle) starts its next build right after one.
void switch_warmup_note_present();

} // namespace aurora::gfx
