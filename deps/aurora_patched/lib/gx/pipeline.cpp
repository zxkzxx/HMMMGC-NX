#include "pipeline.hpp"

#include "../gfx/encoding.hpp"
#include "../gfx/resources.hpp"
#include "../gfx/pipeline_cache.hpp"
#include "../gfx/resource_cache.hpp"

#include "gx_fmt.hpp"
#include "shader_info.hpp"

#include <tracy/Tracy.hpp>
#if defined(AURORA_GFX_DEKO3D)
#include "dk_gfx.hpp"
#endif

namespace aurora::gx {

gfx::CompiledPipeline create_pipeline(const PipelineConfig& config, const gfx::RenderTargetLayout& layout) {
  ZoneScoped;
  const auto hash = xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(gfx::ShaderType::GX)));
#if defined(AURORA_GFX_DEKO3D)
  // the deko3d NRO: the same passes, built by switch/deko/aurora (DKSH from the caches or uam)
  (void)hash;
  const auto create = [&](const PipelineOptions& options, const char*) {
    return gfx::dk::build_gx_pipeline(config, layout, options);
  };
#else
  const auto create = [&](const PipelineOptions& options, const char* passName) {
    const auto shader = build_shader(config.shaderConfig, layout, options.dstAlphaMode);
    const auto label = fmt::format("GX Pipeline {:x} {}", hash, passName);
    return build_pipeline(config, layout, options, {}, shader, label.c_str());
  };
#endif

  PipelineOptions options{
      .colorUpdate = config.colorUpdate,
      .alphaUpdate = config.alphaUpdate,
      .depthUpdate = config.depthUpdate,
  };
  gfx::CompiledPipeline pipeline;
  if (config.alphaUpdate && config.dstAlpha != UINT32_MAX && config.dstAlpha != 0) {
    const auto usesSourceAlpha = [](GXBlendFactor factor) {
      return factor == GX_BL_SRCALPHA || factor == GX_BL_INVSRCALPHA;
    };
    const bool needsSourceAlpha = config.colorUpdate && config.blendMode == GX_BM_BLEND &&
                                  (usesSourceAlpha(config.blendFacSrc) || usesSourceAlpha(config.blendFacDst));
    if (!needsSourceAlpha) {
      options.dstAlphaMode = DstAlphaMode::Replace;
#if defined(AURORA_GFX_DEKO3D)
    } else if (gfx::dk::dual_source_blending() && layout.colorAttachmentCount == 1) {
#else
    } else if (webgpu::g_dualSourceBlendingSupported && layout.colorAttachmentCount == 1) {
#endif
      options.dstAlphaMode = DstAlphaMode::DualSource;
    } else {
      // Write alpha before RGB, no depth write
      auto alpha = options;
      alpha.dstAlphaMode = DstAlphaMode::Replace;
      alpha.colorUpdate = false;
      alpha.depthUpdate = false;
#if defined(AURORA_GFX_DEKO3D)
      pipeline.dkPrepass = create(alpha, "alpha");
      if (!pipeline.dkPrepass) {
        return {};
      }
#else
      pipeline.prepass = create(alpha, "alpha");
      if (!pipeline.prepass) {
        return {};
      }
#endif
      options.alphaUpdate = false;
    }
  }
#if defined(AURORA_GFX_DEKO3D)
  pipeline.dkMain = create(options, "main");
  if (!pipeline.dkMain) {
    return {};
  }
#else
  pipeline.main = create(options, "main");
  if (!pipeline.main) {
    return {};
  }
#endif
  return pipeline;
}

void render(const DrawData& data, const wgpu::RenderPassEncoder& pass) {
  gfx::CompiledPipeline pipeline;
  if (!gfx::get_pipeline(data.pipeline, pipeline)) {
    return;
  }
  const auto& resources = gfx::detail::resources();
  pass.SetImmediates(0, &data.immediateData, sizeof(data.immediateData));
  const std::array offsets{data.uniformRange.offset};
  pass.SetBindGroup(1, resources.uniformBindGroup, offsets.size(), offsets.data());
  if (data.bindGroups.textureBindGroup) {
    pass.SetBindGroup(2, gfx::find_bind_group(data.bindGroups.textureBindGroup));
  }
  pass.SetIndexBuffer(resources.indexBuffer, wgpu::IndexFormat::Uint16, data.idxRange.offset, data.idxRange.size);
  if (data.dstAlpha != UINT32_MAX) {
    const wgpu::Color color{0.f, 0.f, 0.f, data.dstAlpha / 255.f};
    pass.SetBlendConstant(&color);
  }
  const auto draw = [&] {
    if (data.indexCount == 0) {
      pass.Draw(data.vtxCount, data.instanceCount);
    } else {
      pass.DrawIndexed(data.indexCount, data.instanceCount);
    }
  };
  if (pipeline.prepass) {
    gfx::bind_pipeline(pipeline.prepass, pass);
    draw();
  }
  gfx::bind_pipeline(pipeline.main, pass);
  draw();
}

} // namespace aurora::gx
