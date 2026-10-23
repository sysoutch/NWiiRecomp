#include "mouse_keyboard_source.h"
#include "runtime/config.h"
#include "input/key_bindings.h"
#include <SDL.h>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace nwii::runtime::input {

namespace {
std::mutex s_inject_mtx;
double s_inject[2] = {0, 0};    // remaining synthetic motion (x, y)
double s_inject_rate[2] = {0, 0}; // per second
} // namespace

void mouse_camera_event(const SDL_Event &e, bool ui_open) {
    const bool captured = SDL_GetRelativeMouseMode() == SDL_TRUE;
    if (e.type == SDL_MOUSEBUTTONDOWN && e.button.button == SDL_BUTTON_LEFT && !ui_open &&
        !captured && Config::get().mouse_camera)
        SDL_SetRelativeMouseMode(SDL_TRUE);
    else if (captured && ((e.type == SDL_KEYDOWN && e.key.keysym.sym == SDLK_ESCAPE) ||
                          (e.type == SDL_WINDOWEVENT &&
                           e.window.event == SDL_WINDOWEVENT_FOCUS_LOST)))
        SDL_SetRelativeMouseMode(SDL_FALSE);
}

void inject_mouse_motion(int dx, int dy, int ms) {
    std::lock_guard<std::mutex> lk(s_inject_mtx);
    const double s = std::max(ms, 1) / 1000.0;
    s_inject[0] = dx;
    s_inject[1] = dy;
    s_inject_rate[0] = dx / s;
    s_inject_rate[1] = dy / s;
}

// Mouse speed -> stick deflection. The game turns the camera at a rate set
// by the stick, so the mouse's speed (not its position) sets the deflection:
// sensitivity 50 reaches full deflection at ~600 px/s, smoothed over a couple
// of polls, back to centre when the mouse stops, small deflections starting
// above the game's dead zone. Normally the horizontal motion drives the
// C-stick (C-right orbits the camera so the view turns left, hence the
// sign; C-up/down do not tilt it). While Y is held the game is in its look
// mode, where the main stick aims airplane-style (up = look down): there the
// mouse aims in both axes. Runs after all input sources, so a controller's
// Y counts too, and pressed keys/sticks win.
void apply_mouse_camera(GameCubePadState &pad) {
    const Config &cfg = Config::get();
    static auto last = SDL_GetPerformanceCounter();
    static double sx = 0, sy = 0;
    const auto now = SDL_GetPerformanceCounter();
    const double dt = std::max(1e-4, (double)(now - last) / SDL_GetPerformanceFrequency());
    last = now;
    int rx = 0, ry = 0;
    const bool captured = SDL_GetRelativeMouseMode() == SDL_TRUE;
    if (captured) SDL_GetRelativeMouseState(&rx, &ry);
    double d[2] = {captured ? (double)rx : 0.0, captured ? (double)ry : 0.0};
    {
        std::lock_guard<std::mutex> lk(s_inject_mtx);
        for (int i = 0; i < 2; ++i)
            if (s_inject[i] != 0) {
                const double step = s_inject_rate[i] * dt;
                const double take = std::abs(step) >= std::abs(s_inject[i]) ? s_inject[i] : step;
                s_inject[i] -= take;
                d[i] += take;
            }
    }
    if (!cfg.mouse_camera) { sx = sy = 0; return; }
    const bool look = (pad.buttons & 0x0800) != 0; // Y held: the game's look mode
    const double k = 127.0 / 600.0 * (cfg.mouse_sensitivity / 50.0);
    const double a = std::min(1.0, dt / 0.03); // ~30 ms smoothing
    sx += ((look ? d[0] : -d[0]) / dt * k - sx) * a;
    sy += ((look ? d[1] : 0.0) / dt * k * (cfg.mouse_invert_y ? -1 : 1) - sy) * a;
    const auto to_stick = [](double v) -> int8_t {
        const double m = std::abs(v);
        if (m < 1.0) return 0;
        constexpr double kDead = 20.0; // GC games ignore roughly the first 15-20
        const double out = std::min(127.0, kDead + m * (127.0 - kDead) / 127.0);
        return (int8_t)std::lround(v < 0 ? -out : out);
    };
    const int8_t x = to_stick(sx), y = to_stick(sy);
    if (look) {
        if (pad.stick_x == 0 && pad.stick_y == 0) {
            pad.stick_x = x;
            pad.stick_y = y;
        }
    } else if (x && pad.substick_x == 0) {
        pad.substick_x = x;
    }
}

void MouseKeyboardSource::update(GameCubePadState pads[4], WiimoteState motes[4]) {
    int mx, my;
    uint32_t mouse_state = SDL_GetMouseState(&mx, &my);
    const Uint8 *keys = SDL_GetKeyboardState(NULL);

    motes[0].err = 0; 
    int w = nwii::runtime::Config::get().window_width;
    int h = nwii::runtime::Config::get().window_height;
    if (w == 0) w = 640;
    if (h == 0) h = 480;
    
    motes[0].ir_x = (float)mx / (float)w;
    motes[0].ir_y = (float)my / (float)h;

    if (mouse_state & SDL_BUTTON(SDL_BUTTON_LEFT)) motes[0].buttons |= 0x0800; 
    if (mouse_state & SDL_BUTTON(SDL_BUTTON_RIGHT)) motes[0].buttons |= 0x0400; 
    if (keys[SDL_SCANCODE_RETURN]) motes[0].buttons |= 0x0010; 
    if (keys[SDL_SCANCODE_BACKSPACE]) motes[0].buttons |= 0x0001; 
    if (keys[SDL_SCANCODE_UP]) motes[0].buttons |= 0x0008; 
    if (keys[SDL_SCANCODE_DOWN]) motes[0].buttons |= 0x0004; 
    if (keys[SDL_SCANCODE_LEFT]) motes[0].buttons |= 0x0001; 
    if (keys[SDL_SCANCODE_RIGHT]) motes[0].buttons |= 0x0002; 

    // GameCube pad 0 on the keyboard, through the rebindable table
    // (input/key_bindings.h; defaults WASD stick, arrows C-stick, Space A...).
    pads[0].err = 0;
    auto down = [&](PadControl c) {
        const int sc = binding(c).scancode;
        return sc > 0 && sc < SDL_NUM_SCANCODES && keys[sc];
    };
    if (down(PadControl::A)) pads[0].buttons |= 0x0100;
    if (down(PadControl::B)) pads[0].buttons |= 0x0200;
    if (down(PadControl::X)) pads[0].buttons |= 0x0400;
    if (down(PadControl::Y)) pads[0].buttons |= 0x0800;
    if (down(PadControl::Z)) pads[0].buttons |= 0x0010;
    if (down(PadControl::Start)) pads[0].buttons |= 0x1000;
    if (down(PadControl::L)) { pads[0].buttons |= 0x0040; pads[0].trigger_l = 255; }
    if (down(PadControl::R)) { pads[0].buttons |= 0x0020; pads[0].trigger_r = 255; }
    if (down(PadControl::DUp)) pads[0].buttons |= 0x0008;
    if (down(PadControl::DDown)) pads[0].buttons |= 0x0004;
    if (down(PadControl::DLeft)) pads[0].buttons |= 0x0001;
    if (down(PadControl::DRight)) pads[0].buttons |= 0x0002;
    if (down(PadControl::StickUp)) pads[0].stick_y = 127;
    if (down(PadControl::StickDown)) pads[0].stick_y = -128;
    if (down(PadControl::StickRight)) pads[0].stick_x = 127;
    if (down(PadControl::StickLeft)) pads[0].stick_x = -128;
    if (down(PadControl::CUp)) pads[0].substick_y = 127;
    if (down(PadControl::CDown)) pads[0].substick_y = -128;
    if (down(PadControl::CRight)) pads[0].substick_x = 127;
    if (down(PadControl::CLeft)) pads[0].substick_x = -128;
}

} 
