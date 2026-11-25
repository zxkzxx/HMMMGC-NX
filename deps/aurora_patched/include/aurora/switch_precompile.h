#pragma once

/* Switch build (switch/native/aurora/patches): the boot pipeline warm-up. At start Aurora queues every
 * pipeline its pipeline cache knows (the player's pipeline_cache.db, seeded from the bundled
 * initial_pipeline_cache.db) for its compile thread, in order of first use. This reports how far
 * that got, and ends it: on the Switch every pipeline build holds the one GL context for 0.1-0.4 s,
 * so warming up is only worth it while the game shows logos and menus. */

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  uint32_t total;     /* pipelines the warm-up queued */
  uint32_t done;      /* of those, built (by the warm-up, or first because a draw needed it) */
  uint32_t pending;   /* still to build */
  uint32_t dropped;   /* taken off the queue by aurora_switch_stop_precompile */
  uint64_t compileNs; /* compile thread time spent on the built ones */
  bool stopped;       /* aurora_switch_stop_precompile was called */
  /* The priority set: the pipelines the bundled initial_pipeline_cache.db lists in its
   * pipeline_priority table with priority 0 (native/tools/gen_pipeline_cache.sh: those recorded on
   * the boot path, logos to Outset). The warm-up queues them first. 0/0 without that table. */
  uint32_t priorityTotal; /* of the warm-up's pipelines (total), those in the priority set */
  uint32_t priorityDone;  /* of those, built */
  /* The warm-up throttle (aurora_switch_set_warmup_throttle): its duty (0 off), the warm-up builds
   * it held back and the compile thread time it spent waiting. */
  float throttleDuty;
  uint32_t throttled;
  uint64_t throttleWaitNs;
  /* Warm-up builds the throttle let the next one follow at once: they took no longer than
   * aurora_switch_set_warmup_slow_ns (a shader cache hit). */
  uint32_t unthrottled;
  /* Of compileNs, the time the warm-up's builds waited for the GL context while another thread
   * (the render worker) held it: not shader work. compileNs - contextWaitNs is what the builds
   * themselves cost (Dawn's dawn_switch_gl_thread_pipeline_ns; 0 without it). */
  uint64_t contextWaitNs;
} AuroraSwitchPrecompile;

/* A slow build on the compile thread (longer than aurora_switch_set_warmup_slow_ns): which
 * pipeline and where its time went. */
typedef struct {
  uint64_t seq;         /* 1, 2, ... in the order they happened */
  uint64_t hash;        /* the runtime pipeline key (PipelineRef) */
  uint64_t ns;          /* the whole build */
  uint64_t translateNs; /* Tint's WGSL -> GLSL translation */
  uint64_t waitNs;      /* waiting for the GL context (another thread held it) */
  uint64_t heldNs;      /* holding it: Mesa's compile/link or its shader cache lookups */
  uint32_t warmupDone;  /* the warm-up's done count when it finished */
  bool warmup;          /* a warm-up pipeline (else first built for a draw) */
} AuroraSwitchSlowBuild;

/* Copies the slow builds with seq > *sinceSeq still remembered (the last 32), oldest first, up to
 * max; sets *sinceSeq to the last one copied. Returns how many. */
uint32_t aurora_switch_get_slow_builds(AuroraSwitchSlowBuild* out, uint32_t max, uint64_t* sinceSeq);

void aurora_switch_get_precompile(AuroraSwitchPrecompile* out);

/* Ends the warm-up: the pipelines it has not built yet (and no draw has asked for) leave the
 * compile queue and are built when first drawn; a later render target layout change queues none.
 * Returns how many were dropped. */
uint32_t aurora_switch_stop_precompile(void);

/* Throttles the rest of the warm-up so the frames keep coming: each pipeline build holds the GL
 * context the render worker needs for 0.1-0.3 s, so back to back builds leave the logos and menus
 * at 4-7 frames/s. With duty d in (0, 1), a warm-up build (one no draw is waiting for) starts only
 * after the compile thread has stayed idle for (1 - d) / d times the previous warm-up build's
 * duration, and then right after the next present (or 100 ms later without one), so the worker
 * gets about 1 - d of the time. Builds a draw asks for are never held back. 0 or >= 1: off (the
 * default; back to back as before). */
void aurora_switch_set_warmup_throttle(float duty);

/* The throttle only holds back the build after a slow one: a warm-up build that took at most ns
 * (default 25 ms, about a frame: with Mesa's shader cache warm a build costs ~12 ms, a miss
 * 100-300 ms) lets the next start at once, so a warm cache warms up back to back. 0: every build
 * is followed by the throttle's idle time, as before. */
void aurora_switch_set_warmup_slow_ns(uint64_t ns);

#ifdef __cplusplus
}
#endif
