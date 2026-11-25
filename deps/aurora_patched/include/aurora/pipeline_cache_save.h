#ifndef AURORA_PIPELINE_CACHE_SAVE_H
#define AURORA_PIPELINE_CACHE_SAVE_H

#ifdef __cplusplus
extern "C" {
#endif

/**
 * SwitchWaker patch 0023: a GX pipeline first used in a run is written to the pipeline cache file once its build has
 * returned, so a build that crashes the process never reaches the file. This writes the ones still waiting for their
 * build and waits for every queued write, for a clean exit that skips aurora_shutdown (the harness's _Exit). Not after
 * a crash: the pipeline being built then may be the cause.
 */
void aurora_save_unbuilt_pipelines(void);

#ifdef __cplusplus
}
#endif

#endif
