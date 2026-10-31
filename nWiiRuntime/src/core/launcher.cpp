#include "runtime/launcher.h"
#include "runtime/config.h"
#include "runtime/disc_image.h"
#include "runtime/efb.h"
#include "runtime/hw/hw.h"
#include "runtime/settings_overlay.h"
#include "ui_theme.h"

#include <glad/glad.h>
#include <SDL.h>
#include <SDL_syswm.h>
#include <imgui.h>
#include <imgui_internal.h> // ShadeVertsLinearColorGradientKeepAlpha

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shobjidl.h>
#pragma comment(lib, "ole32.lib")
#endif

namespace fs = std::filesystem;

namespace nwii::runtime::launcher {

// ---------------------------------------------------------------------------
// Disc probing

namespace {

uint32_t be32(const unsigned char *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

// boot.bin's ID and name.
void read_header(DiscInfo &d, const unsigned char *boot) {
    d.id.assign(reinterpret_cast<const char *>(boot), 6);
    d.title.assign(reinterpret_cast<const char *>(boot + 0x20), strnlen(reinterpret_cast<const char *>(boot + 0x20), 0x60));
}

} // namespace

DiscInfo probe(const std::string &path, const std::string &expected_id) {
    DiscInfo d;
    if (path.empty()) return d;
    std::error_code ec;
    const fs::path p(path);
    unsigned char boot[0x440] = {};
    if (fs::is_directory(p, ec)) {
        fs::path b = p / "sys" / "boot.bin";
        if (!fs::exists(b, ec)) b = p / "boot.bin";
        std::ifstream f(b, std::ios::binary);
        if (!f.read(reinterpret_cast<char *>(boot), sizeof(boot)) ||
            !fs::exists(b.parent_path() / "main.dol", ec)) {
            d.kind = DiscInfo::Unknown;
            d.error = "This folder is not an extracted GameCube disc (no sys/boot.bin).";
            return d;
        }
        d.kind = DiscInfo::Extracted;
    } else if (fs::is_regular_file(p, ec)) {
        // .iso/.gcm or RVZ; other formats come back with the reason.
        auto img = DiscImage::open(path, &d.error);
        if (!img || !img->read(0, sizeof(boot), boot)) {
            d.kind = DiscInfo::Unknown;
            if (d.error.empty()) d.error = "The disc image can't be read.";
            return d;
        }
        d.kind = DiscInfo::Image;
    } else {
        d.kind = DiscInfo::Unknown;
        d.error = "The disc was not found: " + path;
        return d;
    }
    read_header(d, boot);
    if (!expected_id.empty() && d.id != expected_id)
        d.error = "This build runs game " + expected_id + ", but that disc is " + d.id + " (" +
                  d.title + ").";
    return d;
}

// ---------------------------------------------------------------------------
// Menu

namespace {

using namespace ui::theme;

enum class Screen { Main, Recent, Files, Credits };
enum Pick { kPickNone, kPickDisc, kPickCard, kPickBios };

struct State {
    SDL_Window *window = nullptr;
    std::string expected_id;
    std::string disc; // currently selected disc
    DiscInfo info;
    std::string message; // last error, shown in the footer
    std::string hint;    // footer text for the highlighted entry (this frame)
    Screen screen = Screen::Main;
    int pick = kPickNone; // file dialog to open after this frame
    bool done = false, quit = false;
    // Selection, driven by keys / gamepad / mouse (not ImGui's nav, so one
    // button is always highlighted, like a console menu).
    int sel = 0;           // highlighted button on the current screen
    int move = 0;          // pending Up (-1) / Down (+1)
    bool activate = false; // pending Enter / A
    bool mouse_moved = false; // real mouse motion this frame (not the start-up position)
    int index = 0;         // button counter while drawing
    std::vector<bool> enabled; // per button, from the last frame
    Screen shown = Screen::Main; // screen drawn last frame
    double t0 = 0;
};
State S;

double now_s() {
    using namespace std::chrono;
    return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool playable(const DiscInfo &d) {
    return (d.kind == DiscInfo::Image || d.kind == DiscInfo::Extracted) && d.error.empty();
}

std::string file_name(const std::string &path) {
    const fs::path p(path);
    return p.has_filename() ? p.filename().string() : p.parent_path().filename().string();
}

void remember(const std::string &disc) {
    auto &r = Config::get().recent_discs;
    r.erase(std::remove(r.begin(), r.end(), disc), r.end());
    r.insert(r.begin(), disc);
    if (r.size() > 8) r.resize(8);
    ui::save_settings();
}

// Select a disc; boots it when it is the right game.
void choose(const std::string &path) {
    DiscInfo d = probe(path, S.expected_id);
    if (!playable(d)) {
        S.message = d.error;
        return;
    }
    S.disc = path;
    S.info = d;
    remember(path);
    S.done = true;
}

#ifdef _WIN32
// Windows file dialog.
std::string pick_file(const wchar_t *title, const COMDLG_FILTERSPEC *types, UINT count) {
    std::string result;
    HWND owner = nullptr;
    SDL_SysWMinfo wm;
    SDL_VERSION(&wm.version);
    if (SDL_GetWindowWMInfo(S.window, &wm)) owner = wm.info.win.window;
    const HRESULT co = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    IFileOpenDialog *dlg = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_ALL, IID_PPV_ARGS(&dlg)))) {
        dlg->SetFileTypes(count, types);
        dlg->SetTitle(title);
        if (SUCCEEDED(dlg->Show(owner))) {
            IShellItem *item = nullptr;
            if (SUCCEEDED(dlg->GetResult(&item))) {
                PWSTR wpath = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &wpath))) {
                    try {
                        result = fs::path(wpath).string();
                    } catch (const std::exception &) {
                        S.message = "That path has characters this build can't open; "
                                    "move the file to a folder with a plain name.";
                    }
                    CoTaskMemFree(wpath);
                }
                item->Release();
            }
        }
        dlg->Release();
    }
    if (SUCCEEDED(co)) CoUninitialize();
    return result;
}

// An extracted disc can be opened through its sys/boot.bin or sys/main.dol.
std::string pick_disc() {
    const COMDLG_FILTERSPEC types[] = {
        {L"GameCube disc (*.iso, *.gcm, *.rvz, extracted boot.bin / main.dol)",
         L"*.iso;*.gcm;*.rvz;boot.bin;main.dol"},
        {L"Other disc formats (*.wia, *.gcz, *.ciso) - convert first", L"*.wia;*.gcz;*.ciso"},
        {L"All files", L"*.*"},
    };
    const std::string path = pick_file(L"Open Spyro: Enter the Dragonfly disc", types, 3);
    if (path.empty()) return {};
    fs::path p(path);
    const auto name = p.filename().wstring();
    if (name == L"boot.bin" || name == L"main.dol") {
        p = p.parent_path();
        if (p.filename() == L"sys") p = p.parent_path();
    }
    return p.string();
}

std::string pick_card() {
    const COMDLG_FILTERSPEC types[] = {{L"GameCube memory card (*.raw)", L"*.raw"}, {L"All files", L"*.*"}};
    return pick_file(L"Choose a memory card", types, 2);
}

std::string pick_bios() {
    const COMDLG_FILTERSPEC types[] = {{L"GameCube BIOS (*.bin)", L"*.bin"}, {L"All files", L"*.*"}};
    return pick_file(L"Choose a GameCube BIOS (ipl.bin)", types, 2);
}
#else
std::string pick_disc() {
    S.message = "Pass the disc path on the command line.";
    return {};
}
std::string pick_card() {
    S.message = "Set [launcher] memcard in spyro.toml.";
    return {};
}
std::string pick_bios() {
    S.message = "Set [launcher] bios in spyro.toml.";
    return {};
}
#endif

// Use a card or BIOS the file dialog returned, if it is one.
void choose_file(std::string &setting, const std::string &path, const std::string &error) {
    if (!error.empty()) {
        S.message = error;
        return;
    }
    setting = path;
    ui::save_settings();
}

// "Memory Card: name" / "BIOS: name" button text; "Default" when none is set.
std::string file_label(const char *what, const std::string &path) {
    std::error_code ec;
    return std::string(what) + ": " +
           (path.empty() ? "Default" : file_name(path) + (fs::exists(path, ec) ? "" : "  (missing)"));
}

// --- Background: a sunset over hills, drawn procedurally --------------------
void draw_pagoda(ImDrawList *dl, float x, float base, float s, ImU32 col) {
    float y = base;
    for (int i = 0; i < 4; ++i) {
        const float w = s * (0.55f - 0.1f * i), body = s * 0.22f, roof = s * 0.12f;
        dl->AddRectFilled(ImVec2(x - w * 0.6f, y - body), ImVec2(x + w * 0.6f, y), col);
        y -= body;
        // Roof with upturned eaves.
        dl->AddQuadFilled(ImVec2(x - w * 1.25f, y - roof * 0.15f), ImVec2(x - w * 0.55f, y - roof),
                          ImVec2(x + w * 0.55f, y - roof), ImVec2(x + w * 1.25f, y - roof * 0.15f), col);
        dl->AddTriangleFilled(ImVec2(x - w * 1.25f, y - roof * 0.15f), ImVec2(x - w * 1.38f, y - roof * 0.55f),
                              ImVec2(x - w * 1.0f, y - roof * 0.3f), col);
        dl->AddTriangleFilled(ImVec2(x + w * 1.25f, y - roof * 0.15f), ImVec2(x + w * 1.38f, y - roof * 0.55f),
                              ImVec2(x + w * 1.0f, y - roof * 0.3f), col);
        y -= roof;
    }
    dl->AddRectFilled(ImVec2(x - s * 0.015f, y - s * 0.25f), ImVec2(x + s * 0.015f, y), col);
}

void draw_scene(ImDrawList *dl, ImVec2 sz, float t) {
    const float W = sz.x, H = sz.y, hz = H * 0.64f;
    // Sky bands.
    struct Stop { float y; ImU32 c; };
    const Stop sky[] = {{0.0f, IM_COL32(30, 16, 60, 255)},   {0.28f, IM_COL32(88, 38, 112, 255)},
                        {0.46f, IM_COL32(188, 80, 112, 255)}, {0.57f, IM_COL32(242, 136, 96, 255)},
                        {0.64f, IM_COL32(255, 192, 124, 255)}};
    dl->AddRectFilled(ImVec2(0, 0), sz, IM_COL32(112, 52, 112, 255));
    for (int i = 0; i + 1 < (int)IM_ARRAYSIZE(sky); ++i)
        dl->AddRectFilledMultiColor(ImVec2(0, H * sky[i].y), ImVec2(W, H * sky[i + 1].y + 1), sky[i].c,
                                    sky[i].c, sky[i + 1].c, sky[i + 1].c);
    // Stars, twinkling in the dark upper sky.
    for (int i = 0; i < 26; ++i) {
        const float x = std::fmod(i * 0.3819f * 7.3f, 1.0f) * W;
        const float y = std::fmod(i * 0.618f * 3.1f, 1.0f) * H * 0.3f;
        const float a = 0.5f + 0.5f * std::sin(t * (1.3f + (i % 5) * 0.4f) + i);
        draw_sparkle(dl, ImVec2(x, y), H * (0.004f + 0.003f * (i % 3)) * (0.6f + 0.4f * a),
                     IM_COL32(255, 240, 255, (int)(60 + 150 * a)));
    }
    // Sun with a glow.
    const ImVec2 sun(W * 0.80f, hz - H * 0.035f);
    const float sr = H * 0.085f;
    ImVec2 ring[48];
    auto circle = [&](float r) {
        for (int i = 0; i < 48; ++i) {
            const float a = i * (6.2831853f / 48);
            ring[i] = ImVec2(sun.x + std::cos(a) * r, sun.y + std::sin(a) * r);
        }
    };
    circle(sr * 3.4f);
    fan_gradient(dl, sun, IM_COL32(255, 200, 120, 130), ring, 48, IM_COL32(255, 160, 110, 0));
    circle(sr);
    fan_gradient(dl, sun, IM_COL32(255, 246, 214, 255), ring, 48, IM_COL32(255, 196, 110, 255));
    // Clouds drifting slowly.
    for (int i = 0; i < 7; ++i) {
        const float x = std::fmod(i * 0.29f + t * 0.004f * (1 + i % 3), 1.3f) * W * 1.1f - W * 0.15f;
        const float y = H * (0.12f + 0.06f * (i % 4)) + (i % 2) * H * 0.05f;
        const ImU32 c = i % 2 ? IM_COL32(255, 150, 170, 45) : IM_COL32(120, 52, 120, 90);
        dl->AddEllipseFilled(ImVec2(x, y), ImVec2(W * (0.14f + 0.03f * (i % 3)), H * 0.022f), c);
        dl->AddEllipseFilled(ImVec2(x + W * 0.05f, y - H * 0.012f), ImVec2(W * 0.07f, H * 0.018f), c);
    }
    // Far ridge with pagodas, then the green hills in front.
    const float step = std::max(2.0f, W / 480.0f);
    for (float x = 0; x < W; x += step) {
        const float y = hz - H * (0.05f + 0.035f * std::sin(x / W * 9.0f + 1.0f) +
                                  0.02f * std::sin(x / W * 23.0f));
        dl->AddRectFilled(ImVec2(x, y), ImVec2(x + step + 1, H * 0.75f), IM_COL32(112, 52, 112, 255));
    }
    draw_pagoda(dl, W * 0.88f, hz - H * 0.055f, H * 0.16f, IM_COL32(86, 38, 96, 255));
    draw_pagoda(dl, W * 0.95f, hz - H * 0.05f, H * 0.11f, IM_COL32(86, 38, 96, 255));
    draw_pagoda(dl, W * 0.09f, hz - H * 0.06f, H * 0.09f, IM_COL32(86, 38, 96, 255));
    for (float x = 0; x < W; x += step) {
        const float y = hz + H * (0.01f + 0.03f * std::sin(x / W * 4.0f + 2.0f) +
                                  0.012f * std::sin(x / W * 15.0f));
        dl->AddRectFilledMultiColor(ImVec2(x, y), ImVec2(x + step + 1, H), IM_COL32(92, 118, 62, 255),
                                    IM_COL32(92, 118, 62, 255), IM_COL32(34, 46, 30, 255),
                                    IM_COL32(34, 46, 30, 255));
    }
    // Vignette.
    const ImU32 clear = IM_COL32(10, 4, 20, 0), dark = IM_COL32(10, 4, 20, 150);
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(W * 0.18f, H), dark, clear, clear, dark);
    dl->AddRectFilledMultiColor(ImVec2(W * 0.82f, 0), ImVec2(W, H), clear, dark, dark, clear);
    dl->AddRectFilledMultiColor(ImVec2(0, H * 0.8f), ImVec2(W, H), clear, clear, dark, dark);
}

// A glowing dragonfly, bobbing and flapping.
void draw_dragonfly(ImDrawList *dl, ImVec2 c, float s, float t) {
    c.y += std::sin(t * 2.1f) * s * 0.25f;
    c.x += std::sin(t * 1.3f) * s * 0.15f;
    ImVec2 ring[32];
    for (int i = 0; i < 32; ++i) {
        const float a = i * (6.2831853f / 32);
        ring[i] = ImVec2(c.x + std::cos(a) * s * 2.2f, c.y + std::sin(a) * s * 2.2f);
    }
    fan_gradient(dl, c, IM_COL32(255, 230, 120, 120), ring, 32, IM_COL32(255, 220, 90, 0));
    const float flap = 0.55f + 0.45f * std::fabs(std::sin(t * 18.0f));
    const ImU32 wing = IM_COL32(240, 220, 255, 120), edge = IM_COL32(255, 255, 255, 170);
    for (int side : {-1, 1})
        for (int k = 0; k < 2; ++k) {
            const ImVec2 wc(c.x + side * s * (0.85f - 0.15f * k), c.y - s * (0.35f - 0.55f * k));
            const ImVec2 r(s * (0.95f - 0.15f * k), s * 0.32f * flap);
            const float rot = side * (k ? 0.45f : -0.35f);
            dl->AddEllipseFilled(wc, r, wing, rot);
            dl->AddEllipse(wc, r, edge, rot, 0, std::max(1.0f, s * 0.04f));
        }
    dl->AddEllipseFilled(ImVec2(c.x, c.y + s * 0.35f), ImVec2(s * 0.18f, s * 0.55f), IM_COL32(245, 190, 40, 255));
    dl->AddCircleFilled(ImVec2(c.x, c.y - s * 0.3f), s * 0.3f, IM_COL32(255, 214, 60, 255));
    dl->AddCircleFilled(ImVec2(c.x - s * 0.11f, c.y - s * 0.36f), s * 0.08f, IM_COL32(40, 20, 10, 255));
    dl->AddCircleFilled(ImVec2(c.x + s * 0.11f, c.y - s * 0.36f), s * 0.08f, IM_COL32(40, 20, 10, 255));
}

// Text filled with a vertical gradient and outlined (title lettering).
void title_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, const char *text, ImU32 top,
                ImU32 bottom, ImU32 rim, float rim_w) {
    const float o = rim_w;
    for (int i = 0; i < 16; ++i) {
        const float a = i * (6.2831853f / 16);
        dl->AddText(font, size, ImVec2(pos.x + std::cos(a) * o * 1.9f, pos.y + std::sin(a) * o * 1.9f + o),
                    kInk, text);
    }
    for (int i = 0; i < 16; ++i) {
        const float a = i * (6.2831853f / 16);
        dl->AddText(font, size, ImVec2(pos.x + std::cos(a) * o, pos.y + std::sin(a) * o), rim, text);
    }
    const ImVec2 sz = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
    const int v0 = dl->VtxBuffer.Size;
    dl->AddText(font, size, pos, IM_COL32_WHITE, text);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, ImVec2(pos.x, pos.y + sz.y * 0.2f),
                                                  ImVec2(pos.x, pos.y + sz.y * 0.85f), top, bottom);
}

// Rounded rect with a vertical gradient.
void gradient_rect(ImDrawList *dl, ImVec2 a, ImVec2 b, ImU32 top, ImU32 bottom, float rounding) {
    const int v0 = dl->VtxBuffer.Size;
    dl->AddRectFilled(a, b, IM_COL32_WHITE, rounding);
    ImGui::ShadeVertsLinearColorGradientKeepAlpha(dl, v0, dl->VtxBuffer.Size, a, ImVec2(a.x, b.y), top, bottom);
}

// The logo plaque: wings, gold-framed plate, "SPYRO" and the subtitle.
void draw_title(ImDrawList *dl, ImVec2 a, ImVec2 b, float fs, float t) {
    const float cy = (a.y + b.y) * 0.5f;
    draw_wing(dl, ImVec2(a.x + fs * 0.6f, cy + fs * 0.6f), fs * 1.7f, -1.0f);
    draw_wing(dl, ImVec2(b.x - fs * 0.6f, cy + fs * 0.6f), fs * 1.7f, 1.0f);
    gradient_rect(dl, a, b, IM_COL32(74, 30, 128, 255), IM_COL32(30, 10, 60, 255), fs * 1.2f);
    draw_gold_band(dl, a, b, fs * 1.2f, fs * 0.3f);
    dl->AddRect(ImVec2(a.x + fs * 0.6f, a.y + fs * 0.6f), ImVec2(b.x - fs * 0.6f, b.y - fs * 0.6f),
                IM_COL32(236, 186, 74, 120), fs * 0.8f, 0, std::max(1.0f, fs * 0.06f));
    draw_scroll(dl, ImVec2(a.x + fs * 1.4f, b.y - fs * 0.6f), ImVec2(1, 0), ImVec2(0, -1), fs * 0.7f);
    draw_scroll(dl, ImVec2(b.x - fs * 1.4f, b.y - fs * 0.6f), ImVec2(-1, 0), ImVec2(0, -1), fs * 0.7f);
    draw_gem(dl, ImVec2((a.x + b.x) * 0.5f, a.y - fs * 0.15f), fs * 1.05f);

    ImFont *big = title_font ? title_font : banner_font ? banner_font : ImGui::GetFont();
    const float ts = title_font ? title_font->FontSize : fs * 4.0f;
    const char *name = "SPYRO";
    const ImVec2 nsz = big->CalcTextSizeA(ts, FLT_MAX, 0.0f, name);
    const float cx = (a.x + b.x) * 0.5f;
    title_text(dl, big, ts, ImVec2(cx - nsz.x * 0.5f, a.y + fs * 0.35f), name, IM_COL32(214, 160, 255, 255),
               IM_COL32(112, 36, 196, 255), kGold, ts * 0.035f);
    // Shine sweeping across the name now and then.
    const float sweep = std::fmod(t * 0.35f, 2.2f) - 0.6f;
    if (sweep > -0.2f && sweep < 1.2f)
        draw_sparkle(dl, ImVec2(cx - nsz.x * 0.5f + nsz.x * sweep, a.y + fs * 0.35f + nsz.y * 0.35f),
                     fs * 0.7f, IM_COL32(255, 255, 255, 200));
    ImFont *sub = banner_font ? banner_font : ImGui::GetFont();
    const float ss = fs * 1.35f;
    const char *subtitle = "ENTER THE DRAGONFLY";
    const ImVec2 ssz = sub->CalcTextSizeA(ss, FLT_MAX, 0.0f, subtitle);
    title_text(dl, sub, ss, ImVec2(cx - ssz.x * 0.5f, b.y - fs * 0.75f - ssz.y), subtitle,
               IM_COL32(255, 236, 160, 255), IM_COL32(226, 146, 40, 255), IM_COL32(90, 40, 20, 255),
               ss * 0.04f);
}

// A big themed menu button; true when activated (click, Enter, gamepad A).
bool menu_button(const char *label, float width, bool enabled = true) {
    const float fs = ImGui::GetFontSize();
    const ImVec2 size(width, fs * 1.95f);
    ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
    const int me = S.index++;
    S.enabled.push_back(enabled);
    const bool clicked = ImGui::InvisibleButton(label, size);
    if (enabled && ImGui::IsItemHovered() && S.mouse_moved)
        S.sel = me; // the mouse moves the highlight
    const bool hot = enabled && S.sel == me;
    const bool pressed = (clicked && enabled) || (hot && S.activate);
    const ImVec2 a = ImGui::GetItemRectMin(), b = ImGui::GetItemRectMax();
    ImDrawList *dl = ImGui::GetWindowDrawList();
    const float r = fs * 0.45f;
    if (hot) {
        gradient_rect(dl, a, b, IM_COL32(150, 86, 232, 255), IM_COL32(92, 36, 170, 255), r);
        dl->AddRect(ImVec2(a.x - 2, a.y - 2), ImVec2(b.x + 2, b.y + 2), IM_COL32(255, 220, 120, 90), r + 2, 0,
                    fs * 0.3f);
        dl->AddRect(a, b, kGoldLight, r, 0, fs * 0.12f);
        draw_gem(dl, ImVec2(a.x - fs * 0.25f, (a.y + b.y) * 0.5f), fs * 0.62f);
    } else {
        gradient_rect(dl, a, b, IM_COL32(84, 36, 140, 235), IM_COL32(44, 16, 84, 235), r);
        dl->AddRect(a, b, enabled ? kGold : IM_COL32(150, 120, 90, 160), r, 0, fs * 0.08f);
    }
    ImFont *font = ImGui::GetFont();
    const float ts = fs * 1.15f;
    const ImVec2 tsz = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, label, ImGui::FindRenderedTextEnd(label));
    const ImVec2 tp((a.x + b.x - tsz.x) * 0.5f, (a.y + b.y - tsz.y) * 0.5f);
    dl->AddText(font, ts, ImVec2(tp.x, tp.y + 2), IM_COL32(20, 6, 40, 200), label,
                ImGui::FindRenderedTextEnd(label));
    dl->AddText(font, ts, tp,
                !enabled ? IM_COL32(170, 150, 190, 255) : hot ? IM_COL32(255, 248, 225, 255)
                                                              : IM_COL32(236, 226, 255, 255),
                label, ImGui::FindRenderedTextEnd(label));
    return pressed && enabled;
}

// Footer status: an error, the highlighted entry's detail, or the disc state.
std::string footer_status(ImU32 &col) {
    col = IM_COL32(245, 236, 255, 255);
    if (!S.message.empty()) {
        col = IM_COL32(255, 176, 120, 255);
        return S.message;
    }
    if (!S.hint.empty()) return S.hint;
    if (playable(S.info)) return "Ready: " + S.info.title + "  (" + S.info.id + ")";
    return "Select a game image to begin.";
}

// `text` split into lines that fit `wrap` pixels at font size fs.
std::vector<std::pair<const char *, const char *>> wrap_lines(const std::string &text, float wrap, float fs) {
    std::vector<std::pair<const char *, const char *>> lines;
    ImFont *font = ImGui::GetFont();
    const char *p = text.c_str(), *end = p + text.size();
    while (p < end) {
        const char *eol = font->CalcWordWrapPositionA(fs / font->FontSize, p, end, wrap);
        if (eol == p) ++eol;
        lines.emplace_back(p, eol);
        p = eol;
        while (p < end && *p == ' ') ++p;
    }
    return lines;
}

constexpr float kLineStep = 1.15f; // footer line spacing, in font sizes

// Footer box height for `lines` status lines.
float footer_height(size_t lines, float fs) { return std::max<size_t>(lines, 1) * fs * kLineStep + fs * 2.6f; }

void footer_box(ImVec2 a, ImVec2 b, float fs, const std::string &status, ImU32 col, float wrap) {
    ImDrawList *dl = ImGui::GetWindowDrawList();
    gradient_rect(dl, a, b, IM_COL32(30, 10, 56, 220), IM_COL32(20, 6, 40, 220), fs * 0.8f);
    dl->AddRect(a, b, kGold, fs * 0.8f, 0, fs * 0.07f);
    ImFont *font = ImGui::GetFont();
    const float cx = (a.x + b.x) * 0.5f;
    // Wrapped lines, each centred.
    float y = a.y + fs * 0.5f;
    for (const auto &[p, eol] : wrap_lines(status, wrap, fs)) {
        const ImVec2 lsz = font->CalcTextSizeA(fs, FLT_MAX, 0.0f, p, eol);
        dl->AddText(font, fs, ImVec2(cx - lsz.x * 0.5f, y), col, p, eol);
        y += fs * kLineStep;
    }
    const float my = y + fs * 0.35f;
    dl->AddLine(ImVec2(cx - fs * 7, my), ImVec2(cx - fs * 0.6f, my), IM_COL32(236, 186, 74, 110), 1.5f);
    dl->AddLine(ImVec2(cx + fs * 0.6f, my), ImVec2(cx + fs * 7, my), IM_COL32(236, 186, 74, 110), 1.5f);
    draw_gem(dl, ImVec2(cx, my - fs * 0.05f), fs * 0.3f);
    const std::string hints = (playable(S.info) ? file_name(S.disc) : std::string("No disc loaded")) +
                              "   \xC2\xB7   Enter: confirm   \xC2\xB7   Esc: " +
                              (S.screen == Screen::Main ? "quit" : "back");
    const float hs = fs * 0.9f;
    const ImVec2 hsz = font->CalcTextSizeA(hs, FLT_MAX, 0.0f, hints.c_str());
    dl->AddText(font, hs, ImVec2(cx - hsz.x * 0.5f, my + fs * 0.4f), IM_COL32(190, 170, 215, 255),
                hints.c_str());
}

void draw() {
    const ImGuiViewport *vp = ImGui::GetMainViewport();
    const float fs = ImGui::GetFontSize();
    const float t = (float)(now_s() - S.t0);
    draw_scene(ImGui::GetBackgroundDrawList(), vp->Size, t);
    if (ui::is_open()) return; // settings window on top

    // Selection: reset on a new screen, then apply Up/Down over the buttons
    // the last frame drew, skipping disabled ones.
    if (S.shown != S.screen) {
        S.shown = S.screen;
        S.sel = 0;
        S.enabled.clear();
    }
    const int n = (int)S.enabled.size();
    if (n) {
        if (S.sel >= n || !S.enabled[S.sel]) S.sel = 0;
        for (int step = 0; S.move && step < n; ++step) {
            S.sel = (S.sel + S.move + n) % n;
            if (S.enabled[S.sel]) break;
        }
    }
    S.move = 0;
    S.index = 0;
    S.enabled.clear();
    const bool ready = playable(S.info);
    int rows = 0;
    switch (S.screen) {
    case Screen::Main: rows = 7 + (ready ? 1 : 0); break;
    case Screen::Recent: rows = std::max<int>(1, (int)Config::get().recent_discs.size()) + 1; break;
    case Screen::Files: rows = 5; break;
    case Screen::Credits: rows = 1; break;
    }
    const float row_h = fs * 1.95f + ImGui::GetStyle().ItemSpacing.y;
    const float pw = std::min(fs * 25.0f, vp->Size.x * 0.92f);
    // Footer sized to its (wrapped) status text.
    S.hint.clear();
    if (S.screen == Screen::Recent && S.sel < (int)Config::get().recent_discs.size())
        S.hint = Config::get().recent_discs[S.sel];
    if (S.screen == Screen::Files) {
        const Config &cfg = Config::get();
        if (S.sel <= 1)
            S.hint = !cfg.memcard_path.empty() ? cfg.memcard_path : "memcard_A.raw next to the game";
        else if (S.sel <= 3)
            S.hint = "The game runs without a BIOS: only its fonts are used. Default: the font files "
                     "next to the game.";
    }
    ImU32 status_col;
    const std::string status = footer_status(status_col);
    const float wrap = pw - fs * 4.4f;
    const float top_room = fs * 2.6f, footer_h = footer_height(wrap_lines(status, wrap, fs).size(), fs);
    // Same panel size on every screen (the main menu's), taller only for a
    // long recent list.
    const float body = std::max(rows, 8) * row_h;
    const float plaque_h = fs * 7.2f;
    const float ph = std::min(top_room + body + footer_h + fs * 2.2f, vp->Size.y - plaque_h * 0.75f - fs);
    const float total = plaque_h * 0.75f + ph;
    const ImVec2 pos(vp->GetCenter().x - pw * 0.5f, vp->GetCenter().y - total * 0.5f + plaque_h * 0.75f);

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(ImVec2(pw, ph));
    ImGui::Begin("##launcher", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBackground |
                     ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoScrollWithMouse |
                     ImGuiWindowFlags_NoNav);
    // Panel behind, plaque in front.
    draw_menu_frame(pos, ImVec2(pw, ph), nullptr);
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    const float plw = pw * 1.02f;
    const ImVec2 pa(vp->GetCenter().x - plw * 0.5f, pos.y - plaque_h * 0.75f),
        pb(vp->GetCenter().x + plw * 0.5f, pos.y + plaque_h * 0.25f);
    draw_title(fg, pa, pb, fs, t);
    draw_dragonfly(fg, ImVec2(pb.x + fs * 4.2f, pa.y - fs * 1.6f), fs * 1.1f, t);

    ImGui::SetCursorPosY(top_room + fs * 0.4f);
    const float bw = pw * 0.62f;
    switch (S.screen) {
    case Screen::Main:
        if (ready && menu_button("Play", bw)) {
            remember(S.disc);
            S.done = true;
        }
        if (menu_button("Open Spyro ISO", bw)) S.pick = kPickDisc;
        if (menu_button("Recent ISOs", bw, !Config::get().recent_discs.empty())) {
            S.screen = Screen::Recent;
            S.message.clear();
        }
        if (menu_button("Memory Card & BIOS", bw)) {
            S.screen = Screen::Files;
            S.message.clear();
        }
        if (menu_button("Settings", bw)) ui::open_settings(0);
        if (menu_button("Controller Setup", bw)) ui::open_settings(2);
        if (menu_button("Credits", bw)) S.screen = Screen::Credits;
        if (menu_button("Exit", bw)) S.quit = S.done = true;
        break;
    case Screen::Recent: {
        const auto recent = Config::get().recent_discs; // copy: choose() reorders it
        for (size_t i = 0; i < recent.size(); ++i) {
            std::error_code ec;
            const bool there = fs::exists(recent[i], ec);
            const std::string label = file_name(recent[i]) + (there ? "" : "  (missing)") + "##r" +
                                      std::to_string(i);
            if (menu_button(label.c_str(), pw * 0.86f, there)) choose(recent[i]);
        }
        if (menu_button("Back", bw)) S.screen = Screen::Main;
        break;
    }
    case Screen::Files: {
        Config &cfg = Config::get();
        const std::string card = file_label("Memory Card", cfg.memcard_path) + "##card";
        const std::string bios = file_label("BIOS", cfg.bios_path) + "##bios";
        if (menu_button(card.c_str(), pw * 0.86f)) S.pick = kPickCard;
        if (menu_button("Use Default Memory Card", pw * 0.86f, !cfg.memcard_path.empty())) {
            cfg.memcard_path.clear();
            ui::save_settings();
            S.message.clear();
        }
        if (menu_button(bios.c_str(), pw * 0.86f)) S.pick = kPickBios;
        if (menu_button("Use Default BIOS", pw * 0.86f, !cfg.bios_path.empty())) {
            cfg.bios_path.clear();
            ui::save_settings();
            S.message.clear();
        }
        if (menu_button("Back", bw)) {
            S.screen = Screen::Main;
            S.message.clear();
        }
        break;
    }
    case Screen::Credits: {
        const char *lines[] = {
            "Spyro: Enter the Dragonfly",
            "and Spyro are trademarks of their respective owners.",
            "This PC port needs your own copy of the game disc.",
            "",
            "Static recompilation: NWiiRecomp",
            "by Volodymyr Vovchok / BlackLine Interactive",
            "Libraries: SDL2, Dear ImGui, toml++, glad, zstd",
            "Thanks for playing!",
        };
        ImDrawList *dl = ImGui::GetWindowDrawList();
        float y = ImGui::GetCursorScreenPos().y + fs * 0.4f;
        for (const char *l : lines) {
            const ImVec2 sz = ImGui::CalcTextSize(l);
            dl->AddText(ImVec2(vp->GetCenter().x - sz.x * 0.5f, y),
                        l == lines[0] ? kGoldLight : IM_COL32(236, 226, 255, 255), l);
            y += fs * 1.25f;
        }
        ImGui::SetCursorScreenPos(ImVec2(ImGui::GetCursorScreenPos().x, y + fs * 0.6f));
        if (menu_button("Back", bw)) S.screen = Screen::Main;
        break;
    }
    }
    footer_box(ImVec2(pos.x + fs * 1.4f, pos.y + ph - footer_h - fs * 1.0f),
               ImVec2(pos.x + pw - fs * 1.4f, pos.y + ph - fs * 1.0f), fs, status, status_col, wrap);
    ImGui::End();
}

} // namespace

std::string run(SDL_Window *window, const std::string &disc) {
    S = State{};
    S.window = window;
    S.t0 = now_s();
    S.expected_id = Config::get().game_id;
    if (!disc.empty()) {
        S.info = probe(disc, S.expected_id);
        if (playable(S.info))
            S.disc = disc;
        else
            S.message = S.info.error;
    }
    ui::set_launcher_mode(true);
    // NWII_LAUNCHER_SELECT=<n>: start with button n highlighted (tests).
    if (const char *v = std::getenv("NWII_LAUNCHER_SELECT")) S.sel = std::atoi(v);
    // NWII_LAUNCHER_SHOT=<file.bmp>: save one frame after ~1 s (tests).
    const char *shot = std::getenv("NWII_LAUNCHER_SHOT");
    int frames = 0;
    while (!S.done) {
        const auto frame_start = std::chrono::steady_clock::now();
        // NWII_LAUNCHER_ENTER=<frame>: press Enter at that frame (tests).
        static const int enter_at = [] { const char *v = std::getenv("NWII_LAUNCHER_ENTER"); return v ? std::atoi(v) : -1; }();
        if (frames + 1 == enter_at || frames == enter_at) {
            SDL_Event k{};
            k.type = frames == enter_at ? SDL_KEYUP : SDL_KEYDOWN;
            k.key.state = frames == enter_at ? SDL_RELEASED : SDL_PRESSED;
            k.key.keysym.sym = SDLK_RETURN;
            k.key.keysym.scancode = SDL_SCANCODE_RETURN;
            k.key.windowID = SDL_GetWindowID(window);
            SDL_PushEvent(&k);
        }
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
            if (e.type == SDL_QUIT) S.quit = S.done = true;
            if (e.type == SDL_MOUSEMOTION && frames > 10) S.mouse_moved = true;
            if (ui::handle_event(e) || ui::is_open()) continue;
            const bool key = e.type == SDL_KEYDOWN;
            const SDL_Keycode k = key ? e.key.keysym.sym : 0;
            const int btn = e.type == SDL_CONTROLLERBUTTONDOWN ? e.cbutton.button : -1;
            if ((key && !e.key.repeat && k == SDLK_ESCAPE) || btn == SDL_CONTROLLER_BUTTON_B) {
                if (S.screen != Screen::Main)
                    S.screen = Screen::Main;
                else if (key)
                    S.quit = S.done = true;
            }
            if (k == SDLK_UP || k == SDLK_w || btn == SDL_CONTROLLER_BUTTON_DPAD_UP) S.move = -1;
            if (k == SDLK_DOWN || k == SDLK_s || btn == SDL_CONTROLLER_BUTTON_DPAD_DOWN) S.move = 1;
            if ((key && !e.key.repeat && (k == SDLK_RETURN || k == SDLK_KP_ENTER || k == SDLK_SPACE)) ||
                btn == SDL_CONTROLLER_BUTTON_A || btn == SDL_CONTROLLER_BUTTON_START)
                S.activate = true;
            // Left stick: one step per push past half way.
            if (e.type == SDL_CONTROLLERAXISMOTION && e.caxis.axis == SDL_CONTROLLER_AXIS_LEFTY) {
                static int held = 0;
                const int dir = e.caxis.value < -16000 ? -1 : e.caxis.value > 16000 ? 1 : 0;
                if (dir && dir != held) S.move = dir;
                held = dir;
            }
        }
        int w = 0, h = 0;
        SDL_GL_GetDrawableSize(window, &w, &h);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glViewport(0, 0, w, h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);
        ui::frame(&draw);
        S.activate = false;
        S.mouse_moved = false;
        if (++frames == 60 && shot) {
            std::vector<unsigned char> px((size_t)w * h * 4), flip(px.size());
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            for (int y = 0; y < h; ++y)
                std::memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
            for (size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
            if (SDL_Surface *sf = SDL_CreateRGBSurfaceFrom(flip.data(), w, h, 32, w * 4, 0xFF, 0xFF00,
                                                           0xFF0000, 0xFF000000)) {
                SDL_SaveBMP(sf, shot);
                SDL_FreeSurface(sf);
            }
        }
        SDL_GL_SwapWindow(window);

        const auto req = ui::take_requests();
        if (req.rebuild_efb) efb::create();
        if (req.vsync) SDL_GL_SetSwapInterval(Config::get().enable_vsync ? 1 : 0);
        if (req.fullscreen)
            SDL_SetWindowFullscreen(window, Config::get().fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        if (req.quit) S.quit = S.done = true;

        if (S.pick) {
            const int pick = S.pick;
            S.pick = kPickNone;
            S.message.clear();
            Config &cfg = Config::get();
            const std::string path = pick == kPickDisc ? pick_disc() : pick == kPickCard ? pick_card() : pick_bios();
            if (!path.empty() && pick == kPickDisc) choose(path);
            if (!path.empty() && pick == kPickCard) choose_file(cfg.memcard_path, path, hw::memcard_file_error(path));
            if (!path.empty() && pick == kPickBios) choose_file(cfg.bios_path, path, hw::bios_file_error(path));
        }
        // ~60 fps is plenty for a menu (and keeps the GPU idle).
        std::this_thread::sleep_until(frame_start + std::chrono::milliseconds(16));
    }
    ui::set_launcher_mode(false);
    if (S.quit) return {};
    std::cout << "[Launcher] booting " << S.disc << std::endl;
    return S.disc;
}

} // namespace nwii::runtime::launcher
