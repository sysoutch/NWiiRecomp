#include "runtime/config.h"
#include "input/key_bindings.h"
#include <toml++/toml.hpp>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <filesystem>

namespace nwii {
namespace runtime {

bool g_wall_timebase = std::getenv("NWII_WALLTB") != nullptr;
bool g_verbose = std::getenv("NWII_VERBOSE") != nullptr;

void Config::load(const std::string& filepath) {
    config_path = filepath;
    if (!std::filesystem::exists(filepath)) {
        std::cout << "[Config] Config file " << filepath << " not found, using defaults." << std::endl;
        return;
    }

    try {
        toml::table tbl = toml::parse_file(filepath);

        auto plat = tbl["system"]["platform"].value<std::string>();
        if (plat) {
            if (*plat == "GameCube" || *plat == "gamecube") {
                platform = Platform::GameCube;
                std::cout << "[Config] Platform set to GameCube." << std::endl;
            } else if (*plat == "WiiU" || *plat == "wiiu" || *plat == "Wii U") {
                platform = Platform::WiiU;
                std::cout << "[Config] Platform set to Wii U." << std::endl;
            } else {
                platform = Platform::Wii;
                std::cout << "[Config] Platform set to Wii." << std::endl;
            }
        }

        auto vsync = tbl["graphics"]["vsync"].value<bool>();
        if (vsync) enable_vsync = *vsync;

        auto back = tbl["graphics"]["backend"].value<std::string>();
        if (back) {
            if (*back == "Metal" || *back == "metal") {
                backend = Backend::Metal;
                std::cout << "[Config] Backend set to Metal." << std::endl;
            } else {
                backend = Backend::OpenGL;
                std::cout << "[Config] Backend set to OpenGL." << std::endl;
            }
        }

        auto w = tbl["graphics"]["window_width"].value<int>();
        if (w) window_width = *w;

        auto h = tbl["graphics"]["window_height"].value<int>();
        if (h) window_height = *h;

        if (auto v = tbl["graphics"]["internal_scale"].value<int>())
            internal_scale = std::clamp(*v, 1, 8);
        if (auto v = tbl["graphics"]["msaa"].value<int>())
            msaa = std::clamp(*v, 1, 16);
        if (auto v = tbl["graphics"]["anisotropy"].value<int>())
            anisotropy = std::clamp(*v, 1, 16);
        // aspect_ratio = "game": the game's own Widescreen option decides
        // (4:3, or its anamorphic 16:9 stretched by the presenter); "16:10",
        // "21:9", "32:9": wider than that. 4:3 and 16:9 (also from older
        // configs) mean "game".
        if (auto v = tbl["graphics"]["aspect_ratio"].value<std::string>()) {
            int w = 0, h = 0;
            if (std::sscanf(v->c_str(), "%d:%d", &w, &h) == 2 && w > 0 && h > 0 &&
                w * 3 > h * 4 && w * 9 != h * 16)
                set_aspect(w, h);
        }
        if (auto v = tbl["graphics"]["fullscreen"].value<bool>())
            fullscreen = *v;
        if (auto v = tbl["graphics"]["show_fps"].value<bool>())
            show_fps = *v;
        if (auto v = tbl["graphics"]["frame_rate"].value<int>())
            frame_rate = *v >= 120 ? 120 : *v >= 90 ? 90 : 60;
        if (auto v = tbl["audio"]["volume"].value<int>())
            volume = std::clamp(*v, 0, 100);
        if (auto v = tbl["audio"]["mute"].value<bool>())
            mute = *v;
        if (auto v = tbl["mouse"]["camera"].value<bool>())
            mouse_camera = *v;
        if (auto v = tbl["mouse"]["sensitivity"].value<int>())
            mouse_sensitivity = std::clamp(*v, 1, 100);
        if (auto v = tbl["mouse"]["invert_y"].value<bool>())
            mouse_invert_y = *v;
        if (auto v = tbl["launcher"]["enabled"].value<bool>())
            launcher = *v;
        const auto native_path = [](const std::string &u8) -> std::string {
            try {
                return std::filesystem::path(std::u8string(u8.begin(), u8.end())).string();
            } catch (const std::exception &) { return {}; } // not representable here
        };
        if (auto v = tbl["launcher"]["memcard"].value<std::string>())
            memcard_path = native_path(*v);
        if (auto *arr = tbl["launcher"]["recent"].as_array())
            for (auto &&el : *arr)
                if (auto path = el.value<std::string>()) {
                    try {
                        recent_discs.push_back(
                            std::filesystem::path(std::u8string(path->begin(), path->end())).string());
                    } catch (const std::exception &) {} // not representable here
                }
        if (auto *kb = tbl["keyboard"].as_table()) {
            for (auto &&[k, v] : *kb)
                if (auto name = v.value<std::string>();
                    !name || !input::set_key_binding(std::string(k.str()), *name))
                    std::cout << "[Config] [keyboard] " << k.str()
                              << ": unknown control or key name" << std::endl;
        }
        if (auto *pad = tbl["controller"].as_table()) {
            for (auto &&[k, v] : *pad)
                if (auto name = v.value<std::string>();
                    !name || !input::set_pad_binding(std::string(k.str()), *name))
                    std::cout << "[Config] [controller] " << k.str()
                              << ": unknown control or input name" << std::endl;
        }
        std::cout << "[Config] internal_scale " << internal_scale << "x, msaa "
                  << msaa << "x, anisotropy " << anisotropy << "x, aspect "
                  << aspect_w << ":" << aspect_h << std::endl;

        auto gid = tbl["game"]["id"].value<std::string>();
        if (gid) {
            game_id = *gid;
            std::cout << "[Config] Game ID override: " << game_id << std::endl;
        }

        auto mode = tbl["input"]["mode"].value<int>();
        if (mode) {
            input_mode = *mode;
            static const char* mode_names[] = {
                "", "wiimote-bluetooth", "gamepad-classic",
                "gamepad-pointer-assist", "gamepad-full-tilt",
                "smartphone", "keyboard-mouse", "touch"
            };
            if (input_mode >= 1 && input_mode <= 7)
                std::cout << "[Config] Input mode " << input_mode << " ("
                          << mode_names[input_mode] << ")" << std::endl;
            else
                std::cout << "[Config] Unknown input mode " << input_mode
                          << ", falling back to 6 (keyboard-mouse)" << std::endl;
        }

        auto gyro = tbl["input"]["gyro_sensitivity"].value<double>();
        if (gyro) gyro_sensitivity = (float)*gyro;

        auto pspeed = tbl["input"]["pointer_speed"].value<double>();
        if (pspeed) pointer_speed = (float)*pspeed;

        auto pport = tbl["input"]["phone_port"].value<int>();
        if (pport) phone_port = *pport;

        auto eid = tbl["hle"]["ext_interrupt_dispatch"].value<int64_t>();
        if (eid && *eid != 0) {
            game_profile.ext_interrupt_dispatch = (uint32_t)*eid;
            std::cout << "[Config] ext_interrupt_dispatch = 0x"
                      << std::hex << game_profile.ext_interrupt_dispatch
                      << std::dec << std::endl;
        }

        auto slo = tbl["hle"]["sched_lock_offset"].value<int64_t>();
        if (slo && *slo != 0) {
            game_profile.sched_lock_offset = (uint32_t)*slo;
            std::cout << "[Config] sched_lock_offset = 0x"
                      << std::hex << game_profile.sched_lock_offset
                      << std::dec << std::endl;
        }

        auto rs = tbl["hle"]["resched"].value<int64_t>();
        if (rs && *rs != 0) {
            game_profile.resched_addr = (uint32_t)*rs;
            std::cout << "[Config] resched = 0x" << std::hex
                      << game_profile.resched_addr << std::dec << std::endl;
        }

        auto wtb = tbl["hle"]["wall_timebase"].value<bool>();
        if (wtb) {
            g_wall_timebase = *wtb;
            std::cout << "[Config] wall_timebase = " << (*wtb ? "on" : "off")
                      << std::endl;
        }

    } catch (const toml::parse_error& err) {

        std::cerr << "[Config] Failed to parse config.toml:\n" << err << std::endl;
    }
}

void Config::load_game_profile() {
    
    std::vector<std::string> search_paths = {
        "assets/games/" + game_id + ".toml",
        game_dir + "/../assets/games/" + game_id + ".toml",
    };

    std::string found_path;
    for (const auto& p : search_paths) {
        if (std::filesystem::exists(p)) {
            found_path = p;
            break;
        }
    }

    if (found_path.empty()) {
        std::cout << "[Config] No game profile for " << game_id << " (using defaults)." << std::endl;
        return;
    }

    try {
        toml::table tbl = toml::parse_file(found_path);

        auto cst = tbl["quirks"]["callback_stack_top"].value<int64_t>();
        if (cst) {
            game_profile.callback_stack_top = (uint32_t)*cst;
            std::cout << "[Config] Game profile: callback_stack_top = 0x"
                      << std::hex << game_profile.callback_stack_top << std::dec << std::endl;
        }

        if (auto mods = tbl["quirks"]["disable_module"].as_array()) {
            mods->for_each([this](const toml::value<std::string>& v) {
                game_profile.disable_modules.push_back(*v);
                std::cout << "[Config] Game profile: disable_module = " << *v << std::endl;
            });
        }

        auto rpl = tbl["game"]["rpl_path"].value<std::string>();
        if (rpl) game_profile.rpl_path = *rpl;

        auto eid = tbl["hle"]["ext_interrupt_dispatch"].value<int64_t>();
        if (eid) {
            game_profile.ext_interrupt_dispatch = (uint32_t)*eid;
            std::cout << "[Config] Game profile: ext_interrupt_dispatch = 0x"
                      << std::hex << game_profile.ext_interrupt_dispatch
                      << std::dec << std::endl;
        }

        std::cout << "[Config] Loaded game profile: " << found_path << std::endl;

    } catch (const toml::parse_error& err) {
        std::cerr << "[Config] Failed to parse game profile " << found_path
                  << ":\n" << err << std::endl;
    }
}


void Config::apply_default_keyboard() const {
    namespace fs = std::filesystem;
    if (config_path.empty()) return;
    const fs::path p(config_path);
    const fs::path def = p.parent_path() / (p.stem().string() + ".default.toml");
    std::error_code ec;
    if (!fs::exists(def, ec)) return;
    try {
        const toml::table tbl = toml::parse_file(def.string());
        if (auto *kb = tbl["keyboard"].as_table())
            for (auto &&[k, v] : *kb)
                if (auto name = v.value<std::string>())
                    input::set_key_binding(std::string(k.str()), *name);
    } catch (const std::exception &) {}
}

} 
} 
