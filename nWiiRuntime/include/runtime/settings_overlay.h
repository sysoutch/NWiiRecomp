#pragma once

// In-game PC settings overlay (Dear ImGui), toggled with F1 or the
// controller's Back/View button. Changes apply live and are saved back into
// the game's TOML (comments and layout kept). Main-thread / GL-context only.
union SDL_Event;
struct SDL_Window;

namespace nwii::runtime::ui {

// What the host loop must do after ui::frame(), because it owns the window,
// GL objects and the main loop.
struct Requests {
    bool rebuild_efb = false; // internal scale / MSAA / widescreen changed
    bool vsync = false;       // re-apply the swap interval
    bool fullscreen = false;  // re-apply the window mode
    bool quit = false;
};

void init(SDL_Window *window, void *gl_context, int max_msaa, int max_anisotropy);
void shutdown();

// Feed every SDL event. True when the overlay consumed it (e.g. a key press
// while the menu is open or a hotkey); the host still handles SDL_QUIT.
bool handle_event(const SDL_Event &e);

bool is_open();

// Open the settings window on a tab (0 Graphics, 1 Audio, 2 Controls).
void open_settings(int tab);

// Launcher mode (pre-game title menu): keyboard/gamepad navigation always on,
// the settings window says "SETTINGS" and closes with Back instead of
// offering Quit / Resume.
void set_launcher_mode(bool on);

// Build and draw the overlay into the default framebuffer (call after the
// game image is presented, before swapping). Also draws the FPS counter.
// `underlay` (optional) adds more UI each frame, below the settings window.
void frame(void (*underlay)() = nullptr);

Requests take_requests();

// Write the current settings into Config::config_path.
bool save_settings();

} // namespace nwii::runtime::ui
