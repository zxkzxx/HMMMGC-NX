#pragma once
#include_next <aurora/gfx.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct {
  uint64_t totalPresentCount;
  uint32_t sampleCount;
  double framesPerSecond;
  double averageFrameTimeMs;
  double p95FrameTimeMs;
  double jitterMs;
  // framesPerSecond with duplicated presentation slots scaled out: the rate of frames that
  // carried new motion.
  double effectiveFramesPerSecond;
} AuroraPresentTiming;
typedef struct {
  uint32_t targetFps;
  uint32_t targetSamples;
  uint32_t activeSamples;
  uint32_t candidates;
  uint32_t matchable;
  uint32_t matches;
  uint32_t eligible;
  uint32_t replaySafe;
  uint64_t framesSealed;
  uint64_t framesLowMatch;
  uint64_t framesReplayUnsafe;
  uint64_t slotReductions;
  uint64_t lateSealDrops;
} AuroraFrameInterpolationDiagnostics;
void aurora_get_present_timing(AuroraPresentTiming*);
void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics*);
void aurora_set_frame_interpolation_fps(uint32_t);
uint32_t aurora_get_frame_interpolation_fps(void);
void aurora_set_frame_interpolation_max_translation(float);
#ifdef __cplusplus
}
#endif
