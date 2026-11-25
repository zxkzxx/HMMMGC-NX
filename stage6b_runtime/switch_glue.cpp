#include "fault_handler.h"
#include "runtime_log.h"
#include <aurora/aurora.h>
#include <aurora/gfx.h>
#include <cstring>
namespace fault_handler{void install(unsigned){} void print_host_backtrace(int){runtime_log::error("runtime","(switch backtrace unavailable)");}}
extern "C"{
static AuroraDisplayMode g_dm{};
void aurora_set_present_schedule(uint64_t,uint64_t){}
void aurora_report_producer_paced(bool){}
void aurora_set_display_mode(AuroraDisplayMode m){g_dm=m;}
AuroraDisplayMode aurora_get_display_mode(void){return g_dm;}
bool aurora_display_shows_interpolation(void){return false;}
SDL_Window* aurora_get_window(void){return nullptr;}
AuroraWindowSize aurora_get_window_size(void){AuroraWindowSize w{}; return w;}
void aurora_get_present_timing(AuroraPresentTiming* t){if(t)std::memset(t,0,sizeof(*t));}
void aurora_get_frame_interpolation_diagnostics(AuroraFrameInterpolationDiagnostics* d){if(d)std::memset(d,0,sizeof(*d));}
void aurora_set_frame_interpolation_fps(uint32_t){}
uint32_t aurora_get_frame_interpolation_fps(void){return 0;}
void aurora_set_frame_interpolation_max_translation(float){}
}
