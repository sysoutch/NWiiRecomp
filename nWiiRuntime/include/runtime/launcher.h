#pragma once

// Title menu shown before the game boots: pick a disc (an .iso/.gcm/.rvz image
// or an extracted folder), recent discs, settings, controller setup, credits.
// Main thread, after ui::init (it draws through the settings overlay's ImGui).
#include <string>

struct SDL_Window;

namespace nwii::runtime::launcher {

struct DiscInfo {
    enum Kind { None, Extracted, Image, Unknown } kind = None; // Image: .iso/.gcm/.rvz
    std::string id;    // game ID from boot.bin, e.g. "GS8E7D"
    std::string title; // game name from boot.bin
    std::string error; // why it can't be booted (empty when it can)
};

// Look at a disc path without loading it. `expected_id` (may be empty) is the
// game this build was recompiled for; another game is reported as an error.
DiscInfo probe(const std::string &path, const std::string &expected_id);

// Run the menu until a disc is chosen (returned) or the user quits ("").
// `disc` is preselected (command line or the most recent disc).
std::string run(SDL_Window *window, const std::string &disc);

} // namespace nwii::runtime::launcher
