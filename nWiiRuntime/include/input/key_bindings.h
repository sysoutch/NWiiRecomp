#pragma once
#include <cstdint>
#include <string>

// Keyboard -> GameCube pad 0 bindings: one SDL scancode per pad control.
// Loaded from the [keyboard] section of the game's TOML ("a = \"Space\"",
// names as SDL_GetScancodeName prints them), edited live in the F1 settings
// overlay and saved back there.
namespace nwii::runtime::input {

enum class PadControl {
    A, B, X, Y, Z, L, R, Start,
    DUp, DDown, DLeft, DRight,
    StickUp, StickDown, StickLeft, StickRight,
    CUp, CDown, CLeft, CRight,
    Count
};

struct KeyBinding {
    const char *id;     // TOML key, e.g. "stick_up"
    const char *label;  // shown in the overlay
    int scancode;       // SDL_Scancode currently bound (0 = unbound)
    int default_scancode;
};

// Table indexed by PadControl.
KeyBinding *key_bindings();
constexpr int kNumPadControls = (int)PadControl::Count;

inline KeyBinding &binding(PadControl c) { return key_bindings()[(int)c]; }

void reset_key_bindings();
// Bind from a scancode name; false (and unchanged) for an unknown name.
bool set_key_binding(const std::string &id, const std::string &key_name);

// Controller -> GameCube pad buttons (A ... D-pad right, PadControl order),
// from the [controller] section (e.g. z = "righttrigger"), edited live in the
// overlay. An input is an SDL_GameControllerButton, or kPadAxisBase + an
// SDL_GameControllerAxis (the triggers: pressed above half travel). The
// sticks stay fixed: left = main stick, right = C-stick.
constexpr int kNumPadButtonBindings = (int)PadControl::DRight + 1;
constexpr int kPadAxisBase = 100;
constexpr int kPadUnbound = -1;
struct PadBinding {
    const char *id;
    const char *label;
    int input;
    int default_input;
};
PadBinding *pad_bindings();
void reset_pad_bindings();
bool set_pad_binding(const std::string &id, const std::string &input_name);
std::string pad_input_name(int input); // "a", "lefttrigger", "none"
// controller is an SDL_GameController*.
bool pad_input_pressed(void *controller, int input);
uint8_t pad_input_analog(void *controller, int input); // 0-255

} // namespace nwii::runtime::input
