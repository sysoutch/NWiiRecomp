#include "input/input_manager.h"
#include "sources/mouse_keyboard_source.h"
#include <iostream>
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <mutex>
#include <string>
#include <vector>

namespace nwii::runtime::input {

InputManager& InputManager::get() {
    static InputManager instance;
    return instance;
}

InputManager::InputManager() {
    for (int i = 0; i < 4; ++i) {
        gc_pads[i] = {0, 0, 0, 0, 0, 0, 0, 0, 0, -1};
        wii_motes[i] = {0, 0.5f, 0.5f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, -1};
    }
}

void InputManager::add_source(std::unique_ptr<IInputSource> source) {
    if (source) {
        sources.push_back(std::move(source));
        std::cout << "[InputManager] Registered a new input source. Total sources: " << sources.size() << std::endl;
    }
}

void InputManager::set_mode(InputMode mode) {
    current_mode = mode;
    std::cout << "[InputManager] Switched input mode to " << (int)mode << std::endl;
}

void InputManager::update() {

    

    for (int i = 0; i < 4; ++i) {
        gc_pads[i].buttons = 0;
        gc_pads[i].stick_x = 0;
        gc_pads[i].stick_y = 0;
        gc_pads[i].substick_x = 0;
        gc_pads[i].substick_y = 0;
        gc_pads[i].trigger_l = 0;
        gc_pads[i].trigger_r = 0;
        gc_pads[i].analog_a = 0;
        gc_pads[i].analog_b = 0;
        gc_pads[i].err = -1; 

        wii_motes[i].buttons = 0;
        wii_motes[i].err = -1; 
    }

    for (auto& source : sources) {
        source->update(gc_pads, wii_motes);
    }
    apply_input_script();
    apply_mouse_camera(gc_pads[0]);
    if (suppressed) {
        for (int i = 0; i < 4; ++i) {
            const int err = gc_pads[i].err;
            gc_pads[i] = {};
            gc_pads[i].err = err;
            wii_motes[i].buttons = 0;
        }
    }
}

// NWII_INPUT_SCRIPT="sec:NAME:ms,...": press GameCube pad 0 controls on a
// timeline measured from the first input update -- reproducible test runs
// that don't depend on the window having keyboard focus. NAME is A B X Y Z L
// R START, DUP/DDOWN/DLEFT/DRIGHT (D-pad), UP/DOWN/LEFT/RIGHT (main stick),
// CUP/CDOWN/CLEFT/CRIGHT (C-stick).
// Presses injected at run time (NWII_CTRL command file, see main.cpp).
struct InjectedStep { std::chrono::steady_clock::time_point at; std::string name; int ms; };
static std::mutex g_inject_mu;
static std::vector<InjectedStep> g_injected;

void inject_press(const std::string &name, int ms) {
    std::lock_guard<std::mutex> lk(g_inject_mu);
    g_injected.push_back({std::chrono::steady_clock::now(), name, ms});
}

void InputManager::apply_input_script() {
    struct Step { double at; std::string name; int ms; };
    static const std::vector<Step> steps = [] {
        std::vector<Step> v;
        const char *env = std::getenv("NWII_INPUT_SCRIPT");
        if (!env) return v;
        std::string s(env);
        size_t p = 0;
        while (p < s.size()) {
            size_t e = s.find(',', p);
            if (e == std::string::npos) e = s.size();
            const std::string item = s.substr(p, e - p);
            const size_t c1 = item.find(':'), c2 = item.rfind(':');
            if (c1 != std::string::npos && c2 != c1)
                v.push_back({std::atof(item.substr(0, c1).c_str()),
                             item.substr(c1 + 1, c2 - c1 - 1),
                             std::atoi(item.substr(c2 + 1).c_str())});
            p = e + 1;
        }
        std::cout << "[InputManager] input script: " << v.size() << " steps\n";
        return v;
    }();
    std::vector<std::string> active;
    {
        std::lock_guard<std::mutex> lk(g_inject_mu);
        const auto t = std::chrono::steady_clock::now();
        g_injected.erase(std::remove_if(g_injected.begin(), g_injected.end(),
                             [&](const InjectedStep &st) {
                                 return t > st.at + std::chrono::milliseconds(st.ms);
                             }),
                         g_injected.end());
        for (const auto &st : g_injected) active.push_back(st.name);
    }
    if (steps.empty() && active.empty()) return;
    static const auto t0 = std::chrono::steady_clock::now();
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
    auto &pad = gc_pads[0];
    for (const Step &st : steps)
        if (now >= st.at && now <= st.at + st.ms / 1000.0) active.push_back(st.name);
    for (const std::string &n : active) {
        pad.err = 0;
        if (n == "A") pad.buttons |= 0x0100;
        else if (n == "B") pad.buttons |= 0x0200;
        else if (n == "X") pad.buttons |= 0x0400;
        else if (n == "Y") pad.buttons |= 0x0800;
        else if (n == "Z") pad.buttons |= 0x0010;
        else if (n == "L") { pad.buttons |= 0x0040; pad.trigger_l = 255; }
        else if (n == "R") { pad.buttons |= 0x0020; pad.trigger_r = 255; }
        else if (n == "START") pad.buttons |= 0x1000;
        else if (n == "DUP") pad.buttons |= 0x0008;
        else if (n == "DDOWN") pad.buttons |= 0x0004;
        else if (n == "DLEFT") pad.buttons |= 0x0001;
        else if (n == "DRIGHT") pad.buttons |= 0x0002;
        else if (n == "UP") pad.stick_y = 127;
        else if (n == "DOWN") pad.stick_y = -128;
        else if (n == "LEFT") pad.stick_x = -128;
        else if (n == "RIGHT") pad.stick_x = 127;
        else if (n == "CUP") pad.substick_y = 127;
        else if (n == "CDOWN") pad.substick_y = -128;
        else if (n == "CLEFT") pad.substick_x = -128;
        else if (n == "CRIGHT") pad.substick_x = 127;
    }
}

GameCubePadState InputManager::get_gcpad_state(int index) {
    if (index >= 0 && index < 4) return gc_pads[index];
    return {0,0,0,0,0,0,0,0,0,-1};
}

WiimoteState InputManager::get_wiimote_state(int index) {
    if (index >= 0 && index < 4) return wii_motes[index];
    return {0,0.5f,0.5f,0,0,0,0,0,0,-1};
}

} 
