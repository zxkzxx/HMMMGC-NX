#ifndef AURORA_AURORA_H
#define AURORA_AURORA_H

#ifdef __cplusplus
#include <cstddef>
#include <cstdint>

extern "C" {
#else
#include "stdbool.h"
#include "stddef.h"
#include "stdint.h"
#endif

typedef enum {
  SAMPLER_BILINEAR,
  SAMPLER_AREA,
} AuroraSampler;

typedef enum {
  BACKEND_AUTO,
  BACKEND_D3D11,
  BACKEND_D3D12,
  BACKEND_METAL,
  BACKEND_VULKAN,
  BACKEND_OPENGL,
  BACKEND_OPENGLES,
  BACKEND_WEBGPU,
  BACKEND_NULL,
} AuroraBackend;

typedef enum {
  LOG_DEBUG,
  LOG_INFO,
  LOG_WARNING,
  LOG_ERROR,
  LOG_FATAL,
} AuroraLogLevel;

typedef struct {
  int32_t x;
  int32_t y;
} AuroraWindowPos;

typedef struct {
  uint32_t width;
  uint32_t height;

  /**
   * Width of the main GX framebuffer.
   */
  uint32_t fb_width;

  /**
   * Height of the main GX framebuffer.
   */
  uint32_t fb_height;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_width if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_width;

  /**
   * The size of the framebuffer used to present to the operating system.
   * May differ from fb_height if Aurora is instructed to force an aspect ratio or resolution configuration.
   */
  uint32_t native_fb_height;
  float scale;
} AuroraWindowSize;

typedef struct SDL_Window SDL_Window;
typedef struct AuroraEvent AuroraEvent;

typedef void (*AuroraLogCallback)(AuroraLogLevel level, const char* module, const char* message, unsigned int len);
typedef void (*AuroraImGuiInitCallback)(const AuroraWindowSize* size);

#define MEM1_DEFAULT_SIZE (24 * 1024 * 1024)
#define ARAM_DEFAULT_SIZE (16 * 1024 * 1024)

typedef struct {
  const char* appName;
  const char* userPath;
  const char* cachePath;
  const char* resourcesPath;
  AuroraBackend desiredBackend;
  uint32_t msaa;
  uint16_t maxTextureAnisotropy;
  bool vsync;
  bool startFullscreen;
  bool allowJoystickBackgroundEvents;
  bool pauseOnFocusLost;
  bool allowTextureDumps;
  bool allowCpuAdapter;
  int32_t windowPosX;
  int32_t windowPosY;
  uint32_t windowWidth;
  uint32_t windowHeight;
  void* iconRGBA8;
  uint32_t iconWidth;
  uint32_t iconHeight;
  AuroraLogCallback logCallback;
  AuroraLogLevel logLevel;
  AuroraImGuiInitCallback imGuiInitCallback;

  /*
   * The size of the GameCube's main memory, or MEM1 on the Wii.
   * Note that it will not be allocated at the exact 0x80000000 address, as that cannot be guaranteed.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem1Size;

  /*
   * The size of the GameCube's ARAM, or MEM2 on the Wii.
   * This can be set to 0 to disable allocating this region.
   */
  uint32_t mem2Size;

  /*
   * Compile GX and clear pipelines synchronously: a draw whose pipeline is not built yet waits for
   * it instead of being skipped for that frame (the default asynchronous mode). Every frame then
   * shows every draw, as on the console, at the cost of a stall the first time a pipeline is used.
   */
  bool blockingPipelines;
} AuroraConfig;

typedef struct {
  AuroraBackend backend;
  const char* userPath;
  const char* cachePath;
  SDL_Window* window;
  AuroraWindowSize windowSize;
} AuroraInfo;

AuroraInfo aurora_initialize(int argc, char* argv[], const AuroraConfig* config);
void aurora_shutdown();
const AuroraEvent* aurora_update();
bool aurora_begin_frame();
void aurora_end_frame();
/**
 * Asynchronous end of frame (SwitchWaker patch 0018), off by default. aurora_end_frame then returns without
 * waiting for the GX FIFO worker to process the frame: the worker ends it (texture caches, the last render
 * pass, the hand-off to the render worker) when it gets there, and begins the next. aurora_end_frame waits
 * only while two ended frames are not yet done. Set between frames.
 */
void aurora_set_async_end_frame(bool enabled);
bool aurora_get_async_end_frame();
/**
 * Waits for the frames the async end of frame has not yet ended on the GX FIFO worker. After it Aurora is as
 * a synchronous aurora_end_frame leaves it: copy textures made, the frame queued to the render worker (so
 * work queued there afterwards, such as a readback, runs behind it). Returns at once otherwise.
 */
void aurora_frame_sync();

/**
 * GX FIFO translation profile (SwitchWaker patch 0020): a sampling profile of the thread translating the GX
 * stream (the FIFO worker in Thread mode). The values below AURORA_FIFO_PROFILE_CATEGORIES are running
 * totals of sampled wall time (ns) the translating thread spent per category (innermost wins); the rest
 * are counts and byte totals. Off by default (nothing stored, no sampler thread); aurora_fifo_profile_get
 * fills up to `count` values indexed by AuroraFifoProfileValue.
 */
typedef enum {
  AURORA_FIFO_PROFILE_IDLE,      /* the worker waiting for commands or jobs */
  AURORA_FIFO_PROFILE_PROCESS,   /* fifo::process outside the categories below (command parsing) */
  AURORA_FIFO_PROFILE_JOB,       /* frame jobs (async begin/end of frame: textures, last pass, hand-off) */
  AURORA_FIFO_PROFILE_BP,        /* BP register loads */
  AURORA_FIFO_PROFILE_XF,        /* XF register loads, immediate and indexed (matrices, lights) */
  AURORA_FIFO_PROFILE_CP,        /* CP register loads */
  AURORA_FIFO_PROFILE_AURORA,    /* Aurora subcommands except the draws (texture objects, arrays, ...) */
  AURORA_FIFO_PROFILE_DRAW,      /* draws outside the parts below: vertices, indices, merging */
  AURORA_FIFO_PROFILE_PIPELINE,  /* draw: pipeline config and shader info */
  AURORA_FIFO_PROFILE_PIPELINE_LOOKUP, /* draw: pipeline_ref (the pipeline cache's hashes and locks) */
  AURORA_FIFO_PROFILE_TEXTURES,  /* draw: sampled textures resolved */
  AURORA_FIFO_PROFILE_BIND,      /* draw: bind group (samplers, the bind group cache) */
  AURORA_FIFO_PROFILE_UNIFORM,   /* draw: uniform build */
  AURORA_FIFO_PROFILE_ARRAYS,    /* draw: indexed vertex arrays copied to storage */
  AURORA_FIFO_PROFILE_COMMAND,   /* draw: the draw command recorded */
  AURORA_FIFO_PROFILE_CATEGORIES,
  AURORA_FIFO_PROFILE_SAMPLES = AURORA_FIFO_PROFILE_CATEGORIES,
  AURORA_FIFO_PROFILE_DRAWS,     /* draws recorded (not merged) */
  AURORA_FIFO_PROFILE_MERGED,    /* draws merged into the previous one */
  AURORA_FIFO_PROFILE_PIPELINES, /* pipeline lookups */
  AURORA_FIFO_PROFILE_BINDS,     /* bind group builds */
  AURORA_FIFO_PROFILE_UNIFORMS,  /* uniform builds */
  AURORA_FIFO_PROFILE_UNIFORM_BYTES,
  AURORA_FIFO_PROFILE_VERTEX_BYTES,
  AURORA_FIFO_PROFILE_ARRAY_BYTES,
  AURORA_FIFO_PROFILE_STREAM_BYTES, /* GX stream bytes translated */
  AURORA_FIFO_PROFILE_BP_LOADS,
  AURORA_FIFO_PROFILE_XF_LOADS,
  AURORA_FIFO_PROFILE_PIPELINE_MEMO_HITS, /* pipeline lookups answered by patch 0021's memo */
  AURORA_FIFO_PROFILE_BIND_MEMO_HITS,     /* bind group builds answered by patch 0021's memo */
  AURORA_FIFO_PROFILE_COUNT
} AuroraFifoProfileValue;
void aurora_fifo_profile_enable(bool enabled);
void aurora_fifo_profile_get(uint64_t* out, size_t count);
/**
 * GX FIFO translation fast paths (SwitchWaker patch 0021), on by default: a per-frame memo of pipeline
 * lookups (repeated draw states skip the shader info, the cache hashes and locks) and indices of merged
 * primitives written straight into the frame's index buffer. Off = Aurora's original paths (A/B checks).
 * Set between frames.
 */
void aurora_set_fifo_fast_paths(bool enabled);

void aurora_set_log_level(AuroraLogLevel level);
void aurora_set_pause_on_focus_lost(bool value);
void aurora_set_background_input(bool value);
void aurora_set_resampler(AuroraSampler sampler);
/** Sets the clock timescale. Default 1.0f. 0.0f is paused. Range 0.0f-16.0f. */
void aurora_set_timescale(float scale);

AuroraBackend aurora_get_backend();
const AuroraBackend* aurora_get_available_backends(size_t* count);
float aurora_get_timescale();

#ifdef __cplusplus
}
#endif

#endif
