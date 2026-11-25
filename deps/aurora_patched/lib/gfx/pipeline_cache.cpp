#include "pipeline_cache.hpp"

#include "clear.hpp"
#include "frame_packet.hpp"
#include "resources.hpp"
#include "hash.hpp"
#include "../gx/pipeline.hpp"
#include "../gx/shader_info.hpp"
#include <aurora/pipeline_cache_save.h>
#include "../io.hpp"
#ifdef AURORA_ENABLE_RMLUI
#include "../rmlui/pipeline.hpp"
#endif
#include "../sqlite_utils.hpp"
#include "../webgpu/gpu.hpp"
#include <aurora/switch_precompile.h>
#include <aurora/switch_stats.h>
#if defined(AURORA_GFX_DEKO3D)
#include "dk_gfx.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <limits>
#include <mutex>
#include <optional>
#include <ranges>
#include <variant>
#include <thread>

#include <SDL3/SDL_iostream.h>
#include <absl/container/flat_hash_map.h>
#include <absl/container/flat_hash_set.h>
#include <fmt/format.h>
#include <tracy/Tracy.hpp>

namespace aurora::gfx {
static Module Log("aurora::gfx::pipeline_cache");

constexpr int PipelineCacheSchema = 1;
constexpr const char* InitialPipelineCacheName = "initial_pipeline_cache.db";
constexpr const char* SdlVfsName = "aurora_pipeline_cache_sdl_vfs";

using NewPipelineCallback = std::function<CompiledPipeline()>;

struct PendingPipeline {
  PipelineRef hash;
  NewPipelineCallback create;
};

struct PipelineCacheWrite {
  ShaderType type;
  PipelineRef hash;
  uint32_t configVersion;
  ByteBuffer config;
  uint32_t firstFrameUsed = UINT32_MAX;
};

using SavedPipelineConfig = std::variant<gx::PipelineConfig, clear::PipelineConfig
#ifdef AURORA_ENABLE_RMLUI
                                         ,
                                         rmlui::PipelineConfig
#endif
                                         >;

struct KnownPipeline {
  ShaderType type;
  SavedPipelineConfig config;
  uint32_t firstFrameUsed;
};

struct SdlVfsSqliteFile {
  sqlite3_file base;
  SDL_IOStream* io = nullptr;
};

static std::mutex g_pipelineMutex;
static bool g_hasPipelineThread = false;
static size_t g_pipelinesPerFrame = 0;
// For synchronous pipeline fallback (OpenGL)
#ifdef NDEBUG
constexpr size_t BuildPipelinesPerFrame = 5;
#else
constexpr size_t BuildPipelinesPerFrame = 1;
#endif
static std::thread g_pipelineThread;
static std::atomic_bool g_pipelineThreadEnd = false;
static std::condition_variable g_pipelineQueueCv;
static std::condition_variable g_pipelineReadyCv;
static absl::flat_hash_map<PipelineRef, CompiledPipeline> g_pipelines;
static absl::flat_hash_map<HashType, KnownPipeline> g_knownPipelines;
// Pipelines first seen this run and not built yet (cache key -> first frame used): each is written
// to the cache file only once its build returns (persist_built_pipeline), so a pipeline whose build
// crashes the process never reaches the file, where every later start's warm-up would build it
// again and crash before the game starts. Locked after g_pipelineMutex when both are held.
static std::mutex g_unsavedPipelinesMutex;
static absl::flat_hash_map<HashType, uint32_t> g_unsavedPipelines;
static std::optional<uint64_t> g_pipelineLayoutKey;
static std::deque<PendingPipeline> g_pipelineQueue;
static std::deque<PendingPipeline> g_backgroundPipelineQueue;
static absl::flat_hash_set<PipelineRef> g_pendingPipelines;
static std::atomic_bool g_gpuCachePrunePending = false;

// Switch: the boot warm-up (switch/native/aurora/patches, aurora_switch_get_precompile). The
// pipelines rebuild_pipeline_cache queued in the background and not built yet; those built, by the
// warm-up or first for a draw; those dropped by aurora_switch_stop_precompile. Under g_pipelineMutex.
static absl::flat_hash_set<PipelineRef> g_warmupPending;
static uint32_t g_warmupTotal = 0;
static uint32_t g_warmupDone = 0;
static uint32_t g_warmupDropped = 0;
static uint64_t g_warmupNs = 0;
static uint64_t g_warmupWaitNs = 0; // of g_warmupNs, waiting for the GL context
static bool g_warmupStopped = false;

// Switch: the last slow builds (aurora_switch_get_slow_builds), under g_pipelineMutex.
static constexpr uint32_t kSlowBuildRing = 32;
static AuroraSwitchSlowBuild g_slowBuilds[kSlowBuildRing];
static uint64_t g_slowBuildSeq = 0;

// Dawn's per-thread render pipeline build ns (dawn-switch-gl-pipeline-wait.patch): translation,
// waiting for the GL context, holding it. Weak: zeros without that patch.
extern "C" void dawn_switch_gl_thread_pipeline_ns(uint64_t out[3]) __attribute__((weak));

struct BuildSplit {
  uint64_t translateNs = 0;
  uint64_t waitNs = 0;
  uint64_t heldNs = 0;
};

static BuildSplit thread_build_ns() {
  uint64_t v[3] = {0, 0, 0};
  if (dawn_switch_gl_thread_pipeline_ns != nullptr) {
    dawn_switch_gl_thread_pipeline_ns(v);
  }
  return {v[0], v[1], v[2]};
}

static BuildSplit build_split_since(const BuildSplit& before) {
  const BuildSplit after = thread_build_ns();
  return {after.translateNs - before.translateNs, after.waitNs - before.waitNs, after.heldNs - before.heldNs};
}

// The priority set (AuroraSwitchPrecompile::priorityTotal): the cache keys the bundled cache's
// pipeline_priority table lists with priority 0, read by seed_pipeline_cache; the runtime pipelines
// the warm-up queued for them, and of those the ones still to build.
static absl::flat_hash_set<HashType> g_priorityKeys;
static absl::flat_hash_set<PipelineRef> g_priorityQueued;
static absl::flat_hash_set<PipelineRef> g_priorityPending;
static uint32_t g_priorityDone = 0;

static uint64_t g_slowBuildNs = 25'000'000; // aurora_switch_set_warmup_slow_ns (g_warmupSlowNs's copy)

static void warmup_built(PipelineRef hash, uint64_t ns, const BuildSplit& split = {}) {
  const bool warmup = g_warmupPending.erase(hash) != 0;
  if (warmup) {
    ++g_warmupDone;
    g_warmupNs += ns;
    g_warmupWaitNs += std::min(split.waitNs, ns);
  }
  if (g_slowBuildNs != 0 && ns > g_slowBuildNs) {
    AuroraSwitchSlowBuild& b = g_slowBuilds[g_slowBuildSeq % kSlowBuildRing];
    b.seq = ++g_slowBuildSeq;
    b.hash = static_cast<uint64_t>(hash);
    b.ns = ns;
    b.translateNs = split.translateNs;
    b.waitNs = split.waitNs;
    b.heldNs = split.heldNs;
    b.warmupDone = g_warmupDone;
    b.warmup = warmup;
  }
  if (g_priorityPending.erase(hash) != 0) {
    ++g_priorityDone;
  }
}

// rebuild_pipeline_cache queued (or found built) runtime pipeline ref for a priority cache key.
static void warmup_priority_queued(PipelineRef ref) {
  std::lock_guard lock{g_pipelineMutex};
  if (!g_priorityQueued.insert(ref).second) {
    return;
  }
  if (g_pendingPipelines.contains(ref)) {
    g_priorityPending.insert(ref);
  } else {
    ++g_priorityDone; // built already
  }
}

static sqlite3* g_pipelineCacheDb = nullptr;
static sqlite3_stmt* g_pipelineCacheLoadStmt = nullptr;
static sqlite3_stmt* g_pipelineCacheUpsertStmt = nullptr;
static bool g_pipelineCacheBroken = false;
static std::thread g_pipelineCacheWriterThread;
static std::condition_variable g_pipelineCacheWriterCv;
static std::mutex g_pipelineCacheWriterMutex;
static std::deque<PipelineCacheWrite> g_pipelineCacheWriteQueue;
static bool g_pipelineCacheWriterStop = false;
static int g_sdlVfsRegisterResult = SQLITE_ERROR;

static SdlVfsSqliteFile* sdl_vfs_file(sqlite3_file* file) { return reinterpret_cast<SdlVfsSqliteFile*>(file); }

static sqlite3_vfs* default_vfs(sqlite3_vfs* vfs) { return static_cast<sqlite3_vfs*>(vfs->pAppData); }

static int sdl_vfs_close(sqlite3_file* file) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io != nullptr) {
    SDL_CloseIO(vfsFile->io);
    vfsFile->io = nullptr;
  }
  return SQLITE_OK;
}

static int sdl_vfs_read(sqlite3_file* file, void* buffer, int amount, sqlite3_int64 offset) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || offset < 0 || amount < 0) {
    return SQLITE_IOERR_READ;
  }
  if (SDL_SeekIO(vfsFile->io, offset, SDL_IO_SEEK_SET) < 0) {
    return SQLITE_IOERR_SEEK;
  }

  auto* dst = static_cast<uint8_t*>(buffer);
  int total = 0;
  while (total < amount) {
    const size_t read = SDL_ReadIO(vfsFile->io, dst + total, static_cast<size_t>(amount - total));
    if (read == 0) {
      if (SDL_GetIOStatus(vfsFile->io) == SDL_IO_STATUS_EOF) {
        std::memset(dst + total, 0, static_cast<size_t>(amount - total));
        return SQLITE_IOERR_SHORT_READ;
      }
      return SQLITE_IOERR_READ;
    }
    if (read > static_cast<size_t>(std::numeric_limits<int>::max() - total)) {
      return SQLITE_IOERR_READ;
    }
    total += static_cast<int>(read);
  }
  return SQLITE_OK;
}

static int sdl_vfs_write(sqlite3_file*, const void*, int, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_truncate(sqlite3_file*, sqlite3_int64) { return SQLITE_READONLY; }

static int sdl_vfs_sync(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_file_size(sqlite3_file* file, sqlite3_int64* size) {
  auto* vfsFile = sdl_vfs_file(file);
  if (vfsFile->io == nullptr || size == nullptr) {
    return SQLITE_IOERR_FSTAT;
  }

  const auto ioSize = SDL_GetIOSize(vfsFile->io);
  if (ioSize < 0) {
    return SQLITE_IOERR_FSTAT;
  }
  *size = static_cast<sqlite3_int64>(ioSize);
  return SQLITE_OK;
}

static int sdl_vfs_lock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_unlock(sqlite3_file*, int) { return SQLITE_OK; }

static int sdl_vfs_check_reserved_lock(sqlite3_file*, int* reserved) {
  if (reserved != nullptr) {
    *reserved = 0;
  }
  return SQLITE_OK;
}

static int sdl_vfs_file_control(sqlite3_file*, int op, void* arg) {
  switch (op) {
  case SQLITE_FCNTL_LOCKSTATE:
    *static_cast<int*>(arg) = SQLITE_LOCK_NONE;
    return SQLITE_OK;
  case SQLITE_FCNTL_HAS_MOVED:
    *static_cast<int*>(arg) = 0;
    return SQLITE_OK;
  case SQLITE_FCNTL_SIZE_HINT:
    return SQLITE_OK;
  default:
    return SQLITE_NOTFOUND;
  }
}

static int sdl_vfs_sector_size(sqlite3_file*) { return 4096; }

static int sdl_vfs_device_characteristics(sqlite3_file*) {
  return SQLITE_IOCAP_IMMUTABLE | SQLITE_IOCAP_UNDELETABLE_WHEN_OPEN;
}

static constexpr sqlite3_io_methods SdlVfsIoMethods{
    .iVersion = 1,
    .xClose = sdl_vfs_close,
    .xRead = sdl_vfs_read,
    .xWrite = sdl_vfs_write,
    .xTruncate = sdl_vfs_truncate,
    .xSync = sdl_vfs_sync,
    .xFileSize = sdl_vfs_file_size,
    .xLock = sdl_vfs_lock,
    .xUnlock = sdl_vfs_unlock,
    .xCheckReservedLock = sdl_vfs_check_reserved_lock,
    .xFileControl = sdl_vfs_file_control,
    .xSectorSize = sdl_vfs_sector_size,
    .xDeviceCharacteristics = sdl_vfs_device_characteristics,
};

static int sdl_vfs_open(sqlite3_vfs*, sqlite3_filename name, sqlite3_file* file, int flags, int* outFlags) {
  auto* vfsFile = sdl_vfs_file(file);
  vfsFile->base.pMethods = nullptr;
  vfsFile->io = nullptr;

  if (name == nullptr || (flags & SQLITE_OPEN_READWRITE) != 0 || (flags & SQLITE_OPEN_READONLY) == 0) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->io = SDL_IOFromFile(name, "rb");
  if (vfsFile->io == nullptr) {
    return SQLITE_CANTOPEN;
  }

  vfsFile->base.pMethods = &SdlVfsIoMethods;
  if (outFlags != nullptr) {
    *outFlags = SQLITE_OPEN_READONLY;
  }
  return SQLITE_OK;
}

static int sdl_vfs_delete(sqlite3_vfs*, const char*, int) { return SQLITE_READONLY; }

static int sdl_vfs_access(sqlite3_vfs*, const char* name, int flags, int* result) {
  if (result == nullptr) {
    return SQLITE_IOERR_ACCESS;
  }
  if (name == nullptr || flags == SQLITE_ACCESS_READWRITE) {
    *result = 0;
    return SQLITE_OK;
  }

  auto* io = SDL_IOFromFile(name, "rb");
  *result = io != nullptr ? 1 : 0;
  if (io != nullptr) {
    SDL_CloseIO(io);
  }
  return SQLITE_OK;
}

static int sdl_vfs_full_pathname(sqlite3_vfs*, const char* name, int outSize, char* out) {
  if (name == nullptr || out == nullptr || outSize <= 0) {
    return SQLITE_CANTOPEN;
  }
  sqlite3_snprintf(outSize, out, "%s", name);
  return SQLITE_OK;
}

static void* sdl_vfs_dl_open(sqlite3_vfs*, const char*) { return nullptr; }

static void sdl_vfs_dl_error(sqlite3_vfs*, int bytes, char* message) {
  if (message != nullptr && bytes > 0) {
    sqlite3_snprintf(bytes, message, "%s", "Dynamic loading is unsupported");
  }
}

static void (*sdl_vfs_dl_sym(sqlite3_vfs*, void*, const char*))(void) { return nullptr; }

static void sdl_vfs_dl_close(sqlite3_vfs*, void*) {}

static int sdl_vfs_randomness(sqlite3_vfs* vfs, int bytes, char* out) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xRandomness != nullptr) {
    return base->xRandomness(base, bytes, out);
  }
  if (out != nullptr && bytes > 0) {
    std::memset(out, 0, static_cast<size_t>(bytes));
  }
  return bytes;
}

static int sdl_vfs_sleep(sqlite3_vfs* vfs, int microseconds) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xSleep != nullptr) {
    return base->xSleep(base, microseconds);
  }
  return microseconds;
}

static int sdl_vfs_current_time(sqlite3_vfs* vfs, double* time) {
  if (auto* base = default_vfs(vfs); base != nullptr && base->xCurrentTime != nullptr) {
    return base->xCurrentTime(base, time);
  }
  if (time != nullptr) {
    *time = 2440587.5;
  }
  return SQLITE_OK;
}

static int sdl_vfs_get_last_error(sqlite3_vfs*, int, char*) { return SQLITE_OK; }

static bool register_sdl_vfs() {
  static std::once_flag registerOnce;
  std::call_once(registerOnce, [] {
    auto* baseVfs = sqlite3_vfs_find(nullptr);
    static sqlite3_vfs sdlVfs{
        .iVersion = 1,
        .szOsFile = static_cast<int>(sizeof(SdlVfsSqliteFile)),
        .mxPathname = 4096,
        .pNext = nullptr,
        .zName = SdlVfsName,
        .pAppData = baseVfs,
        .xOpen = sdl_vfs_open,
        .xDelete = sdl_vfs_delete,
        .xAccess = sdl_vfs_access,
        .xFullPathname = sdl_vfs_full_pathname,
        .xDlOpen = sdl_vfs_dl_open,
        .xDlError = sdl_vfs_dl_error,
        .xDlSym = sdl_vfs_dl_sym,
        .xDlClose = sdl_vfs_dl_close,
        .xRandomness = sdl_vfs_randomness,
        .xSleep = sdl_vfs_sleep,
        .xCurrentTime = sdl_vfs_current_time,
        .xGetLastError = sdl_vfs_get_last_error,
    };
    g_sdlVfsRegisterResult = sqlite3_vfs_register(&sdlVfs, 0);
  });
  return g_sdlVfsRegisterResult == SQLITE_OK;
}

#if defined(__cpp_lib_atomic_ref)
static std::atomic_ref queuedPipelines{detail::resources().stats.queuedPipelines};
static std::atomic_ref createdPipelines{detail::resources().stats.createdPipelines};
#else
struct AtomicStatRef {
  uint32_t& ref;
  uint32_t operator++() { return __atomic_add_fetch(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator--() { return __atomic_sub_fetch(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator++(int) { return __atomic_fetch_add(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator--(int) { return __atomic_fetch_sub(&ref, 1, __ATOMIC_RELAXED); }
  uint32_t operator+=(uint32_t val) { return __atomic_add_fetch(&ref, val, __ATOMIC_RELAXED); }
  uint32_t operator=(uint32_t val) {
    __atomic_store_n(&ref, val, __ATOMIC_RELAXED);
    return val;
  }
};
static AtomicStatRef queuedPipelines{detail::resources().stats.queuedPipelines};
static AtomicStatRef createdPipelines{detail::resources().stats.createdPipelines};
#endif

template <typename PipelineConfig>
static PipelineCacheWrite make_pipeline_cache_write(ShaderType type, PipelineRef hash, const PipelineConfig& config,
                                                    uint32_t firstFrameUsed) {
  static_assert(std::has_unique_object_representations_v<PipelineConfig>);

  PipelineCacheWrite write{
      .type = type,
      .hash = hash,
      .configVersion = config.version,
      .config = ByteBuffer(sizeof(config)),
      .firstFrameUsed = firstFrameUsed,
  };
  std::memcpy(write.config.data(), &config, sizeof(config));
  return write;
}

static void enqueue_pipeline_cache_write(PipelineCacheWrite write) {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr) {
    return;
  }

  {
    std::lock_guard lock{g_pipelineCacheWriterMutex};
    g_pipelineCacheWriteQueue.emplace_back(std::move(write));
  }
  g_pipelineCacheWriterCv.notify_one();
}

template <typename Queue>
static auto find_pending_pipeline(Queue& queue, PipelineRef hash) {
  return std::find_if(queue.begin(), queue.end(), [=](const PendingPipeline& pending) { return pending.hash == hash; });
}

enum class PipelinePriority {
  Background, // cache warmup
  Normal,     // async skip draw
  Blocking,   // block until compiled
};

static void promote_pending_pipeline(PipelineRef hash, PipelinePriority priority) {
  if (priority == PipelinePriority::Background) {
    return;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelineQueue, hash);
  if (backgroundIt == g_backgroundPipelineQueue.end()) {
    return;
  }
  if (priority == PipelinePriority::Blocking) {
    g_pipelineQueue.emplace_front(std::move(*backgroundIt));
  } else {
    g_pipelineQueue.emplace_back(std::move(*backgroundIt));
  }
  g_backgroundPipelineQueue.erase(backgroundIt);
}

static std::optional<PendingPipeline> take_pending_pipeline(PipelineRef hash) {
  auto priorityIt = find_pending_pipeline(g_pipelineQueue, hash);
  if (priorityIt != g_pipelineQueue.end()) {
    PendingPipeline pending = std::move(*priorityIt);
    g_pipelineQueue.erase(priorityIt);
    g_pendingPipelines.erase(hash);
    return pending;
  }

  auto backgroundIt = find_pending_pipeline(g_backgroundPipelineQueue, hash);
  if (backgroundIt != g_backgroundPipelineQueue.end()) {
    PendingPipeline pending = std::move(*backgroundIt);
    g_backgroundPipelineQueue.erase(backgroundIt);
    g_pendingPipelines.erase(hash);
    return pending;
  }

  return std::nullopt;
}

static void notify_pipeline_ready(bool queued, uint32_t pipelineCount) {
  createdPipelines += pipelineCount;
  if (queued && --queuedPipelines == 0 && g_gpuCachePrunePending.exchange(false, std::memory_order_acq_rel)) {
    // Prune GPU cache entries after fully loading the pipeline cache.
    webgpu::cache_prune();
  }
  g_pipelineReadyCv.notify_all();
}

// persist: write a new or earlier-used pipeline to the cache file; with saveWhenBuilt (GX pipelines,
// whose shaders the GX generator builds) a new one waits for its build (g_unsavedPipelines).
template <typename Config>
static void remember_pipeline_config(ShaderType type, const Config& config, uint32_t firstFrameUsed, bool persist,
                                     bool saveWhenBuilt = false) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  bool changed = false;
  {
    std::lock_guard lock{g_pipelineMutex};
    auto [it, inserted] = g_knownPipelines.try_emplace(cacheKey, KnownPipeline{type, config, firstFrameUsed});
    changed = inserted || firstFrameUsed < it->second.firstFrameUsed;
    it->second.firstFrameUsed = std::min(it->second.firstFrameUsed, firstFrameUsed);
    if (persist && changed && saveWhenBuilt) {
      std::lock_guard unsavedLock{g_unsavedPipelinesMutex};
      if (inserted) {
        g_unsavedPipelines.emplace(cacheKey, firstFrameUsed);
        return;
      }
      if (auto unsaved = g_unsavedPipelines.find(cacheKey); unsaved != g_unsavedPipelines.end()) {
        unsaved->second = std::min(unsaved->second, firstFrameUsed);
        return;
      }
    }
  }
  if (persist && changed) {
    enqueue_pipeline_cache_write(make_pipeline_cache_write(type, cacheKey, config, firstFrameUsed));
  }
}

// Writes a pipeline first seen this run once its build has returned (g_unsavedPipelines).
template <typename Config>
static void persist_built_pipeline(ShaderType type, const Config& config) {
  const auto cacheKey = xxh3_hash(config, static_cast<HashType>(type));
  uint32_t firstFrameUsed = 0;
  {
    std::lock_guard lock{g_unsavedPipelinesMutex};
    auto it = g_unsavedPipelines.find(cacheKey);
    if (it == g_unsavedPipelines.end()) {
      return;
    }
    firstFrameUsed = it->second;
    g_unsavedPipelines.erase(it);
  }
  enqueue_pipeline_cache_write(make_pipeline_cache_write(type, cacheKey, config, firstFrameUsed));
}

static PipelineRef find_pipeline_impl(PipelineRef runtimeKey, NewPipelineCallback&& cb,
                                      PipelinePriority priority = PipelinePriority::Normal) {
  ZoneScoped;

  const bool blocking = priority == PipelinePriority::Blocking;
  bool notifyWorker = false;
  bool pipelineReady = false;
  bool createdPipeline = false;
  uint32_t pipelineCount = 0;
  bool queued = false;
  {
    std::scoped_lock guard{g_pipelineMutex};
    auto pipelineIt = g_pipelines.find(runtimeKey);
    if (pipelineIt != g_pipelines.end()) {
      pipelineReady = true;
    } else if (g_pendingPipelines.contains(runtimeKey)) {
      if (blocking && !g_hasPipelineThread) {
        auto pending = take_pending_pipeline(runtimeKey);
        if (pending) {
          const uint64_t compileStart = switch_stats::now_ns();
          auto result = pending->create();
          switch_stats::add_pipeline_compile(switch_stats::now_ns() - compileStart);
          warmup_built(runtimeKey, switch_stats::now_ns() - compileStart);
          pipelineCount = result.pipeline_count();
          g_pipelines.try_emplace(runtimeKey, std::move(result));
          pipelineReady = true;
          g_pipelinesPerFrame += std::max(1u, pipelineCount);
          createdPipeline = true;
          queued = true;
        }
      } else {
        promote_pending_pipeline(runtimeKey, priority);
        notifyWorker = priority != PipelinePriority::Background;
      }
    } else if (!g_hasPipelineThread && (blocking || g_pipelinesPerFrame < BuildPipelinesPerFrame)) {
      const uint64_t compileStart = switch_stats::now_ns();
      auto result = cb();
      switch_stats::add_pipeline_compile(switch_stats::now_ns() - compileStart);
      pipelineCount = result.pipeline_count();
      g_pipelines.try_emplace(runtimeKey, std::move(result));
      pipelineReady = true;
      g_pipelinesPerFrame += std::max(1u, pipelineCount);
      createdPipeline = true;
    } else {
      PendingPipeline pending{
          .hash = runtimeKey,
          .create = std::move(cb),
      };
      switch (priority) {
      case PipelinePriority::Background:
        g_backgroundPipelineQueue.emplace_back(std::move(pending));
        if (g_warmupPending.insert(runtimeKey).second) {
          ++g_warmupTotal;
        }
        break;
      case PipelinePriority::Normal:
        g_pipelineQueue.emplace_back(std::move(pending));
        break;
      case PipelinePriority::Blocking:
        g_pipelineQueue.emplace_front(std::move(pending));
        break;
      }
      g_pendingPipelines.insert(runtimeKey);
      ++queuedPipelines;
      notifyWorker = true;
    }
  }

  if (createdPipeline) {
    notify_pipeline_ready(queued, pipelineCount);
  }

  if (notifyWorker) {
    g_pipelineQueueCv.notify_one();
  }

  if (blocking && !pipelineReady) {
    std::unique_lock lock{g_pipelineMutex};
    g_pipelineReadyCv.wait(lock, [=] { return g_pipelines.contains(runtimeKey) || g_pipelineThreadEnd; });
  }
  return runtimeKey;
}

static PipelineRef resolve_pipeline(ShaderType type, const gx::PipelineConfig& config, const RenderTargetLayout& layout,
                                    PipelinePriority priority) {
  const auto runtimeKey = xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(type)));
  return find_pipeline_impl(
      runtimeKey,
      [type, config, layout] {
        auto pipeline = create_pipeline(config, layout);
        persist_built_pipeline(type, config);
        return pipeline;
      },
      priority);
}

static PipelineRef resolve_pipeline(ShaderType type, const clear::PipelineConfig& config,
                                    const RenderTargetLayout& layout, PipelinePriority priority) {
  const auto runtimeKey = xxh3_hash(layout.key, xxh3_hash(config, static_cast<HashType>(type)));
#if defined(AURORA_GFX_DEKO3D)
  return find_pipeline_impl(
      runtimeKey, [config, layout] { return CompiledPipeline{.dkMain = dk::build_clear_pipeline(config, layout)}; },
      priority);
#else
  return find_pipeline_impl(
      runtimeKey, [config, layout] { return CompiledPipeline{.main = create_pipeline(config, layout)}; }, priority);
#endif
}

#ifdef AURORA_ENABLE_RMLUI
static PipelineRef resolve_pipeline(ShaderType type, const rmlui::PipelineConfig& config, const RenderTargetLayout&,
                                    PipelinePriority priority) {
  return find_pipeline_impl(
      xxh3_hash(config, static_cast<HashType>(type)),
      [config] { return CompiledPipeline{.main = rmlui::create_pipeline(config)}; }, priority);
}
#endif

// Set by pipeline_cache_abort when the statement that failed found the file damaged (a write torn by
// a killed process, a truncated or overwritten file): the file is then deleted and, while loading,
// started again from an empty one instead of being left broken for every later run.
static bool g_pipelineCacheDamaged = false;
// The result of the last failed write_pipeline_cache_record (for journal_fallback).
static int g_pipelineCacheWriteError = SQLITE_OK;

static void pipeline_cache_abort() {
  g_pipelineCacheBroken = true;
  if (g_pipelineCacheDb != nullptr && sqlite::is_damaged(sqlite3_errcode(g_pipelineCacheDb))) {
    g_pipelineCacheDamaged = true;
  }
  if (g_pipelineCacheLoadStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheLoadStmt);
    g_pipelineCacheLoadStmt = nullptr;
  }
  if (g_pipelineCacheUpsertStmt != nullptr) {
    sqlite3_finalize(g_pipelineCacheUpsertStmt);
    g_pipelineCacheUpsertStmt = nullptr;
  }
  if (g_pipelineCacheDb != nullptr) {
    sqlite3_close(g_pipelineCacheDb);
    g_pipelineCacheDb = nullptr;
  }
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write);

static std::string pipeline_cache_seed_path() {
  if (g_config.resourcesPath == nullptr || g_config.resourcesPath[0] == '\0') {
    return InitialPipelineCacheName;
  }

  std::string path{g_config.resourcesPath};
  if (path.back() != '/' && path.back() != '\\') {
    path += '/';
  }
  path += InitialPipelineCacheName;
  return path;
}

static sqlite3* open_pipeline_cache_seed_db(const std::string& path) {
  if (!register_sdl_vfs()) {
    Log.warn("Failed to register SDL pipeline cache seed VFS");
    return nullptr;
  }

  sqlite3* seedDb = nullptr;
  const auto ret = sqlite3_open_v2(path.c_str(), &seedDb, SQLITE_OPEN_READONLY | SQLITE_OPEN_PRIVATECACHE, SdlVfsName);
  if (ret != SQLITE_OK) {
    if (seedDb != nullptr) {
      sqlite3_close(seedDb);
    }
    Log.info("No bundled initial pipeline cache found at '{}'", path);
    return nullptr;
  }

  bool schemaMatch = false;
  const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
  const auto schemaRet =
      sqlite::exec(seedDb, schemaQuery.c_str(), [&schemaMatch](int, char**, char**) { schemaMatch = true; });
  if (schemaRet != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache schema from '{}': {}", path, sqlite3_errmsg(seedDb));
    sqlite3_close(seedDb);
    return nullptr;
  }
  if (!schemaMatch) {
    Log.warn("Bundled pipeline cache '{}' does not use schema version {}", path, PipelineCacheSchema);
    sqlite3_close(seedDb);
    return nullptr;
  }

  return seedDb;
}

static int seed_pipeline_cache_once();
static std::string pipeline_cache_db_path();

// Merges the bundled pipeline cache. A write that fails because of the journal file is tried once
// more after sqlite::journal_fallback (the Switch: journal_mode=MEMORY).
static void seed_pipeline_cache() {
  if (g_pipelineCacheBroken || g_pipelineCacheDb == nullptr || g_pipelineCacheUpsertStmt == nullptr) {
    return;
  }
  int ret = seed_pipeline_cache_once();
  if (ret != SQLITE_OK && sqlite::journal_fallback(g_pipelineCacheDb, ret, pipeline_cache_db_path(), Log)) {
    ret = seed_pipeline_cache_once();
  }
  if (ret != SQLITE_OK) {
    if (sqlite::is_damaged(ret)) {
      g_pipelineCacheDamaged = true; // the rollback cleared the connection's error
    }
    pipeline_cache_abort();
  }
}

// Returns SQLITE_OK, also when there is nothing to merge, or the failed write's result.
static int seed_pipeline_cache_once() {

  const auto seedPath = pipeline_cache_seed_path();
  sqlite3* seedDb = open_pipeline_cache_seed_db(seedPath);
  if (seedDb == nullptr) {
    return SQLITE_OK;
  }

  sqlite3_stmt* seedStmt = nullptr;
  auto closeSeed = [&] {
    if (seedStmt != nullptr) {
      sqlite3_finalize(seedStmt);
      seedStmt = nullptr;
    }
    sqlite3_close(seedDb);
  };

  auto ret = sqlite3_prepare_v3(seedDb,
                                "SELECT type, hash, config_version, config_size, config, first_frame_used "
                                "FROM pipeline_cache",
                                -1, 0, &seedStmt, nullptr);
  if (ret != SQLITE_OK) {
    Log.warn("Failed to read bundled pipeline cache rows from '{}': {}", seedPath, sqlite3_errmsg(seedDb));
    closeSeed();
    return SQLITE_OK;
  }

  bool writeFailed = false;
  bool readFailed = false;
  uint32_t mergedRows = 0;
  uint32_t skippedRows = 0;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      Log.error("Failed to begin pipeline cache seed transaction");
      const int beginError = sqlite3_errcode(g_pipelineCacheDb);
      closeSeed();
      return beginError != SQLITE_OK ? beginError : SQLITE_ERROR;
    }

    while ((ret = sqlite3_step(seedStmt)) == SQLITE_ROW) {
      const auto typeValue = sqlite3_column_int(seedStmt, 0);
      const auto hashValue = sqlite3_column_int64(seedStmt, 1);
      const auto configVersionValue = sqlite3_column_int(seedStmt, 2);
      const auto configSizeValue = sqlite3_column_int(seedStmt, 3);
      const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(seedStmt, 4));
      const auto configBlobSize = sqlite3_column_bytes(seedStmt, 4);
      const auto firstFrameUsedValue = sqlite3_column_int64(seedStmt, 5);
      constexpr auto MaxShaderTypeValue = std::numeric_limits<std::underlying_type_t<ShaderType>>::max();
      if (typeValue < 0 || typeValue > MaxShaderTypeValue || configVersionValue < 0 || configSizeValue < 0 ||
          configSizeValue != configBlobSize || (configBlobSize > 0 && configBlob == nullptr) ||
          firstFrameUsedValue < 0 || firstFrameUsedValue > std::numeric_limits<uint32_t>::max()) {
        ++skippedRows;
        continue;
      }

      PipelineCacheWrite write{
          .type = static_cast<ShaderType>(typeValue),
          .hash = static_cast<PipelineRef>(hashValue),
          .configVersion = static_cast<uint32_t>(configVersionValue),
          .config = ByteBuffer(static_cast<size_t>(configBlobSize)),
          .firstFrameUsed = static_cast<uint32_t>(firstFrameUsedValue),
      };
      if (configBlobSize > 0) {
        std::memcpy(write.config.data(), configBlob, static_cast<size_t>(configBlobSize));
      }

      if (!write_pipeline_cache_record(write)) {
        writeFailed = true;
        break;
      }
      ++mergedRows;
    }

    if (!writeFailed && ret != SQLITE_DONE) {
      Log.warn("Failed while reading bundled pipeline cache rows from '{}': {}", seedPath, sqlite3_errmsg(seedDb));
      readFailed = true;
    }

    if (!writeFailed && !readFailed) {
      if (const int commitRet = tx.commit(); commitRet != SQLITE_OK) {
        g_pipelineCacheWriteError = commitRet;
        writeFailed = true;
      }
    }
  }

  if (seedStmt != nullptr) {
    sqlite3_finalize(seedStmt);
    seedStmt = nullptr;
  }
  // Switch: the priority set (switch/native/aurora/patches, aurora_switch_get_precompile). A seed
  // without the table (an older gen_pipeline_cache.sh) has none.
  if (!writeFailed && !readFailed) {
    std::lock_guard lock{g_pipelineMutex};
    g_priorityKeys.clear();
    if (sqlite3_prepare_v3(seedDb, "SELECT hash FROM pipeline_priority WHERE priority = 0", -1, 0, &seedStmt,
                           nullptr) == SQLITE_OK) {
      while (sqlite3_step(seedStmt) == SQLITE_ROW) {
        g_priorityKeys.insert(static_cast<HashType>(sqlite3_column_int64(seedStmt, 0)));
      }
      Log.info("Bundled pipeline cache marks {} pipelines as priority 0", g_priorityKeys.size());
    } else {
      Log.info("Bundled pipeline cache '{}' has no pipeline_priority table", seedPath);
    }
  }
  closeSeed();

  if (writeFailed) {
    return g_pipelineCacheWriteError != SQLITE_OK ? g_pipelineCacheWriteError : SQLITE_ERROR;
  }

  if (!readFailed) {
    Log.info("Seeded pipeline cache from '{}' ({} rows merged, {} rows skipped)", seedPath, mergedRows, skippedRows);
  }
  return SQLITE_OK;
}

static std::string pipeline_cache_db_path() {
  return io::fs_path_to_string(io::fs_path_from_string(g_config.cachePath) / "pipeline_cache.db");
}

// Why the last open failed (the first failing step and its result), for one summary line in
// prepare_pipeline_cache_db instead of a line per step.
static int g_pipelineCacheOpenError = SQLITE_OK;
static std::string g_pipelineCacheOpenMessage;

template <typename... T>
static void open_error(fmt::format_string<T...> fmt, T&&... args) {
  if (g_pipelineCacheOpenError != SQLITE_OK) {
    return;
  }
  g_pipelineCacheOpenError = g_pipelineCacheDb != nullptr ? sqlite3_errcode(g_pipelineCacheDb) : SQLITE_CANTOPEN;
  if (g_pipelineCacheOpenError == SQLITE_OK) {
    g_pipelineCacheOpenError = SQLITE_ERROR;
  }
  g_pipelineCacheOpenMessage = fmt::format(fmt, std::forward<T>(args)...);
}

static bool prepare_pipeline_cache_db_once();

// Opens the cache; one that cannot be opened because the file is damaged is removed and started
// once more from an empty file (then seeded again from the bundled cache).
static bool prepare_pipeline_cache_db() {
  static bool s_retried = false;
  if (g_pipelineCacheBroken) {
    return false;
  }
  if (g_pipelineCacheDb != nullptr) {
    return true;
  }
  g_pipelineCacheOpenError = SQLITE_OK;
  if (prepare_pipeline_cache_db_once()) {
    return true;
  }
  if (g_pipelineCacheOpenError == SQLITE_OK) {
    return false; // failed after opening (seeding), already reported
  }
  const auto path = pipeline_cache_db_path();
  const std::string why = g_pipelineCacheOpenMessage;
  if (s_retried || !sqlite::reset_on_open_failure(g_pipelineCacheOpenError)) {
    Log.error("Pipeline cache {} could not be opened ({}); running without it", path, why);
    return false;
  }
  s_retried = true;
  const long long oldSize = sqlite::reset_cache_file(path);
  g_pipelineCacheBroken = false;
  g_pipelineCacheDamaged = false;
  g_pipelineCacheOpenError = SQLITE_OK;
  if (!prepare_pipeline_cache_db_once()) {
    Log.error("Pipeline cache {} could not be opened ({}); deleted it ({} bytes) but a new one failed too ({}); "
              "running without it",
              path, why, oldSize, g_pipelineCacheOpenMessage);
    return false;
  }
  Log.warn("Pipeline cache {} could not be opened ({}); deleted it ({} bytes) and started an empty one", path, why,
           oldSize);
  return true;
}

static bool prepare_pipeline_cache_db_once() {
  if (g_pipelineCacheBroken) {
    return false;
  }
  if (g_pipelineCacheDb != nullptr) {
    return true;
  }
  const auto fail = [] {
    open_error("failed");
    pipeline_cache_abort();
    return false;
  };

  const auto path = pipeline_cache_db_path();
  const char* openPath = path.c_str(); /* HMMMGC_NX_SQLITE */
#if defined(__SWITCH__)
  if (path.rfind("sdmc:", 0) == 0) openPath += 5;
#endif
  auto ret = sqlite3_open(openPath, &g_pipelineCacheDb);
  if (ret != SQLITE_OK) {
    open_error("Failed to open pipeline cache database: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  ret = sqlite::apply_cache_pragmas(g_pipelineCacheDb, path, Log);
  if (ret != SQLITE_OK) {
    open_error("Failed to set pipeline cache pragmas: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  bool schemaFailed = false;
  {
    sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
    if (!tx) {
      open_error("Failed to begin pipeline cache schema transaction");
      schemaFailed = true;
    } else {
      ret = sqlite::exec(g_pipelineCacheDb, "CREATE TABLE IF NOT EXISTS aurora_schema(value INTEGER);");
      if (ret != SQLITE_OK) {
        open_error("Failed to create pipeline cache schema table: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    bool schemaMatch = false;
    if (!schemaFailed) {
      const auto schemaQuery = fmt::format("SELECT 1 FROM aurora_schema WHERE value = {}", PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaQuery.c_str(),
                         [&schemaMatch](int, char**, char**) { schemaMatch = true; });
      if (ret != SQLITE_OK) {
        open_error("Failed to read pipeline cache schema version: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed && !schemaMatch) {
      const auto schemaSql = fmt::format(
          R"(DROP TABLE IF EXISTS pipeline_cache;
CREATE TABLE pipeline_cache (
  type INTEGER NOT NULL,
  hash INTEGER NOT NULL,
  config_version INTEGER NOT NULL,
  config_size INTEGER NOT NULL,
  config BLOB NOT NULL,
  first_frame_used INTEGER NOT NULL,
  PRIMARY KEY (type, hash)
);
CREATE INDEX pipeline_cache_load_order_idx
  ON pipeline_cache(type, config_version, first_frame_used);
DELETE FROM aurora_schema;
INSERT INTO aurora_schema VALUES ({});)",
          PipelineCacheSchema);
      ret = sqlite::exec(g_pipelineCacheDb, schemaSql.c_str());
      if (ret != SQLITE_OK) {
        open_error("Failed to initialize pipeline cache schema: {}", sqlite3_errmsg(g_pipelineCacheDb));
        schemaFailed = true;
      }
    }

    if (!schemaFailed) {
      tx.commit();
    }
  }

  if (schemaFailed) {
    return fail();
  }

  ret = sqlite3_prepare_v3(g_pipelineCacheDb,
                           "SELECT config, first_frame_used FROM pipeline_cache "
                           "WHERE type = ? AND config_version = ? "
                           "ORDER BY first_frame_used ASC, rowid ASC",
                           -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheLoadStmt, nullptr);
  if (ret != SQLITE_OK) {
    open_error("Failed to prepare pipeline cache load statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  ret = sqlite3_prepare_v3(
      g_pipelineCacheDb,
      "INSERT INTO pipeline_cache (type, hash, config_version, config_size, config, first_frame_used) "
      "VALUES (?, ?, ?, ?, ?, ?) "
      "ON CONFLICT(type, hash) DO UPDATE SET "
      "config_version = excluded.config_version, "
      "config_size = excluded.config_size, "
      "config = excluded.config, "
      "first_frame_used = MIN(pipeline_cache.first_frame_used, excluded.first_frame_used)",
      -1, SQLITE_PREPARE_PERSISTENT, &g_pipelineCacheUpsertStmt, nullptr);
  if (ret != SQLITE_OK) {
    open_error("Failed to prepare pipeline cache upsert statement: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  seed_pipeline_cache();
  if (g_pipelineCacheBroken) {
    return false;
  }

  return true;
}

static void prune_old_pipeline_cache_versions() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  const auto clearDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                       underlying(ShaderType::Clear), clear::ClearPipelineConfigVersion);
  auto ret = sqlite::exec(g_pipelineCacheDb, clearDelete.c_str());
  if (ret != SQLITE_OK) {
    if (!sqlite::is_damaged(ret)) { // a damaged file gets one line where it is deleted
      Log.error("Failed to prune clear pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    }
    pipeline_cache_abort();
    return;
  }

  const auto gxDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                    underlying(ShaderType::GX), gx::GXPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, gxDelete.c_str());
  if (ret != SQLITE_OK) {
    if (!sqlite::is_damaged(ret)) { // a damaged file gets one line where it is deleted
      Log.error("Failed to prune GX pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    }
    pipeline_cache_abort();
    return;
  }

#ifdef AURORA_ENABLE_RMLUI
  const auto rmlDelete = fmt::format("DELETE FROM pipeline_cache WHERE type = {} AND config_version < {}",
                                     underlying(ShaderType::Rml), rmlui::RmlPipelineConfigVersion);
  ret = sqlite::exec(g_pipelineCacheDb, rmlDelete.c_str());
  if (ret != SQLITE_OK) {
    if (!sqlite::is_damaged(ret)) { // a damaged file gets one line where it is deleted
      Log.error("Failed to prune RmlUi pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    }
    pipeline_cache_abort();
  }
#endif
}

static bool write_pipeline_cache_record(const PipelineCacheWrite& write) {
  const auto fail = [&]() {
    g_pipelineCacheWriteError = sqlite3_errcode(g_pipelineCacheDb);
    sqlite3_reset(g_pipelineCacheUpsertStmt);
    sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
    return false;
  };

  auto ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 1, underlying(write.type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 2, static_cast<sqlite3_int64>(write.hash));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache hash: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 3, static_cast<int>(write.configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int(g_pipelineCacheUpsertStmt, 4, static_cast<int>(write.config.size()));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config size: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_blob64(g_pipelineCacheUpsertStmt, 5, write.config.data(), write.config.size(), SQLITE_TRANSIENT);
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache config blob: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }
  ret = sqlite3_bind_int64(g_pipelineCacheUpsertStmt, 6, static_cast<sqlite3_int64>(write.firstFrameUsed));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache first-frame-used: {}", sqlite3_errmsg(g_pipelineCacheDb));
    return fail();
  }

  ret = sqlite3_step(g_pipelineCacheUpsertStmt);
  if (ret != SQLITE_DONE) {
    if (!sqlite::is_damaged(ret)) { // a damaged file gets one line where it is deleted
      Log.error("Failed to upsert pipeline cache row: {}", sqlite3_errmsg(g_pipelineCacheDb));
    }
    return fail();
  }

  sqlite3_reset(g_pipelineCacheUpsertStmt);
  sqlite3_clear_bindings(g_pipelineCacheUpsertStmt);
  return true;
}

static void pipeline_cache_writer() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline cache writer thread");
#endif

  while (true) {
    std::deque<PipelineCacheWrite> batch;
    {
      std::unique_lock lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterCv.wait(lock,
                                   [] { return g_pipelineCacheWriterStop || !g_pipelineCacheWriteQueue.empty(); });
      if (g_pipelineCacheWriterStop && g_pipelineCacheWriteQueue.empty()) {
        return;
      }
      batch.swap(g_pipelineCacheWriteQueue);
    }

    // SQLITE_OK or the failed write's result.
    const auto writeBatch = [&batch]() -> int {
      sqlite::Transaction tx(g_pipelineCacheDb, Log, true);
      if (!tx) {
        Log.error("Failed to begin pipeline cache write transaction");
        const int ret = sqlite3_errcode(g_pipelineCacheDb);
        return ret != SQLITE_OK ? ret : SQLITE_ERROR;
      }
      for (const auto& write : batch) {
        if (!write_pipeline_cache_record(write)) {
          return g_pipelineCacheWriteError != SQLITE_OK ? g_pipelineCacheWriteError : SQLITE_ERROR;
        }
      }
      return tx.commit();
    };
    int writeRet = writeBatch();
    if (writeRet != SQLITE_OK &&
        sqlite::journal_fallback(g_pipelineCacheDb, writeRet, pipeline_cache_db_path(), Log)) {
      writeRet = writeBatch();
    }
    const bool writeFailed = writeRet != SQLITE_OK;
    if (sqlite::is_damaged(writeRet)) {
      g_pipelineCacheDamaged = true; // the rollback cleared the connection's error
    }

    if (writeFailed) {
      pipeline_cache_abort();
      if (g_pipelineCacheDamaged) {
        // Not reopened here (the other threads only check the pointer): the next run starts an
        // empty cache, seeded from the bundled one.
        const auto path = pipeline_cache_db_path();
        const long long oldSize = sqlite::reset_cache_file(path);
        Log.warn("Pipeline cache {} is damaged; deleted it ({} bytes), no pipeline cache writes this run", path,
                 oldSize);
      }
      return;
    }
  }
}

// Switch: the warm-up throttle (aurora_switch_set_warmup_throttle). Duty 0 = off. Under
// g_pipelineMutex but the present count, which the render worker bumps.
static float g_warmupDuty = 0.f;
static uint64_t g_warmupNextNs = 0;      // no throttled build before this
static uint32_t g_warmupThrottled = 0;   // builds held back
static uint64_t g_warmupThrottleWaitNs = 0;
static uint64_t g_warmupSlowNs = 25'000'000; // builds up to this long are not followed by idle time
static bool g_warmupHold = true;             // the last warm-up build was slow: hold the next one
static uint32_t g_warmupUnthrottled = 0;     // warm-up builds followed at once (fast)
static std::atomic<uint64_t> g_presents{0};

void switch_warmup_note_present() { g_presents.fetch_add(1, std::memory_order_relaxed); }

// With the lock held and the next build a warm-up one: waits until the throttle lets it start, a
// draw queues a pipeline, the warm-up empties or the thread ends.
static void warmup_throttle_wait(std::unique_lock<std::mutex>& lock) {
  const auto throttling = [] {
    return g_warmupDuty > 0.f && g_warmupHold && g_pipelineQueue.empty() && !g_backgroundPipelineQueue.empty() &&
           !g_pipelineThreadEnd;
  };
  if (!throttling()) {
    return;
  }
  const uint64_t start = switch_stats::now_ns();
  bool waited = false;
  uint64_t presentsAtReady = 0;
  uint64_t readyNs = 0; // when the idle time was over
  while (throttling()) {
    const uint64_t now = switch_stats::now_ns();
    if (now >= g_warmupNextNs) {
      if (readyNs == 0) {
        readyNs = now;
        presentsAtReady = g_presents.load(std::memory_order_relaxed);
      }
      // Right after a present, so the build overlaps the game thread's next frame; 100 ms without
      // one (a loading pause) is enough waiting.
      if (g_presents.load(std::memory_order_relaxed) != presentsAtReady || now - readyNs >= 100'000'000) {
        break;
      }
    }
    waited = true;
    g_pipelineQueueCv.wait_for(lock, std::chrono::milliseconds(1));
  }
  if (waited) {
    ++g_warmupThrottled;
    g_warmupThrottleWaitNs += switch_stats::now_ns() - start;
  }
}

// Switch: the harness names this thread (its CPU time, and a core away from the render worker's).
extern "C" __attribute__((weak)) void aurora_switch_pipeline_thread_started(void);

static void pipeline_worker() {
#ifdef TRACY_ENABLE
  tracy::SetThreadName("Pipeline compilation thread");
#endif
  if (aurora_switch_pipeline_thread_started != nullptr) {
    aurora_switch_pipeline_thread_started();
  }

  bool hasMore = false;
  while (g_hasPipelineThread || g_pipelinesPerFrame < BuildPipelinesPerFrame) {
    PendingPipeline pending;
    bool throttledBuild = false;
    {
      std::unique_lock lock{g_pipelineMutex};
      if (g_hasPipelineThread) {
        if (!hasMore) {
          g_pipelineQueueCv.wait(lock, [] {
            return !g_pipelineQueue.empty() || !g_backgroundPipelineQueue.empty() || g_pipelineThreadEnd;
          });
        }
      } else if (g_pipelineQueue.empty() && g_backgroundPipelineQueue.empty()) {
        return;
      }
      warmup_throttle_wait(lock);
      if (g_pipelineThreadEnd) {
        break;
      }
      if (g_pipelineQueue.empty() && g_backgroundPipelineQueue.empty()) {
        hasMore = false; // the warm-up was dropped while the throttle waited
        continue;
      }
      throttledBuild = g_pipelineQueue.empty() && g_warmupDuty > 0.f;
      auto& source = !g_pipelineQueue.empty() ? g_pipelineQueue : g_backgroundPipelineQueue;
      pending = std::move(source.front());
      source.pop_front();
    }
    const BuildSplit splitBefore = thread_build_ns();
    const uint64_t compileStart = switch_stats::now_ns();
    auto result = pending.create();
    const uint64_t compileNs = switch_stats::now_ns() - compileStart;
    const BuildSplit split = build_split_since(splitBefore);
    if (throttledBuild) {
      std::lock_guard lock{g_pipelineMutex};
      // A build is slow for its own work (a shader cache miss), not for the time it waited for the
      // GL context while the render worker had it.
      const uint64_t ownNs = compileNs - std::min(split.waitNs, compileNs);
      g_warmupHold = g_warmupSlowNs == 0 || ownNs > g_warmupSlowNs;
      if (!g_warmupHold) {
        ++g_warmupUnthrottled;
      } else if (g_warmupDuty > 0.f && g_warmupDuty < 1.f) {
        g_warmupNextNs = switch_stats::now_ns() +
                         static_cast<uint64_t>(static_cast<double>(compileNs) * (1.0 - g_warmupDuty) / g_warmupDuty);
      }
    }
    switch_stats::add_pipeline_compile(compileNs);
    const auto pipelineCount = result.pipeline_count();
    {
      std::lock_guard lock{g_pipelineMutex};
      warmup_built(pending.hash, compileNs, split);
      g_pipelines.try_emplace(pending.hash, std::move(result));
      g_pendingPipelines.erase(pending.hash);
      hasMore = !g_pipelineQueue.empty() || !g_backgroundPipelineQueue.empty();
    }
    if (!g_hasPipelineThread) {
      g_pipelinesPerFrame += std::max(1u, pipelineCount);
    }
    notify_pipeline_ready(true, pipelineCount);
  }
}

template <typename PipelineConfig>
static size_t load_pipeline_cache_entries(ShaderType type, uint32_t configVersion) {
  if (!prepare_pipeline_cache_db()) {
    return 0;
  }

  auto ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 1, underlying(type));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load type: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return 0;
  }
  ret = sqlite3_bind_int(g_pipelineCacheLoadStmt, 2, static_cast<int>(configVersion));
  if (ret != SQLITE_OK) {
    Log.error("Failed to bind pipeline cache load config version: {}", sqlite3_errmsg(g_pipelineCacheDb));
    pipeline_cache_abort();
    return 0;
  }

  size_t acceptedRows = 0;
  while ((ret = sqlite3_step(g_pipelineCacheLoadStmt)) == SQLITE_ROW) {
    const auto* configBlob = static_cast<const uint8_t*>(sqlite3_column_blob(g_pipelineCacheLoadStmt, 0));
    const auto configSize = sqlite3_column_bytes(g_pipelineCacheLoadStmt, 0);
    const auto firstFrameUsed = static_cast<uint32_t>(sqlite3_column_int64(g_pipelineCacheLoadStmt, 1));
    if (configSize != static_cast<int>(sizeof(PipelineConfig)) || (configSize != 0 && configBlob == nullptr)) {
      continue;
    }

    PipelineConfig config;
    std::memcpy(&config, configBlob, sizeof(config));
    if (config.version != configVersion) {
      continue;
    }
    if constexpr (std::is_same_v<PipelineConfig, gx::PipelineConfig>) {
      // rows written before stages were resolved (a frozen Mothula's crashed every warm-up)
      gx::resolve_ungenerated_texcoords(config.shaderConfig);
    }

    remember_pipeline_config(type, config, firstFrameUsed, false);
    ++acceptedRows;
  }

  if (ret != SQLITE_DONE) {
    if (!sqlite::is_damaged(ret)) { // a damaged file gets one line where it is deleted
      Log.error("Failed to read pipeline cache rows: {}", sqlite3_errmsg(g_pipelineCacheDb));
    }
    pipeline_cache_abort(); // finalizes the load statement: sqlite3_clear_bindings(nullptr) would crash
    return acceptedRows;
  }

  sqlite3_reset(g_pipelineCacheLoadStmt);
  sqlite3_clear_bindings(g_pipelineCacheLoadStmt);
  return acceptedRows;
}

static size_t load_pipeline_cache_once();

// Loads the known pipelines. A file found damaged while reading is deleted and the cache started
// again from an empty file (seeded from the bundled cache), once.
static size_t load_pipeline_cache() {
  g_pipelineCacheDamaged = false;
  size_t acceptedRows = load_pipeline_cache_once();
  if (g_pipelineCacheBroken && g_pipelineCacheDamaged) {
    const auto path = pipeline_cache_db_path();
    const long long oldSize = sqlite::reset_cache_file(path);
    Log.warn("Pipeline cache {} is damaged; deleted it ({} bytes) and started an empty one", path, oldSize);
    g_pipelineCacheBroken = false;
    g_pipelineCacheDamaged = false;
    acceptedRows += load_pipeline_cache_once();
  }
  return acceptedRows;
}

static size_t load_pipeline_cache_once() {
  if (!prepare_pipeline_cache_db()) {
    return 0;
  }
  prune_old_pipeline_cache_versions();
  if (g_pipelineCacheBroken) {
    return 0;
  }

  size_t acceptedRows = 0;
#ifdef AURORA_ENABLE_RMLUI
  acceptedRows += load_pipeline_cache_entries<rmlui::PipelineConfig>(ShaderType::Rml, rmlui::RmlPipelineConfigVersion);
#endif
  acceptedRows +=
      load_pipeline_cache_entries<clear::PipelineConfig>(ShaderType::Clear, clear::ClearPipelineConfigVersion);
  acceptedRows += load_pipeline_cache_entries<gx::PipelineConfig>(ShaderType::GX, gx::GXPipelineConfigVersion);
  return acceptedRows;
}

static void start_pipeline_cache_writer() {
  if (!prepare_pipeline_cache_db()) {
    return;
  }

  g_pipelineCacheWriterStop = false;
  g_pipelineCacheWriterThread = std::thread(pipeline_cache_writer);
}

static void stop_pipeline_cache_writer() {
  if (g_pipelineCacheWriterThread.joinable()) {
    {
      std::lock_guard lock{g_pipelineCacheWriterMutex};
      g_pipelineCacheWriterStop = true;
    }
    g_pipelineCacheWriterCv.notify_one();
    g_pipelineCacheWriterThread.join();
  } else {
    g_pipelineCacheWriterStop = false;
  }

  g_pipelineCacheWriteQueue.clear();
}

// AuroraConfig::blockingPipelines: wait for a draw's pipeline instead of skipping the draw.
static PipelinePriority draw_pipeline_priority() {
  return g_config.blockingPipelines ? PipelinePriority::Blocking : PipelinePriority::Normal;
}

PipelineRef find_pipeline(const clear::PipelineConfig& config, const RenderTargetLayout& layout) {
  remember_pipeline_config(ShaderType::Clear, config, current_frame(), true);
  return resolve_pipeline(ShaderType::Clear, config, layout, draw_pipeline_priority());
}

PipelineRef find_pipeline(const gx::PipelineConfig& config, const RenderTargetLayout& layout) {
  remember_pipeline_config(ShaderType::GX, config, current_frame(), true, true);
  return resolve_pipeline(ShaderType::GX, config, layout, draw_pipeline_priority());
}

#ifdef AURORA_ENABLE_RMLUI
PipelineRef find_pipeline(const rmlui::PipelineConfig& config) {
  remember_pipeline_config(ShaderType::Rml, config, 0, true);
  return resolve_pipeline(ShaderType::Rml, config, {}, PipelinePriority::Blocking);
}
#endif

void rebuild_pipeline_cache() {
  ZoneScoped;
  const auto scene = scene_render_target_layout();
  auto offscreen = scene;
  offscreen.colorAttachmentCount = 1;
  offscreen.sampleCount = 1;
  detail::finalize_render_target_layout(offscreen);
  g_pipelineLayoutKey = scene.key;

  // Switch: the bundled cache's priority set first (g_priorityKeys), then the rest, each in order of
  // first use.
  struct WarmupPipeline {
    bool priority;
    KnownPipeline pipeline;
  };
  std::vector<WarmupPipeline> known;
  {
    std::lock_guard lock{g_pipelineMutex};
    if (g_warmupStopped) {
      // Switch: the warm-up was stopped (aurora_switch_stop_precompile); pipelines are built when
      // first drawn, also for a new render target layout.
      return;
    }
    known.reserve(g_knownPipelines.size());
    for (const auto& [key, pipeline] : g_knownPipelines) {
      known.push_back({g_priorityKeys.contains(key), pipeline});
    }
  }
  std::ranges::sort(known, [](const WarmupPipeline& a, const WarmupPipeline& b) {
    if (a.priority != b.priority) {
      return a.priority;
    }
    return a.pipeline.firstFrameUsed < b.pipeline.firstFrameUsed;
  });
  for (const auto& [priority, pipeline] : known) {
    std::visit(
        [&](const auto& config) {
          const PipelineRef ref = resolve_pipeline(pipeline.type, config, scene, PipelinePriority::Background);
          if (priority) {
            warmup_priority_queued(ref);
          }
          if (offscreen.key != scene.key) {
            const PipelineRef offscreenRef =
                resolve_pipeline(pipeline.type, config, offscreen, PipelinePriority::Background);
            if (priority) {
              warmup_priority_queued(offscreenRef);
            }
          }
        },
        pipeline.config);
  }
}

void initialize_pipeline_cache() {
  g_pipelineCacheBroken = false;
  g_pipelineCacheWriterStop = false;
  g_pipelineThreadEnd = false;
  g_gpuCachePrunePending = false;

  if (webgpu::g_backendType == wgpu::BackendType::WebGPU) {
    g_hasPipelineThread = false;
  } else {
    g_hasPipelineThread = true;
    g_pipelineThread = std::thread(pipeline_worker);
  }

  const size_t loadedCount = load_pipeline_cache();
  rebuild_pipeline_cache();
  if (!g_pipelineCacheBroken && loadedCount > 0) {
    g_gpuCachePrunePending = true;
  }

  if (!g_pipelineCacheBroken) {
    start_pipeline_cache_writer();
  }
}

// The pipelines still waiting for their build (g_unsavedPipelines), queued for the cache file as a
// draw's request queued them before patch 0023: for a clean exit, where none of them crashed.
static void enqueue_unbuilt_pipeline_writes() {
  std::scoped_lock lock{g_pipelineMutex, g_unsavedPipelinesMutex};
  for (const auto& [cacheKey, firstFrameUsed] : g_unsavedPipelines) {
    const auto known = g_knownPipelines.find(cacheKey);
    if (known != g_knownPipelines.end()) {
      std::visit(
          [&](const auto& config) {
            enqueue_pipeline_cache_write(make_pipeline_cache_write(known->second.type, cacheKey, config, firstFrameUsed));
          },
          known->second.config);
    }
  }
  g_unsavedPipelines.clear();
}

void save_unbuilt_pipelines() {
  enqueue_unbuilt_pipeline_writes();
  stop_pipeline_cache_writer(); // drains the queue; later writes wait for a writer that never comes
}

// C linkage: the same function as include/aurora/pipeline_cache_save.h's global declaration
extern "C" void aurora_save_unbuilt_pipelines(void) { save_unbuilt_pipelines(); }

void shutdown_pipeline_cache() {
  if (g_hasPipelineThread) {
    g_pipelineThreadEnd = true;
    g_pipelineQueueCv.notify_all();
    g_pipelineReadyCv.notify_all();
    g_pipelineThread.join();
  }
  g_hasPipelineThread = false;

  // A clean shutdown: the pipelines still waiting for their build crashed nothing
  enqueue_unbuilt_pipeline_writes();
  stop_pipeline_cache_writer();
  pipeline_cache_abort();
  g_pipelineCacheBroken = false;
  g_pipelinesPerFrame = 0;
  g_gpuCachePrunePending = false;
  g_pipelines.clear();
  g_knownPipelines.clear();
  g_pipelineLayoutKey.reset();
  g_pipelineQueue.clear();
  g_backgroundPipelineQueue.clear();
  g_pendingPipelines.clear();
  g_warmupPending.clear();
  g_warmupTotal = g_warmupDone = g_warmupDropped = 0;
  g_warmupNs = 0;
  g_warmupWaitNs = 0;
  g_slowBuildSeq = 0;
  g_warmupStopped = false;
  g_priorityKeys.clear();
  g_priorityQueued.clear();
  g_priorityPending.clear();
  g_priorityDone = 0;

  queuedPipelines = 0;
  createdPipelines = 0;
}

void begin_pipeline_frame() {
  if (!g_hasPipelineThread) {
    g_pipelinesPerFrame = 0;
  }
  if (g_pipelineLayoutKey != scene_render_target_layout().key) {
    rebuild_pipeline_cache();
  }
}

void end_pipeline_frame() {
  if (!g_hasPipelineThread) {
    pipeline_worker();
  }
}

bool get_pipeline(PipelineRef ref, CompiledPipeline& pipeline) {
  std::lock_guard guard{g_pipelineMutex};
  const auto it = g_pipelines.find(ref);
#if defined(AURORA_GFX_DEKO3D)
  if (it == g_pipelines.end() || it->second.dkMain == nullptr) {
#else
  if (it == g_pipelines.end() || !it->second.main) {
#endif
    return false;
  }
  pipeline = it->second;
  return true;
}

} // namespace aurora::gfx

// Switch: progress of the boot warm-up, and a way to end it (switch/native/aurora/patches).
void aurora_switch_get_precompile(AuroraSwitchPrecompile* out) {
  using namespace aurora::gfx;
  if (out == nullptr) {
    return;
  }
  std::lock_guard lock{g_pipelineMutex};
  out->total = g_warmupTotal;
  out->done = g_warmupDone;
  out->pending = static_cast<uint32_t>(g_warmupPending.size());
  out->dropped = g_warmupDropped;
  out->compileNs = g_warmupNs;
  out->stopped = g_warmupStopped;
  out->priorityTotal = static_cast<uint32_t>(g_priorityQueued.size());
  out->priorityDone = g_priorityDone;
  out->throttleDuty = g_warmupDuty;
  out->throttled = g_warmupThrottled;
  out->throttleWaitNs = g_warmupThrottleWaitNs;
  out->unthrottled = g_warmupUnthrottled;
  out->contextWaitNs = g_warmupWaitNs;
}

uint32_t aurora_switch_get_slow_builds(AuroraSwitchSlowBuild* out, uint32_t max, uint64_t* sinceSeq) {
  using namespace aurora::gfx;
  if (out == nullptr || sinceSeq == nullptr) {
    return 0;
  }
  std::lock_guard lock{g_pipelineMutex};
  uint64_t first = *sinceSeq + 1;
  if (g_slowBuildSeq > kSlowBuildRing && first <= g_slowBuildSeq - kSlowBuildRing) {
    first = g_slowBuildSeq - kSlowBuildRing + 1;
  }
  uint32_t n = 0;
  for (uint64_t seq = first; seq <= g_slowBuildSeq && n < max; ++seq) {
    out[n++] = g_slowBuilds[(seq - 1) % kSlowBuildRing];
    *sinceSeq = seq;
  }
  return n;
}

void aurora_switch_set_warmup_slow_ns(uint64_t ns) {
  using namespace aurora::gfx;
  {
    std::lock_guard lock{g_pipelineMutex};
    g_warmupSlowNs = ns;
    g_slowBuildNs = ns;
  }
  g_pipelineQueueCv.notify_all();
}

void aurora_switch_set_warmup_throttle(float duty) {
  using namespace aurora::gfx;
  {
    std::lock_guard lock{g_pipelineMutex};
    g_warmupDuty = duty > 0.f && duty < 1.f ? duty : 0.f;
  }
  g_pipelineQueueCv.notify_all();
}

uint32_t aurora_switch_stop_precompile(void) {
  using namespace aurora::gfx;
  uint32_t dropped = 0;
  {
    std::lock_guard lock{g_pipelineMutex};
    g_warmupStopped = true;
    // Entries a draw already asked for were promoted to g_pipelineQueue and stay; the rest of the
    // warm-up queue goes. A pipeline dropped here is queued again when a draw first needs it.
    for (const auto& pending : g_backgroundPipelineQueue) {
      g_pendingPipelines.erase(pending.hash);
      g_warmupPending.erase(pending.hash);
      g_priorityPending.erase(pending.hash);
      ++dropped;
    }
    g_backgroundPipelineQueue.clear();
    g_warmupDropped += dropped;
  }
  for (uint32_t i = 0; i < dropped; ++i) {
    --queuedPipelines;
  }
  // The Dawn cache prune after the warm-up would delete the entries of every pipeline not built
  // this session, i.e. the dropped ones; skip it.
  g_gpuCachePrunePending = false;
  return dropped;
}
