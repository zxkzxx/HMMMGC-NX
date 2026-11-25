#pragma once
#include_next <aurora/aurora.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef enum {
  AURORA_DISPLAY_MODE_WINDOWED,
  AURORA_DISPLAY_MODE_BORDERLESS,
  AURORA_DISPLAY_MODE_EXCLUSIVE,
} AuroraDisplayMode;
void aurora_set_present_schedule(uint64_t, uint64_t);
void aurora_report_producer_paced(bool);
void aurora_set_display_mode(AuroraDisplayMode);
AuroraDisplayMode aurora_get_display_mode(void);
bool aurora_display_shows_interpolation(void);
SDL_Window* aurora_get_window(void);
AuroraWindowSize aurora_get_window_size(void);
#ifdef __cplusplus
}
#endif
