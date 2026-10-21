#include "input/key_bindings.h"
#include <SDL.h>

namespace nwii::runtime::input {

KeyBinding *key_bindings() {
    // Defaults: WASD main stick, arrows C-stick, Space A, F B, Left Shift X,
    // R Y, Tab Z, Q/E L/R, Enter Start, numpad 8/2/4/6 D-pad.
    static KeyBinding table[kNumPadControls] = {
        {"a", "A", SDL_SCANCODE_SPACE, SDL_SCANCODE_SPACE},
        {"b", "B", SDL_SCANCODE_F, SDL_SCANCODE_F},
        {"x", "X", SDL_SCANCODE_LSHIFT, SDL_SCANCODE_LSHIFT},
        {"y", "Y", SDL_SCANCODE_R, SDL_SCANCODE_R},
        {"z", "Z", SDL_SCANCODE_TAB, SDL_SCANCODE_TAB},
        {"l", "L trigger", SDL_SCANCODE_Q, SDL_SCANCODE_Q},
        {"r", "R trigger", SDL_SCANCODE_E, SDL_SCANCODE_E},
        {"start", "Start", SDL_SCANCODE_RETURN, SDL_SCANCODE_RETURN},
        {"dpad_up", "D-pad up", SDL_SCANCODE_KP_8, SDL_SCANCODE_KP_8},
        {"dpad_down", "D-pad down", SDL_SCANCODE_KP_2, SDL_SCANCODE_KP_2},
        {"dpad_left", "D-pad left", SDL_SCANCODE_KP_4, SDL_SCANCODE_KP_4},
        {"dpad_right", "D-pad right", SDL_SCANCODE_KP_6, SDL_SCANCODE_KP_6},
        {"stick_up", "Move forward", SDL_SCANCODE_W, SDL_SCANCODE_W},
        {"stick_down", "Move back", SDL_SCANCODE_S, SDL_SCANCODE_S},
        {"stick_left", "Move left", SDL_SCANCODE_A, SDL_SCANCODE_A},
        {"stick_right", "Move right", SDL_SCANCODE_D, SDL_SCANCODE_D},
        {"c_up", "Camera up", SDL_SCANCODE_UP, SDL_SCANCODE_UP},
        {"c_down", "Camera down", SDL_SCANCODE_DOWN, SDL_SCANCODE_DOWN},
        {"c_left", "Camera left", SDL_SCANCODE_LEFT, SDL_SCANCODE_LEFT},
        {"c_right", "Camera right", SDL_SCANCODE_RIGHT, SDL_SCANCODE_RIGHT},
    };
    return table;
}

void reset_key_bindings() {
    KeyBinding *t = key_bindings();
    for (int i = 0; i < kNumPadControls; ++i)
        t[i].scancode = t[i].default_scancode;
}

bool set_key_binding(const std::string &id, const std::string &key_name) {
    // SDL_GetScancodeFromName is a table lookup; it works before SDL_Init.
    const SDL_Scancode sc = SDL_GetScancodeFromName(key_name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    KeyBinding *t = key_bindings();
    for (int i = 0; i < kNumPadControls; ++i)
        if (id == t[i].id) {
            t[i].scancode = sc;
            return true;
        }
    return false;
}

PadBinding *pad_bindings() {
    // Defaults = the previous fixed layout.
    static PadBinding table[kNumPadButtonBindings] = {
        {"a", "A", SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_A},
        {"b", "B", SDL_CONTROLLER_BUTTON_B, SDL_CONTROLLER_BUTTON_B},
        {"x", "X", SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_X},
        {"y", "Y", SDL_CONTROLLER_BUTTON_Y, SDL_CONTROLLER_BUTTON_Y},
        {"z", "Z", kPadAxisBase + SDL_CONTROLLER_AXIS_TRIGGERRIGHT,
         kPadAxisBase + SDL_CONTROLLER_AXIS_TRIGGERRIGHT},
        {"l", "L trigger", SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_LEFTSHOULDER},
        {"r", "R trigger", SDL_CONTROLLER_BUTTON_RIGHTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER},
        {"start", "Start", SDL_CONTROLLER_BUTTON_START, SDL_CONTROLLER_BUTTON_START},
        {"dpad_up", "D-pad up", SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_UP},
        {"dpad_down", "D-pad down", SDL_CONTROLLER_BUTTON_DPAD_DOWN, SDL_CONTROLLER_BUTTON_DPAD_DOWN},
        {"dpad_left", "D-pad left", SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_LEFT},
        {"dpad_right", "D-pad right", SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
         SDL_CONTROLLER_BUTTON_DPAD_RIGHT},
    };
    return table;
}

void reset_pad_bindings() {
    PadBinding *t = pad_bindings();
    for (int i = 0; i < kNumPadButtonBindings; ++i)
        t[i].input = t[i].default_input;
}

bool set_pad_binding(const std::string &id, const std::string &input_name) {
    int input = kPadUnbound;
    if (input_name != "none") {
        const SDL_GameControllerButton b = SDL_GameControllerGetButtonFromString(input_name.c_str());
        const SDL_GameControllerAxis a = SDL_GameControllerGetAxisFromString(input_name.c_str());
        if (b != SDL_CONTROLLER_BUTTON_INVALID)
            input = b;
        else if (a == SDL_CONTROLLER_AXIS_TRIGGERLEFT || a == SDL_CONTROLLER_AXIS_TRIGGERRIGHT)
            input = kPadAxisBase + a;
        else
            return false;
    }
    PadBinding *t = pad_bindings();
    for (int i = 0; i < kNumPadButtonBindings; ++i)
        if (id == t[i].id) {
            t[i].input = input;
            return true;
        }
    return false;
}

std::string pad_input_name(int input) {
    const char *n = nullptr;
    if (input >= kPadAxisBase)
        n = SDL_GameControllerGetStringForAxis((SDL_GameControllerAxis)(input - kPadAxisBase));
    else if (input >= 0)
        n = SDL_GameControllerGetStringForButton((SDL_GameControllerButton)input);
    return n ? n : "none";
}

bool pad_input_pressed(void *controller, int input) {
    auto *c = static_cast<SDL_GameController *>(controller);
    if (!c || input < 0) return false;
    if (input >= kPadAxisBase)
        return SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)(input - kPadAxisBase)) > 16384;
    return SDL_GameControllerGetButton(c, (SDL_GameControllerButton)input) != 0;
}

uint8_t pad_input_analog(void *controller, int input) {
    auto *c = static_cast<SDL_GameController *>(controller);
    if (!c || input < 0) return 0;
    if (input >= kPadAxisBase) {
        const int v = SDL_GameControllerGetAxis(c, (SDL_GameControllerAxis)(input - kPadAxisBase));
        return (uint8_t)(v <= 0 ? 0 : v * 255 / 32767);
    }
    return SDL_GameControllerGetButton(c, (SDL_GameControllerButton)input) ? 255 : 0;
}

} // namespace nwii::runtime::input
