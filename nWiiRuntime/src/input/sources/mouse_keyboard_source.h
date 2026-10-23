#pragma once
#include "input/input_source.h"

union SDL_Event;

namespace nwii::runtime::input {

// Mouse camera capture, fed every SDL event from the main loop: a click
// into the game captures the mouse (relative mode), Esc / focus loss /
// the settings overlay release it.
void mouse_camera_event(const SDL_Event &e, bool ui_open);
// Mouse motion onto pad 0 (C-stick, or the main stick while Y is held);
// InputManager calls it after all sources.
void apply_mouse_camera(GameCubePadState &pad);
// Test hook (NWII_CTRL "mouse <dx> <ms> [dy]"): synthetic motion spread
// over ms, as if the mouse were captured.
void inject_mouse_motion(int dx, int dy, int ms);

class MouseKeyboardSource : public IInputSource {
public:
    MouseKeyboardSource() = default;
    ~MouseKeyboardSource() override = default;

    void update(GameCubePadState pads[4], WiimoteState motes[4]) override;
};

} 
