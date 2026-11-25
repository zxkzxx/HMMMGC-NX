#include <atomic>
#include <switch.h>
#include <cstdio>
#include <cstdlib>
#include <cstdint>
#include <pthread.h>
#include <SDL3/SDL.h>
#include <dolphin/gx.h>


namespace {
struct SwitchCoreTrampArg { void* (*fn)(void*); void* arg; int core; };
std::atomic<int> g_nx_core_rr{0};
} // namespace
extern "C" int switch_anchor_threads_pinned() { return g_nx_core_rr.load(std::memory_order_relaxed); }
extern "C" void* switch_core_trampoline(void* raw) {
    auto* a = static_cast<SwitchCoreTrampArg*>(raw);
    auto fn = a->fn;
    auto arg = a->arg;
    const int core = a->core;
    delete a;
    svcSetThreadCoreMask(CUR_THREAD_HANDLE, core, (1u << core));
    return fn(arg);
}

extern "C" {
int __real_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg);
int __wrap_pthread_create(pthread_t* t, const pthread_attr_t* a, void* (*fn)(void*), void* arg) {
    const int core = 1 + (g_nx_core_rr.fetch_add(1, std::memory_order_relaxed) & 1);
    auto* wrap = new SwitchCoreTrampArg{fn, arg, core};
    if (a != nullptr) {
        int rc = __real_pthread_create(t, a, switch_core_trampoline, wrap);
        if (rc != 0) delete wrap;
        return rc;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 2 * 1024 * 1024);
    int rc = __real_pthread_create(t, &attr, switch_core_trampoline, wrap);
    pthread_attr_destroy(&attr);
    if (rc != 0) delete wrap;
    return rc;
}
const char* SDL_GetScancodeName(SDL_Scancode) { return ""; }
bool SDL_SetCurrentThreadPriority(SDL_ThreadPriority) { return true; }
SDL_Window* SDL_GetWindowFromEvent(const SDL_Event*) { return nullptr; }
bool SDL_RenamePath(const char* o, const char* n) { return o && n && std::rename(o, n) == 0; }
SDL_GamepadButtonLabel SDL_GetGamepadButtonLabelForType(SDL_GamepadType, SDL_GamepadButton) { return SDL_GAMEPAD_BUTTON_LABEL_UNKNOWN; }
bool SDL_SetError(const char*, ...) { return false; }
Uint32 SDL_rand_bits(void) { return static_cast<Uint32>(std::rand()); }
bool SDL_ReadU8(SDL_IOStream* s, Uint8* v) { return s && v && SDL_ReadIO(s, v, 1) == 1; }
bool SDL_ReadU16LE(SDL_IOStream* s, Uint16* v) { Uint8 b[2]; if (!s || !v || SDL_ReadIO(s, b, 2) != 2) return false; *v = b[0] | (b[1] << 8); return true; }
bool SDL_WriteU16LE(SDL_IOStream* d, Uint16 v) { Uint8 b[2] = {Uint8(v & 0xFF), Uint8(v >> 8)}; return d && SDL_WriteIO(d, b, 2) == 2; }
void PADRestoreDefaultKeyBindings(uint32_t) {}
void PADSetDefaultKeyBindings(uint32_t, const void*) {}
void GXSetMisc(GXMiscToken, u32) {}
}
struct ImGui_ImplWGPU_InitInfo; struct ImDrawData; struct WGPURenderPassEncoderImpl;
enum ImGui_ImplSDL3_GamepadMode : int;
bool ImGui_ImplWGPU_Init(ImGui_ImplWGPU_InitInfo*) { return true; }
void ImGui_ImplWGPU_Shutdown() {}
void ImGui_ImplWGPU_NewFrame() {}
bool ImGui_ImplWGPU_CreateDeviceObjects() { return true; }
void ImGui_ImplWGPU_RenderDrawData(ImDrawData*, WGPURenderPassEncoderImpl*) {}
void ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode, SDL_Gamepad**, int) {}


extern "C" void cos_switch_thread_role(int) {}
extern "C" void cos_switch_flush_logs(void) { std::fflush(stdout); std::fflush(stdout); }
void JKRPcBeginHostAlloc() {}

extern "C" int pc_aniso_get(void) { return 1; }
extern "C" float pc_dynres_last_3d_scale(void) { return 1.0f; }
