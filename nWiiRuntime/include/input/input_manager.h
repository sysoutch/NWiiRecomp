#pragma once
#include <string>
#include "input/input_source.h"
#include <cstdint>
#include <vector>
#include <memory>

namespace nwii::runtime::input {

enum class InputMode {
    GamepadClassic, 
    GamepadWiimote, 
    MouseKeyboard,  
    Touch,          
    Remote          
};

class InputManager {
public:
    static InputManager& get();

    void update();
    void apply_input_script(); // NWII_INPUT_SCRIPT timeline (see .cpp)
    void set_mode(InputMode mode);
    InputMode get_mode() const { return current_mode; }
    // While set (settings overlay open), pads stay connected but neutral.
    void set_suppressed(bool s) { suppressed = s; }
    bool is_suppressed() const { return suppressed; }

    void add_source(std::unique_ptr<IInputSource> source);

    GameCubePadState get_gcpad_state(int index);
    WiimoteState get_wiimote_state(int index);

private:
    InputManager();
    ~InputManager() = default;

    InputMode current_mode = InputMode::GamepadClassic;
    bool suppressed = false;

    GameCubePadState gc_pads[4];
    WiimoteState wii_motes[4];

    std::vector<std::unique_ptr<IInputSource>> sources;
};

// Press a pad control (NWII_INPUT_SCRIPT name) for ms milliseconds, starting
// now. Thread-safe; used by the NWII_CTRL test driver.
void inject_press(const std::string &name, int ms);

} 

