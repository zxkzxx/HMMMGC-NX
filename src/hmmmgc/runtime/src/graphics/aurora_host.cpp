#include <cstdint>
extern "C" { double hmmmgc_take_sleep_ms(); double hmmmgc_take_ax_ms(); double hmmmgc_take_copydisp_ms(); int switch_anchor_threads_pinned(); }
static double g_prof_post_ms = 0.0;
namespace gx_draw { double take_prof_ms(uint32_t* calls_out); }
// See aurora_host.h.

#include "graphics/aurora_host.h"

#include "audio/audio_output.h"
#include "controls_editor.h"
#include "game_clock.h"
#include "game_scene.h"
#include "ground_mesh.h"
#include "graphics/camera_view.h"
#include "graphics/character_range.h"
#include "graphics/far_camera.h"
#include "graphics/frame_interpolation.h"
#include "graphics/map_draw_range.h"
#include "graphics/shadow_range.h"
#include "in_game_menu.h"
#include "input/touch_buttons.h"
#include "player_folder.h"
#include "player_options.h"
#include "runtime_log.h"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <aurora/gfx.h>
#include <dolphin/vi.h>

#include <SDL3/SDL.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <thread>

namespace aurora_host {
namespace {

constexpr const char* WINDOW_TITLE = "Harvest Moon: Magical Melody";
// The window starts at the size of the console's picture and can be resized. (The game
// copies 640x448 to the screen, and the video interface centres it in the 480 lines
// with a border above and below; aurora keeps its proportions the same way.)
constexpr uint32_t WINDOW_WIDTH = 640;
constexpr uint32_t WIDESCREEN_WINDOW_WIDTH = 854;
constexpr uint32_t WINDOW_HEIGHT = 480;
// How long to wait for the window to accept frames again (while it is minimized).
constexpr auto WAIT_FOR_WINDOW = std::chrono::milliseconds(16);

bool g_initialized = false;
bool g_window_closed = false;

// Where aurora keeps its settings (controller mappings, texture replacements) and the
// memory card, and where it keeps its caches (compiled pipelines).
DisplayOptions g_display_options;
std::string g_user_folder;
std::string g_cache_folder;

// The frame rate, counted over each second and shown in the window's title with the
// game's speed when it is not 100% (the game presents 30 pictures a second at 100%; fewer
// means the PC is not keeping up).
struct FrameRate {
    std::chrono::steady_clock::time_point second_started = std::chrono::steady_clock::now();
    unsigned frames = 0;
};
FrameRate g_frame_rate;
double g_frames_per_second = 0;  // as last counted

void count_frame() {
    ++g_frame_rate.frames;
    const auto now = std::chrono::steady_clock::now();
    const std::chrono::duration<double> elapsed = now - g_frame_rate.second_started;
    if (elapsed.count() < 1.0) {
        return;
    }
    // With frames made in between, aurora shows them from its render thread: it counts them.
    double frames_per_second = g_frame_rate.frames / elapsed.count();
    if (frame_interpolation::is_on()) {
        AuroraPresentTiming timing{};
        aurora_get_present_timing(&timing);
        frames_per_second = timing.framesPerSecond;
    }
    g_frames_per_second = frames_per_second;
    char title[128];
    const unsigned speed = game_clock::speed_percent();
    if (speed == game_clock::NORMAL_SPEED_PERCENT) {
        std::snprintf(title, sizeof(title), "%s - %.0f fps", WINDOW_TITLE, frames_per_second);
    } else {
        std::snprintf(title, sizeof(title), "%s - %.0f fps - %u%%", WINDOW_TITLE, frames_per_second, speed);
    }
    SDL_SetWindowTitle(aurora_get_window(), title);
    g_frame_rate = FrameRate{now, 0};
}

void log_from_aurora(AuroraLogLevel level, const char* module, const char* message, unsigned int length) {
    const int text_length = static_cast<int>(length);
    switch (level) {
        case LOG_DEBUG:
        case LOG_INFO:
            runtime_log::info("aurora", "%s: %.*s", module, text_length, message);
            return;
        case LOG_WARNING:
            runtime_log::warning("aurora", "%s: %.*s", module, text_length, message);
            return;
        case LOG_ERROR:
        case LOG_FATAL:
            runtime_log::error("aurora", "%s: %.*s", module, text_length, message);
            return;
    }
}

// aurora draws into the frame opened by aurora_begin_frame. It refuses to open one while
// the window cannot show anything (minimized), so wait for it, still handling events.
void begin_next_frame() {
    while (!aurora_begin_frame()) {
        static int sp = 0;
        if (++sp == 1 || sp == 100 || (sp % 50000) == 0) { runtime_log::info("probe-gfx", "begin_frame spin=%d", sp); }
        if (!process_events()) {
            return;
        }
        std::this_thread::sleep_for(WAIT_FOR_WINDOW);
    }
}

void exit_if_window_closed() {
    if (g_window_closed) {
        runtime_log::info("graphics", "the window was closed");
        player_options::save_if_changed();  // what the player changed in the options menu
        shutdown();
        std::exit(EXIT_SUCCESS);
    }
}

// The resampler that reduces a picture drawn larger than the window: averaging every pixel it
// covers (rather than the four nearest, bilinear) keeps the edges smooth without the shimmer of
// skipped pixels.
void apply_resolution_scale(float resolution_scale) {
    /* HMMMGC_SWITCH_AURORA_HOST */
#if !defined(__SWITCH__)
    VISetFrameBufferScale(resolution_scale);
#else
    (void)resolution_scale;
#endif
    aurora_set_resampler(resolution_scale > 0.f ? SAMPLER_AREA : SAMPLER_BILINEAR);
}

}  // namespace

void configure(const DisplayOptions& options) { g_display_options = options; }

void initialize() {
    if (g_initialized) {
        return;
    }
    const std::filesystem::path cache_folder = player_folder::path() / "Cache";
    std::filesystem::create_directories(cache_folder);  // aurora's databases do not create it
    g_user_folder = player_folder::path().string();
    g_cache_folder = cache_folder.string();

    AuroraConfig config{};
    config.appName = WINDOW_TITLE;
    config.desiredBackend = BACKEND_AUTO;
    // A software device (such as Mesa's lavapipe) is accepted when there is no GPU, which
    // is how the runtime is tested on machines without one.
    config.allowCpuAdapter = true;
    // The game paces itself on the video interface's retraces (hardware/video_interface.h);
    // waiting for the screen's refresh as well would pace it twice.
    config.vsync = false;
#if defined(__SWITCH__)
    config.blockingPipelines = true; /* HMMMGC_FIX_BLOCKPIPE */
#endif
    // A 16:9 window for the widescreen picture: as tall as the 4:3 one, and wider.
    config.windowWidth = camera_view::widescreen() ? WIDESCREEN_WINDOW_WIDTH : WINDOW_WIDTH;
    config.windowHeight = WINDOW_HEIGHT;
    config.startFullscreen = g_display_options.fullscreen;
    config.logCallback = log_from_aurora;
    config.logLevel = LOG_INFO;
    // The game's memory belongs to the runtime (guest_memory.h); aurora needs none of its own.
    config.mem1Size = 0;
    config.mem2Size = 0;
    // Given explicitly: aurora's default is a folder named after the window title, which
    // has a colon, and Windows does not allow one in a folder name.
    config.userPath = g_user_folder.c_str();
    config.cachePath = g_cache_folder.c_str();

    const AuroraInfo info = aurora_initialize(0, nullptr, &config);
#if !defined(__SWITCH__)
    if (info.initializationStatus != AURORA_INITIALIZATION_SUCCESS) {
        runtime_log::error("graphics", "no graphics device can be used: %s",
                           info.initializationError != nullptr ? info.initializationError : "unknown error");
        std::exit(EXIT_FAILURE);
    }
#endif
    camera_view::apply_to_aurora();
    frame_interpolation::apply_to_aurora();
    camera_view::set_window_size(info.windowSize.width, info.windowSize.height);
    if (g_display_options.resolution_scale > 0.f) {
        apply_resolution_scale(g_display_options.resolution_scale);
    }
    runtime_log::info("graphics", "HMMMGC_FIX_BLOCKPIPE+WIDESCREEN169: aurora backend %d in %ux%u, internal resolution %s",
                      static_cast<int>(info.backend), info.windowSize.width, info.windowSize.height,
                      g_display_options.resolution_scale > 0.f ? "fixed" : "following the window");
    g_initialized = true;
    begin_next_frame();
    exit_if_window_closed();
}

bool is_initialized() { return g_initialized; }

void present_frame() {
    touch_buttons::draw();
    in_game_menu::draw();
    frame_interpolation::schedule_frame();
    static int fr = 0;
    if (++fr <= 5 || (fr % 60) == 0) { runtime_log::info("probe-gfx", "end_frame #%d", fr); }
    {
        using clk = std::chrono::steady_clock;
        static auto s_after_end = clk::now();
        static double s_game_ms = 0.0, s_end_ms = 0.0;
        static uint32_t s_f = 0;
        const auto t0 = clk::now();
        aurora_end_frame();
        const auto t1 = clk::now();
        s_game_ms += std::chrono::duration<double, std::milli>(t0 - s_after_end).count();
        s_end_ms += std::chrono::duration<double, std::milli>(t1 - t0).count();
        s_after_end = t1;
        if (++s_f % 60 == 0) {
            uint32_t gx_calls = 0;
            const double gx_ms = gx_draw::take_prof_ms(&gx_calls) / 60.0;
            const double sl_ms = hmmmgc_take_sleep_ms() / 60.0;
            const double ax_ms = hmmmgc_take_ax_ms() / 60.0;
            const double cd_ms = hmmmgc_take_copydisp_ms() / 60.0;
            const double po_ms = g_prof_post_ms / 60.0; g_prof_post_ms = 0.0;
            const double tot_ms = (s_game_ms + s_end_ms) / 60.0;
            const double en_ms = s_end_ms / 60.0;
            const double ppc_ms = tot_ms - gx_ms - sl_ms - ax_ms - cd_ms - po_ms - en_ms;
            runtime_log::info("prof", "frame=%u tot=%.2f | ppc=%.2f draw=%.2f(%u) cpy=%.2f post=%.2f sleep=%.2f ax=%.2f end=%.2f | pin=%d",
                              s_f, tot_ms, ppc_ms, gx_ms, gx_calls / 60u, cd_ms, po_ms, sl_ms, ax_ms, en_ms, switch_anchor_threads_pinned());
            s_game_ms = 0.0; s_end_ms = 0.0;
        }
    }
    const auto _post_t0 = std::chrono::steady_clock::now();
    count_frame();
    process_events();
    if (in_game_menu::quit_requested()) {
        g_window_closed = true;  // the menu's "Quit the game", as the window's close button
    }
    // What the next frame shows: the game's camera, or the zoom while walking outdoors, and
    // as much of the map, its characters, its shadows and the farm's tilled soil as that shows.
    camera_view::set_zoom(far_camera::zoom());
    camera_view::follow_scene(game_scene::player_walks_outdoors());
    map_draw_range::update();
    character_range::update();
    shadow_range::update();
    ground_mesh::follow_zoom(camera_view::zoom());
    // The turbo: the whole game runs at game_clock::TURBO_SPEED_PERCENT while its key (Tab, or
    // the one chosen in the controls), its gamepad button or the touch button TURBO is held; not
    // while the options menu is open, where Tab moves between its controls.
    game_clock::set_turbo(!in_game_menu::is_open() && (controls_editor::turbo_held() || touch_buttons::turbo_held()));
    // The menu's shortcut on the controller and its lettering for the window's size, before the
    // next frame starts (and with it ImGui's).
    in_game_menu::before_frame();
    begin_next_frame();
    g_prof_post_ms += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - _post_t0).count();
    exit_if_window_closed();
}

bool process_events() {
    for (const AuroraEvent* event = aurora_update(); event != nullptr && event->type != AURORA_NONE; ++event) {
        if (event->type == AURORA_EXIT) {
            g_window_closed = true;
        } else if (event->type == AURORA_SDL_EVENT) {
            touch_buttons::handle_event(event->sdl);
            in_game_menu::handle_event(event->sdl);
        } else if (event->type == AURORA_WINDOW_RESIZED) {
            const AuroraWindowSize& size = event->windowSize;
            camera_view::set_window_size(size.width, size.height);
            runtime_log::info("graphics", "window %ux%u, the game is drawn at %ux%u", size.width, size.height,
                              size.fb_width, size.fb_height);
        }
    }
    return !g_window_closed;
}

double frames_per_second() { return g_frames_per_second; }

void set_resolution_scale(float resolution_scale) {
    g_display_options.resolution_scale = resolution_scale;
    if (g_initialized) {
        apply_resolution_scale(resolution_scale);
        runtime_log::info("graphics", "internal resolution %s",
                          resolution_scale > 0.f ? "fixed" : "following the window");
    }
}

void set_fullscreen(bool fullscreen) {
    g_display_options.fullscreen = fullscreen;
    if (g_initialized) {
        aurora_set_display_mode(fullscreen ? AURORA_DISPLAY_MODE_BORDERLESS : AURORA_DISPLAY_MODE_WINDOWED);
    }
}

bool is_fullscreen() {
    return g_initialized ? aurora_get_display_mode() != AURORA_DISPLAY_MODE_WINDOWED : g_display_options.fullscreen;
}

void set_widescreen(bool widescreen) {
    camera_view::set_widescreen(widescreen);
    if (!g_initialized || is_fullscreen()) {
        return;
    }
    SDL_Window* window = aurora_get_window();
    int width = 0;
    int height = 0;
    if (window != nullptr && SDL_GetWindowSize(window, &width, &height) && height > 0) {
        const int wanted = widescreen ? height * 16 / 9 : height * 4 / 3;
        SDL_SetWindowSize(window, wanted, height);
    }
}

void shutdown() {
    if (g_initialized) {
        g_initialized = false;
        audio_output::shutdown();  // the sound plays through the SDL that aurora started
        aurora_shutdown();
    }
}

}  // namespace aurora_host
