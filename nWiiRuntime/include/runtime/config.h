#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace nwii {
namespace runtime {

enum class Platform {
    Wii,
    GameCube,
    WiiU
};

enum class Backend {
    OpenGL,
    Metal
};

struct GameProfile {

    uint32_t callback_stack_top = 0;

    std::vector<std::string> disable_modules;

    
    std::string rpl_path;

    

    uint32_t ext_interrupt_dispatch = 0;

    

    uint32_t sched_lock_offset = 0;

    // __OSReschedule address: run after an external interrupt so a thread the
    // handler woke (e.g. the VI retrace queue) can preempt (NWII_RESCHED).
    uint32_t resched_addr = 0;
};

// Timebase from real elapsed time instead of the instruction count
// ([hle] wall_timebase or NWII_WALLTB); read on the hot path, so a plain flag.
extern bool g_wall_timebase;

class Config {
public:
    static Config& get() {
        static Config instance;
        return instance;
    }

    void load(const std::string& filepath);
    // The game's own keyboard defaults: the [keyboard] section of
    // <config name>.default.toml next to the loaded config, if any (after
    // the runtime's generic defaults, e.g. on "Reset to defaults").
    void apply_default_keyboard() const;

    
    void load_game_profile();

    Platform platform = Platform::Wii;
    Backend backend = Backend::OpenGL;
    bool enable_vsync = true;
    int window_width = 640;
    int window_height = 480;
    int internal_scale = 1; // EFB render scale (1 = native 640x480)
    int msaa = 1;           // multisample count for the EFB (1 = off)
    int anisotropy = 1;     // max anisotropic filtering for linear-filtered textures
    bool widescreen = false; // 16:9 output with a widened 3D field of view
    bool fullscreen = false; // borderless fullscreen on the current display
    bool show_fps = false;   // small game-FPS counter in the corner
    int frame_rate = 60;     // VI retrace rate = max game fps (60, 90 or 120)
    int volume = 100;        // [audio] master volume, percent
    bool mute = false;
    bool mouse_camera = true;   // [mouse] camera: mouse movement drives the C-stick
    int mouse_sensitivity = 50; // [mouse] sensitivity, 1-100
    bool mouse_invert_y = false; // [mouse] invert_y (look mode, Y held)
    bool launcher = true;    // [launcher] enabled: title menu before the game boots
    std::vector<std::string> recent_discs; // [launcher] recent, newest first
    std::string config_path; // the TOML loaded; the settings overlay saves here
    std::string game_dir;

    
    
    std::string game_id = "RSZK";

    

    

    
    int input_mode = 6;

    float gyro_sensitivity = 1.0f;

    float pointer_speed = 1.0f;

    int phone_port = 4313;

    GameProfile game_profile;

private:
    Config() = default;
};

} 
} 
