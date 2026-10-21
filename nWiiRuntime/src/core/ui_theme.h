#pragma once

// Drawing helpers for the themed menus (pause/settings overlay, launcher):
// gold trim, cut gems, dragon wings and framed panels, all built from ImGui
// draw lists -- no image assets -- and scaled by the font size, so they stay
// crisp at any window size.
#include <imgui.h>

namespace nwii::runtime::ui::theme {

constexpr ImU32 kGold = IM_COL32(236, 186, 74, 255);
constexpr ImU32 kGoldDark = IM_COL32(150, 98, 24, 255);
constexpr ImU32 kGoldLight = IM_COL32(255, 230, 150, 255);
constexpr ImU32 kPurpleTop = IM_COL32(86, 44, 142, 250);
constexpr ImU32 kPurpleBottom = IM_COL32(40, 16, 74, 250);
constexpr ImU32 kInk = IM_COL32(40, 14, 60, 255); // dark outline

extern ImFont *banner_font; // heavy weight, ~2.6x the UI font (banners)
extern ImFont *title_font;  // heavy weight, ~5x (launcher title)

// Triangle fan with a colour gradient from `centre` to the outline points.
void fan_gradient(ImDrawList *dl, ImVec2 centre, ImU32 col_c, const ImVec2 *pts, int n,
                  ImU32 col_edge);
void draw_sparkle(ImDrawList *dl, ImVec2 c, float r, ImU32 col);
void draw_gem(ImDrawList *dl, ImVec2 c, float r);
// A dragon wing growing out of `root` towards `dir` (-1 left, +1 right).
void draw_wing(ImDrawList *dl, ImVec2 root, float s, float dir);
// A spiral scroll: starts at `p`, runs along `d` and curls towards `n`.
void draw_scroll(ImDrawList *dl, ImVec2 p, ImVec2 d, ImVec2 n, float s);
void draw_gold_band(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, float w);
void outlined_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col,
                   const char *text);
// Purple panel with gold border, gems, scrolls and a winged `title` banner
// straddling the top edge (behind / in front of the window at pos, size).
// No banner when `title` is null.
void draw_menu_frame(ImVec2 pos, ImVec2 size, const char *title);

} // namespace nwii::runtime::ui::theme
