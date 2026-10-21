#include "mouse_keyboard_source.h"
#include "runtime/config.h"
#include "input/key_bindings.h"
#include <SDL.h>

namespace nwii::runtime::input {

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
