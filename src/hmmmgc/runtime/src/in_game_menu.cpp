// See in_game_menu.h.

#include "in_game_menu.h"
#include "input/touch_buttons.h"

#include "crop_grid.h"
#include "controls_editor.h"
#include "graphics/aurora_host.h"
#include "mods/native_mods.h"
#include "player_options.h"
#include "runtime_log.h"
#include "settings.h"
#include "texts.h"
#include "theme.h"

#include <aurora/aurora.h>
#include <backends/imgui_impl_sdl3.h>
#include <imgui.h>

#include <SDL3/SDL_gamepad.h>
#include <SDL3/SDL_video.h>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace in_game_menu {
namespace {

using texts::Text;
using texts::text;

// The sizes are the launcher's, made for a window of 1280x720 (shared/theme.h); in a smaller
// window the menu shrinks with it, down to where its lettering is still easy to read.
constexpr float DESIGN_WIDTH = 1280.f;
constexpr float DESIGN_HEIGHT = 720.f;
constexpr float SMALLEST_SCALE = 0.75f;

// A stick moved less than this (of 127), or a trigger pressed less than this (of 255), is let go.
constexpr int STICK_AT_REST = 24;
constexpr int TRIGGER_AT_REST = 40;

bool g_open = false;
bool g_controls_window = false;     // the window for the keys and buttons is showing
bool g_asking_to_quit = false;      // the question before quitting is showing
float g_bar_height = 0.f;           // of the bar at the top, as last drawn
bool g_quit = false;
bool g_waiting_for_release = false;  // the menu closed; the buttons that closed it are still held
bool g_select_held = false;         // the controller's Select button, as last seen
bool g_b_held = false;              // its button that is the game's B, as last seen
bool g_b_pressed = false;           // pressed since the last frame
#if defined(__SWITCH__)
int g_switch_tab = 0;
bool g_l_held = false, g_r_held = false;
bool g_l_pressed = false, g_r_pressed = false;
bool g_focus_tab_body = false;
bool g_gamepad_fed = false;
#endif
bool g_ready = false;               // the language and ImGui's settings are set
// The lettering was made before this frame started: ImGui builds it as a frame starts, so a menu
// opened between frames (by an event read while the game loads) waits for the next one.
bool g_lettering_ready = false;

// The menu is in Spanish when that is the language of the settings, and in English for the others.
void apply_language() {
    const std::string language = settings::GAME_LANGUAGES[player_options::current().language()];
    texts::set_language(language == "es" ? texts::Language::Spanish : texts::Language::English);
}

// The language of the settings, and nothing of ImGui kept between runs.
void get_ready() {
    if (g_ready) {
        return;
    }
    g_ready = true;
    apply_language();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.LogFilename = nullptr;
}

// The controller that plays as the game's port 1, the one aurora chose.
SDL_Gamepad* first_port_gamepad() {
    const s32 index = PADGetIndexForPort(PAD_CHAN0);
    return index >= 0 ? PADGetSDLGamepadForIndex(static_cast<u32>(index)) : nullptr;
}

// Whether the game plays something with that controller's Select button (the Nintendo Switch
// Online GameCube controller has Z there): then the button does not open the menu.
bool game_uses_select() {
    u32 count = 0;
    const PADButtonMapping* mappings = PADGetButtonMappings(PAD_CHAN0, &count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i) {
        if (mappings[i].nativeButton == SDL_GAMEPAD_BUTTON_BACK) {
            return true;
        }
    }
    return false;
}

// The controller's button that is the game's B (aurora's mapping): it goes back in the menu, as
// B does in the game's own menus. (The game's A, the one at the bottom of the face, is the one
// that ImGui takes for choosing.)
#if defined(__SWITCH__)
SDL_GamepadButton game_a_button() {
    u32 count = 0;
    const PADButtonMapping* mappings = PADGetButtonMappings(PAD_CHAN0, &count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i) {
        if (mappings[i].padButton == PAD_BUTTON_A && mappings[i].nativeButton < SDL_GAMEPAD_BUTTON_COUNT) {
            return static_cast<SDL_GamepadButton>(mappings[i].nativeButton);
        }
    }
    return SDL_GAMEPAD_BUTTON_SOUTH;
}
#endif

SDL_GamepadButton game_b_button() {
    u32 count = 0;
    const PADButtonMapping* mappings = PADGetButtonMappings(PAD_CHAN0, &count);
    for (u32 i = 0; mappings != nullptr && i < count; ++i) {
        if (mappings[i].padButton == PAD_BUTTON_B && mappings[i].nativeButton < SDL_GAMEPAD_BUTTON_COUNT) {
            return static_cast<SDL_GamepadButton>(mappings[i].nativeButton);
        }
    }
    return SDL_GAMEPAD_BUTTON_EAST;
}

void open() {
    get_ready();
    g_open = true;
    g_controls_window = false;
    g_asking_to_quit = false;
    player_options::note_fullscreen(aurora_host::is_fullscreen());
    // The keyboard and the controller move between the menu's controls; ImGui reads the same
    // controller as the game.
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    SDL_Gamepad* gamepad = first_port_gamepad();
    ImGui_ImplSDL3_SetGamepadMode(ImGui_ImplSDL3_GamepadMode_Manual, &gamepad, gamepad != nullptr ? 1 : 0);
#if defined(__SWITCH__)
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    g_focus_tab_body = true;
    runtime_log::info("menu", "HMMMGC_SWITCH_MENU: Switch-native options menu opened");
#else
    runtime_log::info("menu", "the options menu is open");
#endif
}

void close() {
    g_open = false;
    g_asking_to_quit = false;
    g_controls_window = false;
    g_waiting_for_release = true;
    controls_editor::stop();
    ImGui::GetIO().ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
    player_options::save_if_changed();
    runtime_log::info("menu", "the options menu is closed");
}

void toggle() {
    if (g_open) {
        close();
    } else {
        open();
    }
}

// The lettering for the window's size: the fonts are built again only when it changed.
void fit_to_window() {
    int width = 0;
    int height = 0;
    if (SDL_Window* win = aurora_get_window()) {
        SDL_GetWindowSize(win, &width, &height);
    }
#if defined(__SWITCH__)
    if (width <= 0 || height <= 0) {
        const ImVec2 ds = ImGui::GetIO().DisplaySize;
        width = ds.x > 0.f ? static_cast<int>(ds.x) : 1280;
        height = ds.y > 0.f ? static_cast<int>(ds.y) : 720;
    }
#endif
    if (width <= 0 || height <= 0) {
        return;
    }
    theme::set_scale(std::max(SMALLEST_SCALE, std::min(static_cast<float>(width) / DESIGN_WIDTH,
                                                       static_cast<float>(height) / DESIGN_HEIGHT)));
}

bool is_held(const PADStatus& status) {
    return status.err == PAD_ERR_NONE &&
           (status.button != 0 || std::abs(status.stickX) > STICK_AT_REST || std::abs(status.stickY) > STICK_AT_REST ||
            std::abs(status.substickX) > STICK_AT_REST || std::abs(status.substickY) > STICK_AT_REST ||
            status.triggerLeft > TRIGGER_AT_REST || status.triggerRight > TRIGGER_AT_REST);
}

// Text in a dark label, centred under the bar at the top, like the notices of an emulator.
void draw_note(const char* note, float top) {
    ImFont* font = theme::fonts().small;
    ImDrawList* draw = ImGui::GetBackgroundDrawList();
    const ImVec2 extent = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, note);
    const ImVec2 padding(theme::px(10), theme::px(5));
    const ImVec2 min((ImGui::GetIO().DisplaySize.x - extent.x) * 0.5f - padding.x, top + theme::px(8));
    const ImVec2 max = min + extent + padding * 2;
    draw->AddRectFilled(min, max, IM_COL32(20, 20, 20, 200), theme::px(3));
    draw->AddText(font, font->FontSize, min + padding, IM_COL32(235, 235, 235, 255), note);
}

void draw_frame_rate(float top) {
    const std::string label =
        texts::format(Text::FrameRateCounter, static_cast<int>(std::lround(aurora_host::frames_per_second())));
    ImFont* font = theme::fonts().body;
    const ImVec2 extent = font->CalcTextSizeA(font->FontSize, FLT_MAX, 0.f, label.c_str());
    const ImVec2 position(ImGui::GetIO().DisplaySize.x - extent.x - theme::px(16), top + theme::px(10));
    theme::outlined_text(ImGui::GetForegroundDrawList(), font, font->FontSize, position, theme::WHITE,
                         IM_COL32(40, 36, 32, 230), theme::px(2.5f), label.c_str());
}

// The menu is plain, as the menus of an emulator are, and not like the launcher: ImGui's own dark
// colours and a bar at the top with drop-down menus, over the game. It is put on for the time the
// menu draws, and the style the game's other windows use is put back after.
class PlainLook {
public:
    PlainLook() : saved_(ImGui::GetStyle()) {
        ImGuiStyle& style = ImGui::GetStyle();
        ImGui::StyleColorsDark(&style);
        style.WindowRounding = 0.f;
        style.PopupRounding = theme::px(3);
        style.FrameRounding = theme::px(3);
        style.WindowBorderSize = 1.f;
        // Room between the edge of a window or a menu and what is in it (the launcher's style, which
        // the game's windows start from, has none: its panels bring their own).
        style.WindowPadding = ImVec2(theme::px(14), theme::px(12));
        style.FramePadding = ImVec2(theme::px(9), theme::px(5));
        style.ItemSpacing = ImVec2(theme::px(9), theme::px(7));
        style.ItemInnerSpacing = ImVec2(theme::px(8), theme::px(4));
        style.CellPadding = ImVec2(theme::px(8), theme::px(4));
        style.WindowTitleAlign = ImVec2(0.f, 0.5f);
        style.ScrollbarSize = theme::px(12);
        style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.07f, 0.08f, 0.10f, 0.94f);
        ImGui::PushFont(theme::fonts().small);
    }
    ~PlainLook() {
        ImGui::PopFont();
        ImGui::GetStyle() = saved_;
    }
    PlainLook(const PlainLook&) = delete;
    PlainLook& operator=(const PlainLook&) = delete;

private:
    ImGuiStyle saved_;
};

std::string resolution_label(int lines) { return lines == 0 ? text(Text::ResolutionWindow) : std::to_string(lines) + "p"; }

std::string frame_rate_label(int rate) {
    return rate == 30 ? text(Text::FrameRateGame) : texts::format(Text::FrameRateMade, rate);
}

// A switch of the settings, in a menu that stays open.
bool flag_item(settings::Settings& settings, const char* name, Text label) {
    bool value = settings.flag(name, false);
    if (!ImGui::Checkbox(text(label), &value)) {
        return false;
    }
    settings.set(name, value);
    return true;
}

// Whether the bar has room for the values beside the names of its menus ("Volume: 100%").
bool g_bar_is_wide = true;

bool draw_resolution_menu(settings::Settings& settings) {
    const int lines = settings.one_of(settings::RESOLUTION, settings::RESOLUTIONS);
    const std::string title = std::string(text(Text::Resolution)) +
                              (g_bar_is_wide ? ": " + resolution_label(lines) : std::string()) + "###resolution";
    bool changed = false;
    if (ImGui::BeginMenu(title.c_str())) {
        for (const int value : settings::RESOLUTIONS) {
            if (ImGui::MenuItem(resolution_label(value).c_str(), nullptr, value == lines)) {
                settings.set(settings::RESOLUTION, value);
                changed = true;
            }
        }
        ImGui::EndMenu();
    }
    return changed;
}

bool draw_picture_menu(settings::Settings& settings) {
    bool changed = false;
    if (ImGui::BeginMenu(text(Text::PictureSection))) {
        changed |= flag_item(settings, settings::FULLSCREEN, Text::Fullscreen);
        changed |= flag_item(settings, settings::WIDESCREEN, Text::Widescreen);
        const int rate = settings.one_of(settings::FRAME_RATE, settings::FRAME_RATES);
        const std::string rate_title = std::string(text(Text::FrameRate)) + ": " + std::to_string(rate) + "###rate";
        if (ImGui::BeginMenu(rate_title.c_str())) {
            for (const int value : settings::FRAME_RATES) {
                if (ImGui::MenuItem(frame_rate_label(value).c_str(), nullptr, value == rate)) {
                    settings.set(settings::FRAME_RATE, value);
                    changed = true;
                }
            }
            ImGui::EndMenu();
        }
        changed |= flag_item(settings, settings::SHOW_FRAME_RATE, Text::ShowFrameRate);
#if defined(__ANDROID__)
        // The touch buttons' options, only where the buttons are part of playing: a computer shows
        // them only on a touch screen, and its launcher has these options too.
        changed |= flag_item(settings, settings::HIDE_TOUCH_BUTTONS, Text::HideTouchButtons);
        changed |= flag_item(settings, settings::HIDE_TOUCH_TURBO, Text::HideTouchTurbo);
#endif
        ImGui::EndMenu();
    }
    return changed;
}

bool draw_audio_menu(settings::Settings& settings) {
    int volume = settings.number(settings::VOLUME, 100, 0, 100);
    const std::string title = std::string(text(Text::Volume)) +
                              (g_bar_is_wide ? ": " + std::to_string(volume) + "%" : std::string()) + "###audio";
    bool changed = false;
    if (ImGui::BeginMenu(title.c_str())) {
        ImGui::SetNextItemWidth(theme::px(240));
        if (ImGui::SliderInt("##volume", &volume, 0, 100, "%d%%")) {
            settings.set(settings::VOLUME, volume);
            changed = true;
        }
        ImGui::EndMenu();
    }
    return changed;
}

// The colour of what tells the player that the game has to be restarted.
constexpr ImVec4 RESTART_COLOR(1.f, 0.82f, 0.35f, 1.f);

// The language of the game's dialogues: the game reads it when it starts (game_language.h).
void draw_language_menu() {
    settings::Settings& settings = player_options::current();
    const int chosen = settings.language();
    if (!ImGui::BeginMenu(text(Text::GameLanguageShort))) {
        return;
    }
    for (int i = 0; i < static_cast<int>(std::size(settings::GAME_LANGUAGES)); ++i) {
        if (ImGui::MenuItem(settings::GAME_LANGUAGE_NAMES[i], nullptr, i == chosen)) {
            player_options::set_game_language(settings::GAME_LANGUAGES[i]);
            apply_language();  // the menu changes at once; the game's dialogues, at the next start
        }
    }
    ImGui::Separator();
    ImGui::PushTextWrapPos(theme::px(340));
    ImGui::TextColored(RESTART_COLOR, "%s", text(Text::GameLanguageRestart));
    ImGui::PopTextWrapPos();
    ImGui::EndMenu();
}

// The name of a mod in the language of the menu; the ids this game's mods have are known here.
std::string mod_name(const std::string& id) {
    const std::pair<const char*, Text> names[] = {
        {"jamie_marriage", Text::ModNameJamieMarriage},
        {"player_speed", Text::ModNamePlayerSpeed},
        {"carry_speed", Text::ModNameCarrySpeed},
        {"far_camera", Text::ModNameFarCamera},
        {"marry_anyone", Text::ModNameMarryAnyone},
        {"longer_days", Text::ModNameLongerDays},
        {"gentle_stamina", Text::ModNameGentleStamina},
        {"faster_tools", Text::ModNameFasterTools},
    };
    for (const auto& [known, name] : names) {
        if (id == known) {
            return text(name);
        }
    }
    return id;
}

// How fast the player_speed mod makes the character, under its switch: the same number as in the
// launcher, in steps of 5.
void draw_player_speed(bool mod_on) {
    int percent = player_options::player_speed_percent();
    const std::string format = std::string(text(Text::PlayerSpeed)) + ": %d%%";
    ImGui::BeginDisabled(!mod_on);
    ImGui::Indent();
    ImGui::SetNextItemWidth(theme::px(260));
    if (ImGui::SliderInt("##player speed", &percent, settings::PLAYER_SPEED_MINIMUM, settings::PLAYER_SPEED_MAXIMUM,
                         format.c_str())) {
        const int step = settings::PLAYER_SPEED_STEP;
        const int rounded = (percent + step / 2) / step * step;
        if (rounded != player_options::player_speed_percent()) {
            player_options::set_player_speed_percent(rounded);  // the character has it from the next frame
        }
    }
    ImGui::Unindent();
    ImGui::EndDisabled();
}

// The mods that only make the game more comfortable ("category": "quality_of_life" in their
// mod.json), shown after the others under their own heading, as the launcher shows them.
bool is_quality_of_life(const std::string& id) {
    for (const char* comfort :
         {"player_speed", "carry_speed", "far_camera", "longer_days", "gentle_stamina", "faster_tools"}) {
        if (id == comfort) {
            return true;
        }
    }
    return false;
}

// One mod's switch, with when a change to it counts.
void draw_mod_switch(const settings::Settings& settings, const std::string& id) {
    bool on = settings.mod_enabled(id);
    const std::string label = mod_name(id) + "###" + id;
    if (ImGui::Checkbox(label.c_str(), &on)) {
        player_options::set_mod_enabled(id, on);
    }
    // Each mod says when a change to it counts: at once, or when the game is restarted.
    ImGui::SameLine();
    if (native_mods::switches_while_running(id)) {
        ImGui::TextDisabled("(%s)", text(Text::ModAppliesAtOnce));
    } else {
        ImGui::TextColored(RESTART_COLOR, "(%s)", text(Text::NeedsRestart));
    }
    if (id == "player_speed") {
        draw_player_speed(on);
    }
}

// The mods that are compiled in, with a switch each. The game keeps the mods it started with, so
// a change waits for the next start.
void draw_mods_menu() {
    if (!ImGui::BeginMenu(text(Text::Mods))) {
        return;
    }
    const settings::Settings& settings = player_options::current();
    std::vector<std::string> comfort;
    for (const std::string& id : native_mods::compiled_in()) {
        if (id == settings::CHEATS_MOD) {
            continue;  // on when a cheat is, in the launcher's options
        }
        if (is_quality_of_life(id)) {
            comfort.push_back(id);
        } else {
            draw_mod_switch(settings, id);
        }
    }
    if (!comfort.empty()) {
        ImGui::SeparatorText(text(Text::QualityOfLife));
        for (const std::string& id : comfort) {
            draw_mod_switch(settings, id);
        }
    }
    ImGui::EndMenu();
}

// The bar at the top: the picture, the sound, the mods and the keys, and at the right the way back
// to the game and the way out.
void draw_menu_bar(ImVec2 window) {
    if (!ImGui::BeginMainMenuBar()) {
        return;
    }
    settings::Settings& settings = player_options::current();
    bool changed = false;
    // The game's name and the values of the menus only where the bar has room for them.
    g_bar_is_wide = window.x >= theme::px(1100);
    if (g_bar_is_wide) {
        ImGui::TextDisabled("Harvest Moon MM");
    }
    changed |= draw_resolution_menu(settings);
    changed |= draw_picture_menu(settings);
    if (ImGui::MenuItem(text(Text::ControllerSettings))) {
        g_controls_window = true;
    }
    changed |= draw_audio_menu(settings);
    draw_language_menu();
    draw_mods_menu();
    if (changed) {
        player_options::apply_changes();
    }
    if (player_options::restart_pending()) {
        ImGui::TextColored(RESTART_COLOR, "%s", text(Text::RestartPending));
    }
    // At the right, in the order they are read from the end.
    const ImGuiStyle& style = ImGui::GetStyle();
    const float quit_width = ImGui::CalcTextSize(text(Text::QuitGame)).x + 2 * style.FramePadding.x;
    const float hide_width = ImGui::CalcTextSize(text(Text::HideMenu)).x + 2 * style.FramePadding.x;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX() + style.ItemSpacing.x,
                                  window.x - quit_width - hide_width - 4 * style.ItemSpacing.x - theme::px(8)));
    if (ImGui::MenuItem(text(Text::QuitGame))) {
        g_asking_to_quit = true;
    }
    if (ImGui::MenuItem(text(Text::HideMenu))) {
        close();
    }
    g_bar_height = ImGui::GetWindowSize().y;
    ImGui::EndMainMenuBar();
}

// "Quit the game?", over the menu, with "No" first.
void draw_quit_question() {
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    if (g_asking_to_quit && !ImGui::IsPopupOpen("###quit")) {
        ImGui::OpenPopup("###quit");
    }
    const std::string title = std::string(text(Text::QuitGame)) + "###quit";
    if (ImGui::BeginPopupModal(title.c_str(), &g_asking_to_quit,
                               ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoSavedSettings)) {
        ImGui::TextUnformatted(text(Text::QuitQuestion));
        ImGui::Spacing();
        ImGui::SetItemDefaultFocus();
        if (ImGui::Button(text(Text::QuitNo), ImVec2(theme::px(120), 0))) {
            g_asking_to_quit = false;
        }
        ImGui::SameLine();
        if (ImGui::Button(text(Text::QuitYes), ImVec2(theme::px(120), 0))) {
            runtime_log::info("menu", "the player quits the game");
            g_quit = true;
            close();
        }
        ImGui::EndPopup();
    }
}

#if defined(__SWITCH__)
int switch_nav_lr() {
    int d = 0;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickLeft, true)) --d;
    if (ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight, true) || ImGui::IsKeyPressed(ImGuiKey_GamepadLStickRight, true)) ++d;
    return d;
}

bool switch_toggle_row(const char* id, const char* label, const char* hint, bool* val, bool focus_first) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (hint && *hint) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetColumnWidth());
        ImGui::TextDisabled("%s", hint);
        ImGui::PopTextWrapPos();
    }
    ImGui::TableNextColumn();
    if (focus_first) ImGui::SetKeyboardFocusHere();
    ImGui::PushID(id);
    const std::string btn = *val ? std::string("[ ") + text(Text::ModOn) + " ]" : std::string("[ ") + text(Text::ModOff) + " ]";
    if (*val) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.18f, 0.56f, 0.24f, 1.0f));
    bool changed = ImGui::Button(btn.c_str(), ImVec2(-FLT_MIN, 0));
    if (*val) ImGui::PopStyleColor();
    if (ImGui::IsItemFocused()) {
        const int lr = switch_nav_lr();
        if ((lr < 0 && *val) || (lr > 0 && !*val)) changed = true;
    }
    ImGui::PopID();
    if (changed) *val = !*val;
    return changed;
}

bool switch_stepper_row(const char* id, const char* label, const char* hint, const std::string& val_text,
                        int* delta_out, bool focus_first, bool enabled = true) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::BeginDisabled(!enabled);
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    if (hint && *hint) {
        ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetColumnWidth());
        ImGui::TextDisabled("%s", hint);
        ImGui::PopTextWrapPos();
    }
    ImGui::TableNextColumn();
    if (focus_first && enabled) ImGui::SetKeyboardFocusHere();
    ImGui::PushID(id);
    const std::string btn = "<   " + val_text + "   >";
    const bool clicked = ImGui::Button(btn.c_str(), ImVec2(-FLT_MIN, 0));
    int d = 0;
    if (enabled && ImGui::IsItemFocused()) d = switch_nav_lr();
    if (enabled && clicked && d == 0) d = 1;
    ImGui::PopID();
    ImGui::EndDisabled();
    *delta_out = d;
    return d != 0;
}

void draw_switch_menu(ImVec2 window) {
    const PlainLook look;
    g_bar_height = 0.f;

    // L / R switch between tabs cleanly (not while remapping a button or confirming quit)
    if (!g_asking_to_quit && !controls_editor::busy()) {
        if (std::exchange(g_l_pressed, false)) {
            g_switch_tab = (g_switch_tab + 3) % 4;
            controls_editor::stop();
            g_focus_tab_body = true;
        }
        if (std::exchange(g_r_pressed, false)) {
            g_switch_tab = (g_switch_tab + 1) % 4;
            controls_editor::stop();
            g_focus_tab_body = true;
        }
    } else {
        g_l_pressed = false;
        g_r_pressed = false;
    }

    // Dim backdrop over the game
    ImGui::GetBackgroundDrawList()->AddRectFilled(ImVec2(0, 0), window, IM_COL32(8, 12, 20, 150));

    const ImVec2 win_size(std::min(window.x - theme::px(80), theme::px(900)),
                          std::min(window.y - theme::px(70), theme::px(600)));
    ImGui::SetNextWindowPos(ImVec2(window.x * 0.5f, window.y * 0.5f), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme::px(10));
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.08f, 0.09f, 0.12f, 0.96f));
    ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.92f, 0.62f, 0.24f, 0.90f));
    ImGui::PushStyleColor(ImGuiCol_NavHighlight, ImVec4(0.98f, 0.72f, 0.24f, 1.0f));

    if (ImGui::Begin("Harvest Moon: Magical Melody###switch_options", nullptr,
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse |
                     ImGuiWindowFlags_NoSavedSettings)) {
        // Tab header strip (non-navigable with Up/Down; switched cleanly via L / R)
        const char* tab_names[4] = {text(Text::Options), text(Text::Mods), text(Text::Cheats), text(Text::Controls)};
        const float tab_w = (ImGui::GetContentRegionAvail().x - theme::px(27)) / 4.f;
        for (int t = 0; t < 4; ++t) {
            if (t > 0) ImGui::SameLine();
            const bool active = (t == g_switch_tab);
            ImGui::PushStyleColor(ImGuiCol_Button, active ? ImVec4(0.88f, 0.50f, 0.14f, 1.0f)
                                                          : ImVec4(0.16f, 0.18f, 0.23f, 1.0f));
            ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
            if (ImGui::Button(tab_names[t], ImVec2(tab_w, theme::px(34)))) {
                g_switch_tab = t;
                controls_editor::stop();
                g_focus_tab_body = true;
            }
            ImGui::PopItemFlag();
            ImGui::PopStyleColor();
        }
        ImGui::Separator();

        settings::Settings& settings = player_options::current();
        bool changed = false;
        const bool focus_first = std::exchange(g_focus_tab_body, false);
        const float footer_h = theme::px(76);

        if (ImGui::BeginChild("##sw_body", ImVec2(0, -footer_h), ImGuiChildFlags_None)) {
            if (g_switch_tab == 0) {
                // Tab 0: Display, Audio & Language (Switch-only relevant options)
                if (ImGui::BeginTable("##sw_opts", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("lbl", ImGuiTableColumnFlags_WidthStretch, 0.58f);
                    ImGui::TableSetupColumn("ctl", ImGuiTableColumnFlags_WidthStretch, 0.42f);

                    bool wide = settings.flag(settings::WIDESCREEN, true);
                    if (switch_toggle_row("wide", text(Text::Widescreen), text(Text::WidescreenHint), &wide, focus_first)) {
                        settings.set(settings::WIDESCREEN, wide);
                        changed = true;
                    }
                    bool show_fps = settings.flag(settings::SHOW_FRAME_RATE, false);
                    if (switch_toggle_row("fps", text(Text::ShowFrameRate), text(Text::ShowFrameRateHint), &show_fps, false)) {
                        settings.set(settings::SHOW_FRAME_RATE, show_fps);
                        changed = true;
                    }
                    int vol = settings.number(settings::VOLUME, 100, 0, 100);
                    int vd = 0;
                    if (switch_stepper_row("vol", text(Text::Volume), text(Text::VolumeHint), std::to_string(vol) + "%", &vd, false)) {
                        vol = std::clamp(vol + vd * 5, 0, 100);
                        settings.set(settings::VOLUME, vol);
                        changed = true;
                    }
                    int lang = settings.language();
                    int ld = 0;
                    if (switch_stepper_row("lang", text(Text::GameLanguage), text(Text::GameLanguageRestart),
                                           settings::GAME_LANGUAGE_NAMES[lang], &ld, false)) {
                        const int n = static_cast<int>(std::size(settings::GAME_LANGUAGES));
                        lang = (lang + ld + n) % n;
                        player_options::set_game_language(settings::GAME_LANGUAGES[lang]);
                        apply_language();
                    }
                    ImGui::EndTable();
                }
            } else if (g_switch_tab == 1) {
                // Tab 1: Mods
                if (ImGui::BeginTable("##sw_mods", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("lbl", ImGuiTableColumnFlags_WidthStretch, 0.62f);
                    ImGui::TableSetupColumn("ctl", ImGuiTableColumnFlags_WidthStretch, 0.38f);
                    bool first = focus_first;
                    for (const std::string& id : native_mods::compiled_in()) {
                        if (id == settings::CHEATS_MOD) continue;
                        bool on = settings.mod_enabled(id);
                        const char* note = native_mods::switches_while_running(id)
                                               ? text(Text::ModAppliesAtOnce)
                                               : text(Text::NeedsRestart);
                        if (switch_toggle_row(id.c_str(), mod_name(id).c_str(), note, &on, first)) {
                            player_options::set_mod_enabled(id, on);
                        }
                        first = false;
                        if (id == "player_speed") {
                            int spd = player_options::player_speed_percent();
                            int sd = 0;
                            if (switch_stepper_row("spd", text(Text::PlayerSpeed), nullptr, std::to_string(spd) + "%", &sd, false, on)) {
                                spd = std::clamp(spd + sd * settings::PLAYER_SPEED_STEP,
                                                 settings::PLAYER_SPEED_MINIMUM, settings::PLAYER_SPEED_MAXIMUM);
                                player_options::set_player_speed_percent(spd);
                            }
                        }
                    }
                    ImGui::EndTable();
                }
            } else if (g_switch_tab == 2) {
                // Tab 2: Cheats
                ImGui::TextColored(RESTART_COLOR, "%s", text(Text::NeedsRestart));
                ImGui::Spacing();
                if (ImGui::BeginTable("##sw_cheats", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
                    ImGui::TableSetupColumn("lbl", ImGuiTableColumnFlags_WidthStretch, 0.62f);
                    ImGui::TableSetupColumn("ctl", ImGuiTableColumnFlags_WidthStretch, 0.38f);
                    struct CheatRow { const char* key; Text name; Text hint; };
                    const CheatRow rows[] = {
                        {"infiniteStamina", Text::CheatInfiniteStamina, Text::CheatInfiniteStaminaHint},
                        {"infiniteMoney", Text::CheatInfiniteMoney, Text::CheatInfiniteMoneyHint},
                        {"stopTime", Text::CheatStopTime, Text::CheatStopTimeHint},
                        {"maxFriendship", Text::CheatMaxFriendship, Text::CheatMaxFriendshipHint},
                    };
                    bool first = focus_first;
                    for (const CheatRow& r : rows) {
                        bool on = settings.cheat(r.key);
                        if (switch_toggle_row(r.key, text(r.name), text(r.hint), &on, first)) {
                            settings.set_cheat(r.key, on);
                            changed = true;
                        }
                        first = false;
                    }
                    ImGui::EndTable();
                }
            } else {
                // Tab 3: Switch Controller Remapping
                controls_editor::draw_embedded();
            }
        }
        ImGui::EndChild();

        if (changed) {
            player_options::apply_changes();
        }

        ImGui::Separator();
        if (player_options::restart_pending()) {
            ImGui::TextColored(RESTART_COLOR, "%s", text(Text::RestartPending));
        } else {
            ImGui::TextDisabled("%s", text(Text::MenuOpenNote));
        }
        if (ImGui::Button(text(Text::HideMenu), ImVec2(theme::px(180), 0))) {
            close();
        }
        ImGui::SameLine();
        if (ImGui::Button(text(Text::QuitGame), ImVec2(theme::px(180), 0))) {
            g_asking_to_quit = true;
        }
    }
    ImGui::End();
    ImGui::PopStyleColor(3);
    ImGui::PopStyleVar();

    draw_quit_question();
}
#endif

void draw_menu(ImVec2 window) {
#if defined(__SWITCH__)
    draw_switch_menu(window);
#else
    const PlainLook look;
    draw_menu_bar(window);
    if (g_controls_window && !controls_editor::draw_window(&g_controls_window)) {
        controls_editor::stop();
    }
    draw_quit_question();
#endif
}

}  // namespace

void handle_event(const SDL_Event& event) {
    if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat && event.key.scancode == SDL_SCANCODE_F10) {
        toggle();
    } else if (g_open) {
        controls_editor::handle_event(event);
    }
}


void before_frame() {
    SDL_Gamepad* gamepad = first_port_gamepad();
    const bool select =
        gamepad != nullptr && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_BACK) && !game_uses_select();
    if (select && !g_select_held) {
#if defined(__SWITCH__)
        if (g_open && controls_editor::busy()) {
            controls_editor::stop();
        } else {
            toggle();
        }
#else
        toggle();
#endif
    }
    g_select_held = select;
    const bool b = gamepad != nullptr && SDL_GetGamepadButton(gamepad, game_b_button());
    g_b_pressed = b && !g_b_held;
    g_b_held = b;
#if defined(__SWITCH__)
    const bool l_btn = gamepad != nullptr && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_LEFT_SHOULDER);
    const bool r_btn = gamepad != nullptr && SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER);
    g_l_pressed = l_btn && !g_l_held;
    g_r_pressed = r_btn && !g_r_held;
    g_l_held = l_btn;
    g_r_held = r_btn;

    // Feed Switch controller state directly into Dear ImGui (since Switch SDL3 shim has no ImGui SDL3 backend)
    if (ImGui::GetCurrentContext() != nullptr) {
        ImGuiIO& io = ImGui::GetIO();
        io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
        const bool feed = g_open && gamepad != nullptr && !controls_editor::busy();
        if (feed) {
            g_gamepad_fed = true;
            io.AddKeyEvent(ImGuiKey_GamepadDpadUp, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_UP));
            io.AddKeyEvent(ImGuiKey_GamepadDpadDown, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_DOWN));
            io.AddKeyEvent(ImGuiKey_GamepadDpadLeft, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_LEFT));
            io.AddKeyEvent(ImGuiKey_GamepadDpadRight, SDL_GetGamepadButton(gamepad, SDL_GAMEPAD_BUTTON_DPAD_RIGHT));
            io.AddKeyEvent(ImGuiKey_GamepadFaceDown, SDL_GetGamepadButton(gamepad, game_a_button()));
            io.AddKeyEvent(ImGuiKey_GamepadFaceRight, b);
            io.AddKeyEvent(ImGuiKey_GamepadL1, l_btn);
            io.AddKeyEvent(ImGuiKey_GamepadR1, r_btn);

            const auto stick_analog = [](Sint16 raw, bool neg) -> float {
                float v = static_cast<float>(raw) / 32767.f;
                if (neg) v = -v;
                constexpr float DZ = 0.25f;
                return v > DZ ? std::min(1.f, (v - DZ) / (1.f - DZ)) : 0.f;
            };
            const Sint16 lx = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTX);
            const Sint16 ly = SDL_GetGamepadAxis(gamepad, SDL_GAMEPAD_AXIS_LEFTY);
            const float sl = stick_analog(lx, true);
            const float sr = stick_analog(lx, false);
            const float su = stick_analog(ly, true);
            const float sd = stick_analog(ly, false);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickLeft, sl > 0.f, sl);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickRight, sr > 0.f, sr);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickUp, su > 0.f, su);
            io.AddKeyAnalogEvent(ImGuiKey_GamepadLStickDown, sd > 0.f, sd);
        } else if (g_gamepad_fed) {
            g_gamepad_fed = false;
            for (const ImGuiKey k : {ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft,
                                     ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight,
                                     ImGuiKey_GamepadL1, ImGuiKey_GamepadR1}) {
                io.AddKeyEvent(k, false);
            }
            for (const ImGuiKey k : {ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight,
                                     ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown}) {
                io.AddKeyAnalogEvent(k, false, 0.f);
            }
        }
    }
#endif
    if (g_open || player_options::show_frame_rate() || touch_buttons::visible()) {
        get_ready();
        fit_to_window();
        g_lettering_ready = true;
    }
}

void draw() {
    if (!g_lettering_ready) {
        return;
    }
    if (player_options::show_frame_rate()) {
        draw_frame_rate(g_open ? g_bar_height : 0.f);
    }
    if (!g_open) {
        return;
    }
    // Escape and the controller's B go back: from the question to the menu, from the menu to the
    // game. The controller's shoulder buttons change the page.
    const bool back = (ImGui::IsKeyPressed(ImGuiKey_Escape, false) || std::exchange(g_b_pressed, false)) &&
                      !controls_editor::busy();
    if (g_asking_to_quit) {
        if (back) {
            g_asking_to_quit = false;
        }
    } else if (back) {
        close();
        return;
    }
    const ImVec2 window = ImGui::GetIO().DisplaySize;
    draw_menu(window);
#if !defined(__SWITCH__)
    if (g_open) {
        draw_note(text(Text::MenuOpenNote), g_bar_height);
    }
#endif
}

bool is_open() { return g_open; }

void open_from_touch() {
    if (!g_open) {
        open();
    }
}

bool quit_requested() { return g_quit; }

void hold_back_controllers(PADStatus statuses[PAD_CHANMAX]) {
    if (!g_open && !g_waiting_for_release) {
        return;
    }
    if (!g_open && std::none_of(statuses, statuses + PAD_CHANMAX, is_held)) {
        g_waiting_for_release = false;
        return;
    }
    for (uint32_t port = 0; port < PAD_CHANMAX; ++port) {
        const s8 error = statuses[port].err;
        statuses[port] = PADStatus{};
        statuses[port].err = error;
    }
}

}  // namespace in_game_menu
