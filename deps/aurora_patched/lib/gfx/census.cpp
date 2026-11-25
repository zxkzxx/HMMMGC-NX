#include "census.hpp"

#include "../logging.hpp"
#include "../webgpu/gpu.hpp"

#include <algorithm>

#include <atomic>
#include <cstdlib>
#include <cstdio>
#include <mutex>

namespace aurora::gfx::census {
using webgpu::g_device;

namespace {
constexpr Module Log{"aurora::census"};
constexpr uint32_t kCapacity = 16384;

std::mutex g_mutex;
std::string g_requestPath; // guarded by g_mutex
std::atomic_int g_pending{0};
std::shared_ptr<Capture> g_current; // recording thread

void write_csv(const Capture& c, const uint64_t* samples, uint32_t count, const uint32_t* counters) {
  const std::string drawsPath = c.path + "-draws.csv";
  if (FILE* f = std::fopen(drawsPath.c_str(), "w")) {
    std::fprintf(f, "draw,pass,marker,prim,vtx,merges,samples,frags,frags_kept,vp_w,vp_h,pipeline,tev,ind,alpha_test,ac0,ar0,aop,"
                    "ac1,ar1,blend,bsrc,bdst,zcmp,zfunc,zupd,cupd,aupd,cull,fog,dst_alpha,chans,ntex,textures\n");
    for (uint32_t i = 0; i < c.draws.size(); ++i) {
      const auto& d = c.draws[i];
      const uint64_t s = i < count ? samples[i] : 0;
      std::string tex;
      for (uint32_t t = 0; t < d.texCount && t < 8; ++t) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s%ux%u/m%u/f%u/min%u%s", t ? " " : "", d.tex[t].width, d.tex[t].height,
                      d.tex[t].mips, d.tex[t].gxFormat, d.tex[t].minFilter, d.tex[t].isCopy ? "/copy" : "");
        tex += buf;
      }
      const char* marker = d.marker >= 0 && size_t(d.marker) < c.markers.size() ? c.markers[d.marker].c_str() : "";
      const uint32_t frags = counters != nullptr && i < count ? counters[(i + 1) * 2] : 0;
      const uint32_t kept = counters != nullptr && i < count ? counters[(i + 1) * 2 + 1] : 0;
      std::fprintf(f, "%u,%u,%s,%u,%u,%u,%llu,%u,%u,%.0f,%.0f,%016llx,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%u,%s\n",
                   i, d.pass, marker, d.prim, d.vtxCount, d.merges, (unsigned long long)s, frags, kept, d.vpWidth, d.vpHeight,
                   (unsigned long long)d.pipelineHash, d.tevStages, d.indStages, d.alphaTest, d.alphaComp0,
                   d.alphaRef0, d.alphaOp, d.alphaComp1, d.alphaRef1, d.blendMode, d.blendSrc, d.blendDst,
                   d.depthCompare, d.depthFunc, d.depthUpdate, d.colorUpdate, d.alphaUpdate, d.cull, d.fog,
                   d.dstAlpha, d.numChans, d.texCount, tex.c_str());
    }
    std::fclose(f);
  } else {
    Log.warn("census: cannot write {}", drawsPath);
  }
  const std::string passesPath = c.path + "-passes.csv";
  if (FILE* f = std::fopen(passesPath.c_str(), "w")) {
    std::fprintf(f, "op,kind,label,width,height,attachments,load,draws,resolve,detail\n");
    for (const auto& row : c.passes) {
      std::fprintf(f, "%s\n", row.c_str());
    }
    std::fclose(f);
  }
  Log.info("census: frame {} written to {}-*.csv ({} draws, {} passes/copies)", c.frame, c.path, c.draws.size(),
           c.passes.size());
}
} // namespace

bool counters_enabled() noexcept {
  static const bool enabled = [] {
    const char* v = std::getenv("AURORA_DRAW_CENSUS");
    return v != nullptr && v[0] == '1';
  }();
  return enabled;
}

wgpu::Buffer counter_buffer() {
  static wgpu::Buffer buffer;
  if (!buffer && counters_enabled()) {
    const wgpu::BufferDescriptor desc{
        .label = "Draw census counters",
        .usage = wgpu::BufferUsage::Storage | wgpu::BufferUsage::CopySrc | wgpu::BufferUsage::CopyDst,
        .size = uint64_t(kCounterSlots) * 2 * sizeof(uint32_t),
    };
    buffer = g_device.CreateBuffer(&desc);
  }
  return buffer;
}

Capture* recording() noexcept { return g_current.get(); }

std::shared_ptr<Capture> begin_frame(uint32_t frameIndex) {
  g_current.reset();
  std::string path;
  {
    std::lock_guard lock{g_mutex};
    if (g_requestPath.empty()) {
      return nullptr;
    }
    path = std::move(g_requestPath);
    g_requestPath.clear();
  }
  auto capture = std::make_shared<Capture>();
  capture->path = std::move(path);
  capture->frame = frameIndex;
  capture->capacity = kCapacity;
  const wgpu::QuerySetDescriptor qsDesc{
      .label = "Draw census",
      .type = wgpu::QueryType::Occlusion,
      .count = kCapacity,
  };
  capture->querySet = g_device.CreateQuerySet(&qsDesc);
  capture->draws.reserve(4096);
  g_current = capture;
  return capture;
}

void end_frame() noexcept { g_current.reset(); }

void encode_resolve(const wgpu::CommandEncoder& cmd, const std::shared_ptr<Capture>& capture) {
  const uint32_t count = std::min<uint32_t>(static_cast<uint32_t>(capture->draws.size()), capture->capacity);
  capture->resolved = count;
  if (count == 0) {
    return;
  }
  const uint64_t size = uint64_t(count) * sizeof(uint64_t);
  const wgpu::BufferDescriptor resolveDesc{
      .label = "Draw census resolve",
      .usage = wgpu::BufferUsage::QueryResolve | wgpu::BufferUsage::CopySrc,
      .size = size,
  };
  capture->resolveBuffer = g_device.CreateBuffer(&resolveDesc);
  const wgpu::BufferDescriptor readDesc{
      .label = "Draw census readback",
      .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
      .size = size,
  };
  capture->readbackBuffer = g_device.CreateBuffer(&readDesc);
  cmd.ResolveQuerySet(capture->querySet, 0, count, capture->resolveBuffer, 0);
  cmd.CopyBufferToBuffer(capture->resolveBuffer, 0, capture->readbackBuffer, 0, size);
  if (counters_enabled()) {
    const uint64_t counterSize = uint64_t(count + 1) * 2 * sizeof(uint32_t);
    const wgpu::BufferDescriptor counterDesc{
        .label = "Draw census counter readback",
        .usage = wgpu::BufferUsage::MapRead | wgpu::BufferUsage::CopyDst,
        .size = counterSize,
    };
    capture->counterReadback = g_device.CreateBuffer(&counterDesc);
    cmd.CopyBufferToBuffer(counter_buffer(), 0, capture->counterReadback, 0, counterSize);
    cmd.ClearBuffer(counter_buffer(), 0, counterSize);
  }
}

void after_submit(const std::shared_ptr<Capture>& capture) {
  if (capture->resolved == 0) {
    write_csv(*capture, nullptr, 0, nullptr);
    g_pending.fetch_sub(1);
    return;
  }
  const uint64_t size = uint64_t(capture->resolved) * sizeof(uint64_t);
  capture->readbackBuffer.MapAsync(
      wgpu::MapMode::Read, 0, size, wgpu::CallbackMode::AllowSpontaneous,
      [capture, size](wgpu::MapAsyncStatus status, wgpu::StringView message) {
        if (status != wgpu::MapAsyncStatus::Success) {
          Log.warn("census: readback failed: {}", std::string_view{message});
          g_pending.fetch_sub(1);
          return;
        }
        if (!capture->counterReadback) {
          const auto* samples = static_cast<const uint64_t*>(capture->readbackBuffer.GetConstMappedRange(0, size));
          write_csv(*capture, samples, capture->resolved, nullptr);
          capture->readbackBuffer.Unmap();
          g_pending.fetch_sub(1);
          return;
        }
        const uint64_t counterSize = uint64_t(capture->resolved + 1) * 2 * sizeof(uint32_t);
        capture->counterReadback.MapAsync(
            wgpu::MapMode::Read, 0, counterSize, wgpu::CallbackMode::AllowSpontaneous,
            [capture, size, counterSize](wgpu::MapAsyncStatus status2, wgpu::StringView) {
              const auto* samples =
                  static_cast<const uint64_t*>(capture->readbackBuffer.GetConstMappedRange(0, size));
              const auto* counters = status2 == wgpu::MapAsyncStatus::Success
                                         ? static_cast<const uint32_t*>(
                                               capture->counterReadback.GetConstMappedRange(0, counterSize))
                                         : nullptr;
              write_csv(*capture, samples, capture->resolved, counters);
              capture->readbackBuffer.Unmap();
              if (counters != nullptr) {
                capture->counterReadback.Unmap();
              }
              g_pending.fetch_sub(1);
            });
      });
}

} // namespace aurora::gfx::census

void aurora_draw_census_request(const char* pathPrefix) {
  if (pathPrefix == nullptr || *pathPrefix == '\0') {
    return;
  }
  std::lock_guard lock{aurora::gfx::census::g_mutex};
  if (aurora::gfx::census::g_requestPath.empty()) {
    aurora::gfx::census::g_pending.fetch_add(1);
  }
  aurora::gfx::census::g_requestPath = pathPrefix;
}

int aurora_draw_census_pending(void) { return aurora::gfx::census::g_pending.load() > 0 ? 1 : 0; }
