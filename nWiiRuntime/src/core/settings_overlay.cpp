#include "runtime/settings_overlay.h"
#include "input/key_bindings.h"
#include "runtime/config.h"
#include "runtime/efb.h"
#include "ui_theme.h"

#include <glad/glad.h>
#include <SDL.h>
#include <imgui.h>
#include <imgui_impl_opengl3.h>
#include <imgui_impl_sdl2.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

extern uint64_t g_stat_frames; // game frames rendered (gx.cpp)

namespace nwii::runtime::ui {
namespace {

using namespace theme;

SDL_Window *s_window = nullptr;
bool s_ready = false;
bool s_open = false;
bool s_launcher = false; // pre-game launcher is up (settings open from it)
int s_open_tab = -1;     // tab to select when the settings window next opens
int s_max_msaa = 1, s_max_aniso = 1;
int s_capturing = -1; // PadControl being rebound, -1 = none
int s_pad_capturing = -1; // controller binding being captured, -1 = none
uint64_t s_pad_held = 0;  // inputs held when the capture started
Requests s_req;
std::string s_status;  // last save result, shown in the footer
double s_status_until = 0;

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// ---------------------------------------------------------------------------
// Saving: set `key = value` inside `[section]` of a TOML file, keeping every
// other line (comments, ordering) as the user wrote it. A trailing `# note`
// on the replaced line survives too.
std::string trim(const std::string &s) {
    size_t a = s.find_first_not_of(" \t"), b = s.find_last_not_of(" \t\r");
    return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

std::string section_of(const std::string &line) {
    const std::string t = trim(line);
    if (t.size() > 2 && t[0] == '[' && t[1] != '[') {
        const size_t e = t.find(']');
        if (e != std::string::npos) return trim(t.substr(1, e - 1));
    }
    return {};
}

void set_value(std::vector<std::string> &lines, const std::string &section,
               const std::string &key, const std::string &value) {
    int sec_start = -1, sec_end = (int)lines.size(), last_kv = -1;
    for (int i = 0; i < (int)lines.size(); ++i) {
        const std::string s = section_of(lines[i]);
        if (!s.empty()) {
            if (sec_start >= 0) { sec_end = i; break; }
            if (s == section) sec_start = i;
            continue;
        }
        if (sec_start < 0) continue;
        const std::string t = trim(lines[i]);
        if (t.empty() || t[0] == '#') continue;
        last_kv = i;
        const size_t eq = t.find('=');
        if (eq != std::string::npos && trim(t.substr(0, eq)) == key) {
            // Keep indentation and any trailing comment.
            const size_t ind = lines[i].find_first_not_of(" \t");
            std::string comment;
            const size_t hash = lines[i].find(" #", lines[i].find('='));
            if (hash != std::string::npos) comment = "  " + trim(lines[i].substr(hash));
            lines[i] = lines[i].substr(0, ind) + key + " = " + value + comment;
            return;
        }
    }
    if (sec_start < 0) {
        if (!lines.empty() && !trim(lines.back()).empty()) lines.push_back("");
        lines.push_back("[" + section + "]");
        lines.push_back(key + " = " + value);
        return;
    }
    lines.insert(lines.begin() + (last_kv >= 0 ? last_kv + 1 : sec_start + 1),
                 key + " = " + value);
    (void)sec_end;
}

std::string b2s(bool b) { return b ? "true" : "false"; }

// A TOML basic string (paths: backslashes and quotes escaped).
std::string toml_string(const std::string &v) {
    std::string out = "\"";
    for (char c : v) {
        if (c == '\\' || c == '"') out += '\\';
        out += c;
    }
    return out + "\"";
}

void note(const std::string &msg) {
    s_status = msg;
    s_status_until = now_s() + 4.0;
}

// Apply a change: save, and remember a short status for the footer.
void changed() {
    if (!save_settings()) note("Could not save settings");
}

std::string key_name(int sc) { return input::key_binding_name(sc); }

void set_open(bool open) {
    s_open = open;
    s_capturing = -1;
    s_pad_capturing = -1;
    if (s_ready) {
        // Keyboard/gamepad navigation only while the menu is up, so the game
        // keeps every key when it is closed.
        ImGuiIO &io = ImGui::GetIO();
        if (open || s_launcher)
            io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        else
            io.ConfigFlags &= ~(ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad);
        // Start the pointer over the menu (it may be on another monitor).
        if (open && s_window) {
            int w = 0, h = 0;
            SDL_GetWindowSize(s_window, &w, &h);
            SDL_WarpMouseInWindow(s_window, w / 2, h / 2);
        }
    }
}

// ---------------------------------------------------------------------------
void load_font() {
    ImGuiIO &io = ImGui::GetIO();
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(s_window, &w, &h);
    // Readable on a 1080p TV as well as a laptop: ~1/42 of the window height.
    const float px = std::clamp(h / 42.0f, 16.0f, 40.0f);
    const char *candidates[] = {"C:/Windows/Fonts/segoeui.ttf",
                                "/System/Library/Fonts/Supplemental/Arial.ttf",
                                "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"};
    ImFont *font = nullptr;
    for (const char *path : candidates)
        if (std::filesystem::exists(path) &&
            (font = io.Fonts->AddFontFromFileTTF(path, px)))
            break;
    if (!font) {
        ImFontConfig cfg;
        cfg.SizePixels = std::round(px / 13.0f) * 13.0f;
        io.Fonts->AddFontDefault(&cfg);
    }
    // Banner font for the themed menu frame (heavy weight, 2.6x).
    const char *bold[] = {"C:/Windows/Fonts/seguibl.ttf", "C:/Windows/Fonts/ariblk.ttf",
                          "/System/Library/Fonts/Supplemental/Arial Black.ttf"};
    for (const char *path : bold)
        if (std::filesystem::exists(path) &&
            (banner_font = io.Fonts->AddFontFromFileTTF(path, px * 2.6f))) {
            title_font = io.Fonts->AddFontFromFileTTF(path, px * 5.0f); // launcher title
            break;
        }
    ImGui::GetStyle().ScaleAllSizes(px / 16.0f);
}

void apply_style() {
    ImGui::StyleColorsDark();
    ImGuiStyle &st = ImGui::GetStyle();
    st.WindowRounding = 10.0f;
    st.FrameRounding = 6.0f;
    st.GrabRounding = 6.0f;
    st.TabRounding = 6.0f;
    st.WindowPadding = ImVec2(18, 16);
    st.FramePadding = ImVec2(10, 6);
    st.ItemSpacing = ImVec2(10, 9);
    ImVec4 *c = st.Colors;
    // Spyro purple accents on a dark, slightly translucent panel.
    c[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.06f, 0.12f, 0.94f);
    c[ImGuiCol_TitleBg] = c[ImGuiCol_TitleBgActive] = ImVec4(0.30f, 0.16f, 0.50f, 1.0f);
    c[ImGuiCol_Tab] = ImVec4(0.22f, 0.13f, 0.36f, 1.0f);
    c[ImGuiCol_TabHovered] = ImVec4(0.52f, 0.32f, 0.80f, 1.0f);
    c[ImGuiCol_TabSelected] = ImVec4(0.42f, 0.24f, 0.68f, 1.0f);
    c[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.13f, 0.28f, 1.0f);
    c[ImGuiCol_FrameBgHovered] = ImVec4(0.30f, 0.20f, 0.46f, 1.0f);
    c[ImGuiCol_FrameBgActive] = ImVec4(0.38f, 0.24f, 0.58f, 1.0f);
    c[ImGuiCol_Button] = ImVec4(0.32f, 0.20f, 0.52f, 1.0f);
    c[ImGuiCol_ButtonHovered] = ImVec4(0.50f, 0.32f, 0.78f, 1.0f);
    c[ImGuiCol_ButtonActive] = ImVec4(0.60f, 0.40f, 0.90f, 1.0f);
    c[ImGuiCol_CheckMark] = c[ImGuiCol_SliderGrabActive] = ImVec4(1.0f, 0.80f, 0.25f, 1.0f);
    c[ImGuiCol_SliderGrab] = ImVec4(0.95f, 0.70f, 0.20f, 1.0f);
    c[ImGuiCol_Header] = ImVec4(0.32f, 0.20f, 0.52f, 0.8f);
    c[ImGuiCol_HeaderHovered] = ImVec4(0.50f, 0.32f, 0.78f, 0.9f);
    c[ImGuiCol_NavHighlight] = ImVec4(1.0f, 0.80f, 0.25f, 1.0f);
    // Themed frame (draw_menu_frame): the panel is drawn behind the window,
    // controls get gold trim.
    st.FrameBorderSize = 1.0f;
    st.TabBorderSize = 1.0f;
    c[ImGuiCol_Border] = ImVec4(0.92f, 0.73f, 0.29f, 0.85f);
    c[ImGuiCol_Separator] = ImVec4(0.92f, 0.73f, 0.29f, 0.55f);
    c[ImGuiCol_TabSelected] = ImVec4(0.55f, 0.30f, 0.85f, 1.0f);
    c[ImGuiCol_TabSelectedOverline] = ImVec4(1.0f, 0.82f, 0.35f, 1.0f);
    c[ImGuiCol_ChildBg] = ImVec4(0, 0, 0, 0);
    c[ImGuiCol_TableRowBgAlt] = ImVec4(1.0f, 1.0f, 1.0f, 0.04f);
}

// A labelled row: text on the left, widget in a fixed right-hand column.
void row_label(const char *label) {
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::SameLine(ImGui::GetFontSize() * 13.0f);
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
}

void help(const char *text) {
    ImGui::SameLine();
    ImGui::TextDisabled("(?)");
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

// Combo over a list of integer choices; true when the value changed.
bool int_combo(const char *id, int &value, const std::vector<int> &choices,
               const std::vector<std::string> &labels) {
    int cur = 0;
    for (size_t i = 0; i < choices.size(); ++i)
        if (choices[i] == value) cur = (int)i;
    bool changed_ = false;
    if (ImGui::BeginCombo(id, labels[cur].c_str())) {
        for (size_t i = 0; i < choices.size(); ++i) {
            const bool sel = (int)i == cur;
            if (ImGui::Selectable(labels[i].c_str(), sel) && !sel) {
                value = choices[i];
                changed_ = true;
            }
            if (sel) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed_;
}

void graphics_tab() {
    Config &cfg = Config::get();

    row_label("Show FPS counter");
    if (ImGui::Checkbox("##fps", &cfg.show_fps)) changed();
    help("Game frames per second, top-left. Also toggled with F2.");
    {
        std::vector<int> v{60, 90, 120};
        std::vector<std::string> l{"60  (original)", "90  (smoother than 120 in heavy scenes)",
                                   "120  (experimental)"};
        row_label("Frame rate");
        if (int_combo("##framerate", cfg.frame_rate, v, l)) changed();
        help("Maximum game frame rate. The game runs on real time, so its speed "
             "is the same either way. Above 60 the frame time follows the scene "
             "load; 90 stays even in more places than 120. Needs a high-refresh "
             "display with VSync, or VSync off.");
    }
    ImGui::Spacing();
    {
        std::vector<int> v;
        std::vector<std::string> l;
        for (int s = 1; s <= 6; ++s) {
            const int w = (int)std::lround(640 * s * cfg.aspect_factor());
            char buf[64];
            std::snprintf(buf, sizeof buf, "%dx  (%dx%d)%s", s, w, 480 * s,
                          s == 1 ? "  native" : "");
            v.push_back(s);
            l.push_back(buf);
        }
        row_label("Internal resolution");
        if (int_combo("##scale", cfg.internal_scale, v, l)) {
            s_req.rebuild_efb = true;
            changed();
        }
        help("Renders the game at a multiple of the GameCube's 640x480. Higher is "
             "sharper but heavier on the GPU.");
    }
    {
        std::vector<int> v{1};
        std::vector<std::string> l{"Off"};
        for (int s = 2; s <= s_max_msaa && s <= 8; s *= 2) {
            v.push_back(s);
            l.push_back(std::to_string(s) + "x MSAA");
        }
        cfg.msaa = std::min(cfg.msaa, v.back());
        row_label("Anti-aliasing");
        if (int_combo("##msaa", cfg.msaa, v, l)) {
            s_req.rebuild_efb = true;
            changed();
        }
        help("Smooths jagged polygon edges.");
    }
    {
        std::vector<int> v{1};
        std::vector<std::string> l{"Off"};
        for (int s = 2; s <= s_max_aniso && s <= 16; s *= 2) {
            v.push_back(s);
            l.push_back(std::to_string(s) + "x");
        }
        row_label("Texture filtering");
        if (int_combo("##aniso", cfg.anisotropy, v, l)) changed();
        help("Anisotropic filtering: keeps ground textures sharp at shallow angles.");
    }

    ImGui::Spacing();
    {
        // w * 100 + h as the combo value.
        const std::vector<int> v = {403, 1610, 2109, 3209};
        const std::vector<std::string> l = {"Game Aspect Ratio", "16:10", "21:9", "32:9"};
        int code = cfg.aspect_w * 100 + cfg.aspect_h;
        row_label("Aspect ratio");
        if (int_combo("##aspect", code, v, l)) {
            cfg.set_aspect(code / 100, code % 100);
            s_req.rebuild_efb = true;
            changed();
        }
        help("In-game: the game's own Widescreen option decides (4:3 or 16:9). The others "
             "widen the field of view; the HUD keeps its shape.");
    }
    row_label("Fullscreen");
    if (ImGui::Checkbox("##fs", &cfg.fullscreen)) {
        s_req.fullscreen = true;
        changed();
    }
    help("Borderless fullscreen. Also F11 or Alt+Enter.");
    row_label("VSync");
    if (ImGui::Checkbox("##vsync", &cfg.enable_vsync)) {
        s_req.vsync = true;
        changed();
    }
    help("Syncs presentation to the display; turning it off removes waiting but "
         "can tear and uses more CPU/GPU. The game's own speed is unaffected.");
}

void audio_tab() {
    Config &cfg = Config::get();
    row_label("Master volume");
    if (ImGui::SliderInt("##vol", &cfg.volume, 0, 100, "%d%%")) {}
    if (ImGui::IsItemDeactivatedAfterEdit()) changed();
    row_label("Mute");
    if (ImGui::Checkbox("##mute", &cfg.mute)) changed();
}

void keyboard_bindings() {
    using namespace nwii::runtime::input;
    ImGui::TextWrapped("Keyboard and mouse controls for the GameCube pad. Click a control, then "
                       "press the new key or mouse button (Esc cancels).");
    ImGui::Spacing();
    KeyBinding *t = key_bindings();
    const float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.4f;
    if (ImGui::BeginTable("keys", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerH,
                          ImVec2(0, std::max(h, ImGui::GetFontSize() * 8)))) {
        ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (int i = 0; i < kNumPadControls; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(t[i].label);
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(i);
            const bool cap = s_capturing == i;
            std::string text = cap ? std::string("Press a key or mouse button...") : key_name(t[i].scancode);
            if (cap) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.10f, 1.0f));
            if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0)))
                s_capturing = cap ? -1 : i;
            if (cap) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset to defaults")) {
        reset_key_bindings();
        Config::get().apply_default_keyboard(); // the game's own defaults on top
        s_capturing = -1;
        changed();
        note("Keyboard controls reset to defaults");
    }
}

// Every controller input (buttons and the two triggers) as a bit, for the
// capture below.
uint64_t pad_inputs_down() {
    using namespace nwii::runtime::input;
    uint64_t down = 0;
    for (int j = 0; j < SDL_NumJoysticks(); ++j) {
        if (!SDL_IsGameController(j)) continue;
        SDL_GameController *c = SDL_GameControllerOpen(j);
        if (!c) continue;
        for (int b = 0; b < SDL_CONTROLLER_BUTTON_MAX; ++b)
            if (pad_input_pressed(c, b)) down |= 1ull << b;
        for (int a : {SDL_CONTROLLER_AXIS_TRIGGERLEFT, SDL_CONTROLLER_AXIS_TRIGGERRIGHT})
            if (pad_input_pressed(c, kPadAxisBase + a)) down |= 1ull << (40 + a);
    }
    return down;
}

int pad_input_from_bit(int bit) {
    return bit >= 40 ? nwii::runtime::input::kPadAxisBase + (bit - 40) : bit;
}

void controller_bindings() {
    using namespace nwii::runtime::input;
    ImGui::TextWrapped("Controller buttons for the GameCube pad (sticks: left = "
                       "move, right = camera). Click a control, then press the "
                       "controller button or trigger (Esc cancels).");
    ImGui::Spacing();
    PadBinding *t = pad_bindings();
    // Capture: the first input that goes down after the click. Inputs already
    // held then (e.g. the button used to click) are ignored until released.
    if (s_pad_capturing >= 0) {
        const uint64_t down = pad_inputs_down();
        const uint64_t fresh = down & ~s_pad_held;
        s_pad_held &= down;
        if (fresh) {
            int bit = 0;
            while (!(fresh & (1ull << bit))) ++bit;
            const int input = pad_input_from_bit(bit);
            // Swap with a control that already used this input.
            for (int i = 0; i < kNumPadButtonBindings; ++i)
                if (i != s_pad_capturing && t[i].input == input)
                    t[i].input = t[s_pad_capturing].input;
            t[s_pad_capturing].input = input;
            s_pad_capturing = -1;
            changed();
        }
    }
    const float h = ImGui::GetContentRegionAvail().y - ImGui::GetFrameHeightWithSpacing() * 1.4f;
    if (ImGui::BeginTable("pad", 2,
                          ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY |
                              ImGuiTableFlags_BordersInnerH,
                          ImVec2(0, std::max(h, ImGui::GetFontSize() * 8)))) {
        ImGui::TableSetupColumn("Control", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        ImGui::TableSetupColumn("Input", ImGuiTableColumnFlags_WidthStretch, 1.0f);
        for (int i = 0; i < kNumPadButtonBindings; ++i) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(t[i].label);
            ImGui::TableSetColumnIndex(1);
            ImGui::PushID(1000 + i);
            const bool cap = s_pad_capturing == i;
            std::string text = cap ? std::string("Press a button...") : pad_input_name(t[i].input);
            if (cap) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.85f, 0.55f, 0.10f, 1.0f));
            if (ImGui::Button(text.c_str(), ImVec2(-FLT_MIN, 0))) {
                s_pad_capturing = cap ? -1 : i;
                s_pad_held = pad_inputs_down();
            }
            if (cap) ImGui::PopStyleColor();
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    if (ImGui::Button("Reset to defaults")) {
        reset_pad_bindings();
        s_pad_capturing = -1;
        changed();
        note("Controller buttons reset to defaults");
    }
}

void mouse_settings() {
    Config &cfg = Config::get();
    ImGui::TextWrapped("Turn the camera left/right with the mouse (it drives the C-stick); while Y is "
                       "held (look mode) it aims up/down too. Click into the game to capture the "
                       "mouse; Esc or F1 releases it.");
    ImGui::Spacing();
    row_label("Mouse camera");
    if (ImGui::Checkbox("##mousecam", &cfg.mouse_camera)) changed();
    row_label("Sensitivity");
    ImGui::SliderInt("##mousesens", &cfg.mouse_sensitivity, 1, 100);
    if (ImGui::IsItemDeactivatedAfterEdit()) changed();
    row_label("Invert up/down");
    if (ImGui::Checkbox("##mouseinv", &cfg.mouse_invert_y)) changed();
}

void controls_tab() {
    if (ImGui::BeginTabBar("##controls")) {
        if (ImGui::BeginTabItem("Keyboard")) {
            keyboard_bindings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Mouse")) {
            mouse_settings();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Controller")) {
            controller_bindings();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

void fps_counter() {
    static double t0 = now_s();
    static uint64_t f0 = g_stat_frames;
    static float fps = 0;
    const double t = now_s();
    if (t - t0 >= 0.5) {
        fps = (float)((g_stat_frames - f0) / (t - t0));
        f0 = g_stat_frames;
        t0 = t;
    }
    ImGui::SetNextWindowPos(ImVec2(10, 10));
    ImGui::SetNextWindowBgAlpha(0.45f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoSavedSettings);
    ImGui::Text("%.0f FPS", fps);
    ImGui::End();
}

void settings_window() {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float fs = ImGui::GetFontSize();
    // Shifted down a little: the "PAUSED" banner sits above the panel.
    ImGui::SetNextWindowPos(ImVec2(vp->GetCenter().x, vp->GetCenter().y + fs * 1.4f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(fs * 30.0f, vp->Size.x * 0.95f),
                                    std::min(fs * 27.5f, vp->Size.y * 0.80f)),
                             ImGuiCond_Always);
    ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0, 0, 0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
    ImGui::Begin("Settings", nullptr,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoTitleBar |
                     ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings);
    ImGui::PopStyleVar();
    ImGui::PopStyleColor();
    draw_menu_frame(ImGui::GetWindowPos(), ImGui::GetWindowSize(), s_launcher ? "SETTINGS" : "PAUSED");
    // Room under the banner, then the section title in gold.
    ImGui::Dummy(ImVec2(0, fs * 0.9f));
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.86f, 0.45f, 1.0f));
    ImGui::TextUnformatted("Settings");
    ImGui::PopStyleColor();
    const float footer = ImGui::GetFrameHeightWithSpacing() * 1.6f;
    ImGui::BeginChild("tabs", ImVec2(0, -footer));
    // NWII_UI_TAB=<0..2>: start on that tab (tests).
    static int force_tab = [] { const char *e = std::getenv("NWII_UI_TAB"); return e ? std::atoi(e) : -1; }();
    if (s_open_tab >= 0) force_tab = s_open_tab;
    s_open_tab = -1;
    auto tab_flags = [](int i) {
        return force_tab == i ? ImGuiTabItemFlags_SetSelected : ImGuiTabItemFlags_None;
    };
    if (ImGui::BeginTabBar("tabs")) {
        if (ImGui::BeginTabItem("Graphics", nullptr, tab_flags(0))) { graphics_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Audio", nullptr, tab_flags(1))) { audio_tab(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Controls", nullptr, tab_flags(2))) { controls_tab(); ImGui::EndTabItem(); }
        force_tab = -1;
        ImGui::EndTabBar();
    }
    ImGui::EndChild();

    ImGui::Separator();
    if (now_s() < s_status_until)
        ImGui::TextDisabled("%s", s_status.c_str());
    else
        ImGui::TextDisabled(s_launcher ? "Esc / B: back  -  auto-saved"
                                       : "F1 / Back: close  -  auto-saved");
    const float bw = fs * 6.5f;
    const ImGuiStyle &st = ImGui::GetStyle();
    if (s_launcher) {
        ImGui::SameLine(ImGui::GetWindowWidth() - st.WindowPadding.x - bw);
        if (ImGui::Button("Back", ImVec2(bw, 0))) set_open(false);
    } else {
        ImGui::SameLine(ImGui::GetWindowWidth() - st.WindowPadding.x - bw * 2 - st.ItemSpacing.x);
        if (ImGui::Button("Quit game", ImVec2(bw, 0))) s_req.quit = true;
        ImGui::SameLine();
        if (ImGui::Button("Resume", ImVec2(bw, 0))) set_open(false);
    }
    ImGui::End();
}

} // namespace

// ---------------------------------------------------------------------------
void init(SDL_Window *window, void *gl_context, int max_msaa, int max_anisotropy) {
    s_window = window;
    s_max_msaa = std::max(1, max_msaa);
    s_max_aniso = std::max(1, max_anisotropy);
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO &io = ImGui::GetIO();
    io.IniFilename = nullptr; // layout is fixed; nothing to persist
    apply_style();
    load_font();
    ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
    ImGui_ImplOpenGL3_Init("#version 150");
    s_ready = true;
    set_open(false);
}

void shutdown() {
    if (!s_ready) return;
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplSDL2_Shutdown();
    ImGui::DestroyContext();
    s_ready = false;
}

bool is_open() { return s_open; }

void open_settings(int tab) {
    s_open_tab = tab;
    set_open(true);
}

void set_launcher_mode(bool on) {
    s_launcher = on;
    set_open(false);
}

Requests take_requests() {
    Requests r = s_req;
    s_req = {};
    return r;
}

bool handle_event(const SDL_Event &e) {
    if (!s_ready) return false;
    Config &cfg = Config::get();

    // Rebinding: the next key press becomes the binding (Esc cancels). A key
    // already used by another control is swapped onto it, so nothing is lost.
    if (s_open && s_pad_capturing >= 0 && e.type == SDL_KEYDOWN &&
        e.key.keysym.scancode == SDL_SCANCODE_ESCAPE) {
        s_pad_capturing = -1; // cancel controller capture
        return true;
    }
    // A mouse button binds the same way (the click that started the capture
    // has already been released: ImGui buttons fire on release).
    if (s_open && s_capturing >= 0 &&
        ((e.type == SDL_KEYDOWN && !e.key.repeat) || e.type == SDL_MOUSEBUTTONDOWN)) {
        const int sc = e.type == SDL_MOUSEBUTTONDOWN ? input::kMouseButtonBase + e.button.button
                                                     : (int)e.key.keysym.scancode;
        if (sc != SDL_SCANCODE_ESCAPE && sc != SDL_SCANCODE_F1 &&
            (e.type != SDL_MOUSEBUTTONDOWN || input::is_mouse_binding(sc))) {
            input::KeyBinding *t = input::key_bindings();
            for (int i = 0; i < input::kNumPadControls; ++i)
                if (i != s_capturing && t[i].scancode == sc)
                    t[i].scancode = t[s_capturing].scancode;
            t[s_capturing].scancode = sc;
            changed();
        }
        s_capturing = -1;
        return true;
    }

    if (e.type == SDL_KEYDOWN && !e.key.repeat) {
        const SDL_Keycode k = e.key.keysym.sym;
        if (k == SDLK_F1 || (k == SDLK_ESCAPE && s_open)) {
            set_open(k == SDLK_F1 ? !s_open : false);
            return true;
        }
        if (k == SDLK_F2) {
            cfg.show_fps = !cfg.show_fps;
            changed();
            return true;
        }
        if (k == SDLK_F11 || (k == SDLK_RETURN && (e.key.keysym.mod & KMOD_ALT))) {
            cfg.fullscreen = !cfg.fullscreen;
            s_req.fullscreen = true;
            changed();
            return true;
        }
    }
    if (e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_BACK) {
        set_open(!s_open);
        return true;
    }
    // Gamepad B closes the menu, like a console pause screen.
    if (s_open && e.type == SDL_CONTROLLERBUTTONDOWN && e.cbutton.button == SDL_CONTROLLER_BUTTON_B &&
        !ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopup)) {
        set_open(false);
        return true;
    }

    ImGui_ImplSDL2_ProcessEvent(&e);
    if (!s_open) return false;
    return e.type == SDL_KEYDOWN || e.type == SDL_KEYUP || e.type == SDL_TEXTINPUT ||
           e.type == SDL_MOUSEBUTTONDOWN || e.type == SDL_MOUSEBUTTONUP ||
           e.type == SDL_MOUSEWHEEL || e.type == SDL_CONTROLLERBUTTONDOWN ||
           e.type == SDL_CONTROLLERBUTTONUP;
}

void frame(void (*underlay)()) {
    if (!s_ready) return;
    {
        // Cursor: the OS cursor doesn't reliably show over a borderless
        // fullscreen GL window, so in fullscreen the menu draws its own and
        // gameplay hides it; windowed keeps the normal OS cursor.
        ImGuiIO &io = ImGui::GetIO();
        const bool fs = Config::get().fullscreen;
        const bool menu = s_open || s_launcher;
        io.MouseDrawCursor = menu && fs;
        if (menu && !fs) {
            io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
        } else {
            io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
            SDL_ShowCursor(fs ? SDL_DISABLE : SDL_ENABLE);
        }
    }
    // NWII_UI_OPEN=<present>: open the menu at that present (and save the
    // settings once, exercising the TOML writer) -- for scripted tests.
    static int open_at = [] { const char *e = std::getenv("NWII_UI_OPEN"); return e ? std::atoi(e) : -1; }();
    static int presents = 0;
    if (++presents == open_at) {
        set_open(true);
        if (!save_settings()) std::cout << "[UI] save failed" << std::endl;
        else std::cout << "[UI] settings saved to " << Config::get().config_path << std::endl;
    }
    // NWII_UI_SWITCH=<scale>,<msaa>,<widescreen 0/1>: 40 presents after the
    // menu opened, apply those graphics settings live and close the menu.
    if (const char *sw = std::getenv("NWII_UI_SWITCH"); sw && presents == open_at + 40) {
        int sc = 1, ms = 1, ws = 0;
        if (std::sscanf(sw, "%d,%d,%d", &sc, &ms, &ws) == 3) {
            Config &c = Config::get();
            c.internal_scale = sc;
            c.msaa = ms;
            c.set_aspect(ws ? 16 : 4, ws ? 9 : 3);
            s_req.rebuild_efb = true;
            set_open(false);
            std::cout << "[UI] live switch to " << sc << "x, msaa " << ms << ", ws " << ws << std::endl;
        }
    }
    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();
    if (Config::get().show_fps && !s_launcher) fps_counter();
    if (underlay) underlay();
    if (s_open) {
        // Dim the game behind the menu.
        ImGui::GetBackgroundDrawList()->AddRectFilled(
            ImVec2(0, 0), ImGui::GetMainViewport()->Size, IM_COL32(0, 0, 0, 110));
        settings_window();
    }
    ImGui::Render();
    ImDrawData *dd = ImGui::GetDrawData();
    if (dd->CmdListsCount == 0) return;
    int w = 0, h = 0;
    SDL_GL_GetDrawableSize(s_window, &w, &h);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, w, h);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    ImGui_ImplOpenGL3_RenderDrawData(dd);

    // NWII_UI_SHOT=<file.bmp>: save the window (game + overlay) once, 20
    // presents after the menu opened via NWII_UI_OPEN.
    if (const char *shot = std::getenv("NWII_UI_SHOT"); shot && (presents == open_at + 20 || presents == open_at + 400)) {
        std::vector<unsigned char> px((size_t)w * h * 4), flip(px.size());
        glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        for (int y = 0; y < h; ++y)
            std::memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
        for (size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
        if (SDL_Surface *sf = SDL_CreateRGBSurfaceFrom(flip.data(), w, h, 32, w * 4, 0xFF,
                                                       0xFF00, 0xFF0000, 0xFF000000)) {
            SDL_SaveBMP(sf, (std::string(shot) + (presents == open_at + 20 ? "" : ".after.bmp")).c_str());
            SDL_FreeSurface(sf);
        }
    }
}

bool save_settings() {
    const Config &cfg = Config::get();
    if (cfg.config_path.empty()) return false;
    std::vector<std::string> lines;
    bool crlf = false;
    {
        std::ifstream in(cfg.config_path, std::ios::binary);
        std::string line;
        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') { line.pop_back(); crlf = true; }
            lines.push_back(line);
        }
    }
    set_value(lines, "graphics", "internal_scale", std::to_string(cfg.internal_scale));
    set_value(lines, "graphics", "msaa", std::to_string(cfg.msaa));
    set_value(lines, "graphics", "anisotropy", std::to_string(cfg.anisotropy));
    set_value(lines, "graphics", "aspect_ratio",
              cfg.widescreen ? "\"" + std::to_string(cfg.aspect_w) + ":" +
                                   std::to_string(cfg.aspect_h) + "\""
                             : std::string("\"game\""));
    set_value(lines, "graphics", "fullscreen", b2s(cfg.fullscreen));
    set_value(lines, "graphics", "vsync", b2s(cfg.enable_vsync));
    set_value(lines, "graphics", "show_fps", b2s(cfg.show_fps));
    set_value(lines, "graphics", "frame_rate", std::to_string(cfg.frame_rate));
    set_value(lines, "audio", "volume", std::to_string(cfg.volume));
    set_value(lines, "audio", "mute", b2s(cfg.mute));
    set_value(lines, "mouse", "camera", b2s(cfg.mouse_camera));
    set_value(lines, "mouse", "sensitivity", std::to_string(cfg.mouse_sensitivity));
    set_value(lines, "mouse", "invert_y", b2s(cfg.mouse_invert_y));
    set_value(lines, "launcher", "enabled", b2s(cfg.launcher));
    {
        const std::u8string u8 = std::filesystem::path(cfg.memcard_path).u8string();
        set_value(lines, "launcher", "memcard", toml_string(std::string(u8.begin(), u8.end())));
    }
    {
        std::string arr = "[";
        for (size_t i = 0; i < cfg.recent_discs.size(); ++i)
        {
            const std::u8string u8 = std::filesystem::path(cfg.recent_discs[i]).u8string();
            arr += (i ? ", " : "") + toml_string(std::string(u8.begin(), u8.end()));
        }
        set_value(lines, "launcher", "recent", arr + "]");
    }
    const input::KeyBinding *t = input::key_bindings();
    for (int i = 0; i < input::kNumPadControls; ++i)
        set_value(lines, "keyboard", t[i].id,
                  std::string("\"") + key_name(t[i].scancode) + "\"");
    const input::PadBinding *pb = input::pad_bindings();
    for (int i = 0; i < input::kNumPadButtonBindings; ++i)
        set_value(lines, "controller", pb[i].id,
                  std::string("\"") + input::pad_input_name(pb[i].input) + "\"");

    // Write to a temp file, then replace, so a crash never truncates it.
    const std::string tmp = cfg.config_path + ".tmp";
    {
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        for (const std::string &l : lines) out << l << (crlf ? "\r\n" : "\n");
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp, cfg.config_path, ec);
    if (ec) {
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}

} // namespace nwii::runtime::ui
