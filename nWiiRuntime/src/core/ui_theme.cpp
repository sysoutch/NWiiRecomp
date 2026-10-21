#include "ui_theme.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <vector>

namespace nwii::runtime::ui::theme {

ImFont *banner_font = nullptr;
ImFont *title_font = nullptr;

// Triangle fan with a colour gradient from `centre` to the outline points.
void fan_gradient(ImDrawList *dl, ImVec2 centre, ImU32 col_c, const ImVec2 *pts, int n,
                  ImU32 col_edge) {
    const ImVec2 uv = ImGui::GetFontTexUvWhitePixel();
    dl->PrimReserve(n * 3, n + 1);
    const ImDrawIdx base = (ImDrawIdx)dl->_VtxCurrentIdx;
    dl->PrimWriteVtx(centre, uv, col_c);
    for (int i = 0; i < n; ++i)
        dl->PrimWriteVtx(pts[i], uv, col_edge);
    for (int i = 0; i < n; ++i) {
        dl->PrimWriteIdx(base);
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + i));
        dl->PrimWriteIdx((ImDrawIdx)(base + 1 + (i + 1) % n));
    }
}

// Four-point sparkle.
void draw_sparkle(ImDrawList *dl, ImVec2 c, float r, ImU32 col) {
    const float w = r * 0.22f;
    dl->AddQuadFilled(ImVec2(c.x, c.y - r), ImVec2(c.x + w, c.y), ImVec2(c.x, c.y + r),
                      ImVec2(c.x - w, c.y), col);
    dl->AddQuadFilled(ImVec2(c.x - r, c.y), ImVec2(c.x, c.y - w), ImVec2(c.x + r, c.y),
                      ImVec2(c.x, c.y + w), col);
}

// A cut gem (crown + pavilion, front view) in a gold setting.
void draw_gem(ImDrawList *dl, ImVec2 c, float r) {
    auto P = [&](float x, float y) { return ImVec2(c.x + x * r, c.y + y * r); };
    const ImVec2 L = P(-1.0f, -0.2f), TL = P(-0.62f, -0.78f), TR = P(0.62f, -0.78f),
                 R = P(1.0f, -0.2f), B = P(0.0f, 1.15f), G1 = P(-0.36f, -0.2f),
                 G2 = P(0.36f, -0.2f);
    const ImVec2 outline[] = {L, TL, TR, R, B};
    // Soft glow and the gold setting behind the stone.
    ImVec2 ring[24];
    for (int i = 0; i < 24; ++i) {
        const float ang = i * (6.2831853f / 24);
        ring[i] = P(std::cos(ang) * 1.7f, 0.1f + std::sin(ang) * 1.7f);
    }
    fan_gradient(dl, P(0.0f, 0.1f), IM_COL32(210, 150, 255, 110), ring, 24,
                 IM_COL32(210, 150, 255, 0));
    dl->AddPolyline(outline, 5, IM_COL32(40, 14, 60, 255), ImDrawFlags_Closed, r * 0.42f);
    dl->AddPolyline(outline, 5, kGoldDark, ImDrawFlags_Closed, r * 0.30f);
    dl->AddPolyline(outline, 5, kGold, ImDrawFlags_Closed, r * 0.18f);
    // Facets: crown (top band) lit, pavilion (lower point) shaded.
    dl->AddTriangleFilled(L, TL, G1, IM_COL32(190, 125, 250, 255));
    dl->AddQuadFilled(TL, TR, G2, G1, IM_COL32(228, 190, 255, 255)); // table
    dl->AddTriangleFilled(TR, R, G2, IM_COL32(160, 90, 235, 255));
    dl->AddTriangleFilled(L, G1, B, IM_COL32(120, 50, 200, 255));
    dl->AddTriangleFilled(G1, G2, B, IM_COL32(165, 95, 240, 255));
    dl->AddTriangleFilled(G2, R, B, IM_COL32(82, 28, 150, 255));
    // Facet edges.
    const ImU32 edge = IM_COL32(245, 225, 255, 110);
    const float ew = std::max(1.0f, r * 0.05f);
    dl->AddLine(L, R, edge, ew);
    dl->AddLine(TL, G1, edge, ew);
    dl->AddLine(TR, G2, edge, ew);
    dl->AddLine(G1, B, edge, ew);
    dl->AddLine(G2, B, edge, ew);
    dl->AddPolyline(outline, 5, kGoldLight, ImDrawFlags_Closed, std::max(1.0f, r * 0.06f));
    draw_sparkle(dl, P(-0.32f, -0.5f), r * 0.42f, IM_COL32(255, 255, 255, 230));
}

// Quadratic Bezier from a to b (control q) appended to `out`, excluding a.
void quad_bezier(std::vector<ImVec2> &out, ImVec2 a, ImVec2 q, ImVec2 b, int steps) {
    for (int i = 1; i <= steps; ++i) {
        const float t = (float)i / steps, u = 1.0f - t;
        out.push_back(ImVec2(u * u * a.x + 2 * u * t * q.x + t * t * b.x,
                             u * u * a.y + 2 * u * t * q.y + t * t * b.y));
    }
}

// A tapered bone from a (width w) to the point b.
void draw_bone(ImDrawList *dl, ImVec2 a, ImVec2 b, float w, ImU32 col) {
    const float dx = b.x - a.x, dy = b.y - a.y, len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    const float nx = -dy / len * w * 0.5f, ny = dx / len * w * 0.5f;
    dl->AddTriangleFilled(ImVec2(a.x + nx, a.y + ny), ImVec2(a.x - nx, a.y - ny), b, col);
    dl->AddCircleFilled(a, w * 0.5f, col, 12);
}

// A dragon wing growing out of `root` towards `dir` (-1 left, +1 right): an
// arm curving up to the wrist, four fingers fanning out, and a scalloped
// membrane between them shaded from the wrist outwards.
void draw_wing(ImDrawList *dl, ImVec2 root, float s, float dir) {
    auto P = [&](float x, float y) { return ImVec2(root.x + dir * x * s, root.y + y * s); };
    const ImVec2 wrist = P(1.7f, -2.0f);
    const ImVec2 tips[] = {P(4.5f, -2.9f), P(4.9f, -1.2f), P(3.9f, 0.2f), P(2.3f, 0.9f)};
    const ImVec2 tail = P(0.4f, 0.55f);
    // Outline: root -> arm -> wrist -> top tip -> scallops -> tail.
    std::vector<ImVec2> out;
    out.push_back(root);
    quad_bezier(out, root, P(0.4f, -1.9f), wrist, 8);
    quad_bezier(out, wrist, P(3.0f, -3.1f), tips[0], 6);
    auto scallop = [&](ImVec2 a, ImVec2 b) {
        // Control pulled towards the wrist so each edge bows inwards.
        const ImVec2 m((a.x + b.x) * 0.5f, (a.y + b.y) * 0.5f);
        quad_bezier(out, a, ImVec2(m.x + (wrist.x - m.x) * 0.38f, m.y + (wrist.y - m.y) * 0.38f),
                    b, 8);
    };
    for (int i = 0; i + 1 < 4; ++i)
        scallop(tips[i], tips[i + 1]);
    scallop(tips[3], tail);
    const int n = (int)out.size();
    // Membrane: lighter near the wrist, deep purple at the edges.
    fan_gradient(dl, wrist, IM_COL32(170, 100, 235, 255), out.data(), n,
                 IM_COL32(72, 26, 128, 255));
    // Fingers and arm.
    const ImU32 bone = IM_COL32(214, 160, 80, 255);
    for (const ImVec2 &t : tips)
        draw_bone(dl, wrist, t, s * 0.22f, bone);
    draw_bone(dl, root, wrist, s * 0.42f, bone);
    // Outline: dark rim, then gold.
    dl->AddPolyline(out.data(), n, IM_COL32(40, 14, 60, 255), ImDrawFlags_Closed, s * 0.26f);
    dl->AddPolyline(out.data(), n, kGold, ImDrawFlags_Closed, std::max(1.5f, s * 0.12f));
    // Wrist knuckle.
    dl->AddCircleFilled(wrist, s * 0.26f, IM_COL32(40, 14, 60, 255), 12);
    dl->AddCircleFilled(wrist, s * 0.2f, kGold, 12);
    dl->AddCircleFilled(ImVec2(wrist.x - dir * s * 0.06f, wrist.y - s * 0.07f), s * 0.08f,
                        kGoldLight, 8);
}

// A spiral scroll: starts at `p`, runs along `d` and curls towards `n`.
void draw_scroll(ImDrawList *dl, ImVec2 p, ImVec2 d, ImVec2 n, float s) {
    std::vector<ImVec2> pts;
    pts.push_back(p);
    // Straight run, then about 1.4 turns of a shrinking spiral.
    const ImVec2 c(p.x + d.x * s * 1.6f + n.x * s * 0.55f, p.y + d.y * s * 1.6f + n.y * s * 0.55f);
    const float a0 = std::atan2(-n.y, -n.x);
    const float turn = (d.x * n.y - d.y * n.x) > 0 ? 1.0f : -1.0f;
    for (int i = 0; i <= 28; ++i) {
        const float t = i / 28.0f;
        const float a = a0 + turn * t * 2.8f * 3.14159f;
        const float rr = s * 0.55f * (1.0f - 0.65f * t);
        pts.push_back(ImVec2(c.x + std::cos(a) * rr, c.y + std::sin(a) * rr));
    }
    dl->AddPolyline(pts.data(), (int)pts.size(), IM_COL32(40, 14, 60, 255), 0, s * 0.30f);
    dl->AddPolyline(pts.data(), (int)pts.size(), kGold, 0, s * 0.16f);
    dl->AddCircleFilled(pts.back(), s * 0.14f, kGoldLight, 8);
}

// A bevelled gold band along a rounded rect: dark rim, gold body, light outer
// edge and a thin dark inner edge.
void draw_gold_band(ImDrawList *dl, ImVec2 a, ImVec2 b, float rounding, float w) {
    dl->AddRect(a, b, IM_COL32(30, 10, 50, 255), rounding, 0, w * 1.45f);
    dl->AddRect(a, b, kGoldDark, rounding, 0, w * 1.1f);
    dl->AddRect(a, b, kGold, rounding, 0, w * 0.7f);
    const float h = w * 0.28f;
    dl->AddRect(ImVec2(a.x - h, a.y - h), ImVec2(b.x + h, b.y + h), kGoldLight, rounding + h, 0,
                std::max(1.0f, w * 0.18f));
    dl->AddRect(ImVec2(a.x + h, a.y + h), ImVec2(b.x - h, b.y - h), IM_COL32(120, 70, 20, 255),
                std::max(0.0f, rounding - h), 0, std::max(1.0f, w * 0.12f));
}

// Text with a dark outline (banner title).
void outlined_text(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col,
                   const char *text) {
    const float o = std::max(1.5f, size * 0.05f);
    for (int dx = -1; dx <= 1; ++dx)
        for (int dy = -1; dy <= 1; ++dy)
            if (dx || dy)
                dl->AddText(font, size, ImVec2(pos.x + dx * o, pos.y + dy * o),
                            IM_COL32(50, 20, 70, 255), text);
    dl->AddText(font, size, ImVec2(pos.x, pos.y + o * 0.6f), kGoldDark, text);
    dl->AddText(font, size, pos, col, text);
}

void draw_menu_frame(ImVec2 pos, ImVec2 size, const char *title) {
    const float fs = ImGui::GetFontSize();
    const float r = fs * 0.9f; // corner rounding
    ImDrawList *bg = ImGui::GetBackgroundDrawList();
    ImDrawList *fg = ImGui::GetForegroundDrawList();
    const ImVec2 a = pos, b(pos.x + size.x, pos.y + size.y);

    // Panel: rounded purple base plus a vertical gradient inside.
    bg->AddRectFilled(a, b, kPurpleBottom, r);
    const float in = r * 0.6f;
    bg->AddRectFilledMultiColor(ImVec2(a.x + in, a.y + in), ImVec2(b.x - in, b.y - in), kPurpleTop,
                                kPurpleTop, kPurpleBottom, kPurpleBottom);

    // Gold band border with an inner hairline.
    draw_gold_band(fg, a, b, r, fs * 0.24f);
    fg->AddRect(ImVec2(a.x + fs * 0.5f, a.y + fs * 0.5f), ImVec2(b.x - fs * 0.5f, b.y - fs * 0.5f),
                IM_COL32(236, 186, 74, 150), r * 0.6f, 0, std::max(1.0f, fs * 0.06f));

    // Scrolls at the corners, curling into the panel along both edges.
    const float sc = fs * 0.62f, e = fs * 0.5f;
    draw_scroll(fg, ImVec2(a.x + e * 2.4f, a.y + e), ImVec2(1, 0), ImVec2(0, 1), sc);
    draw_scroll(fg, ImVec2(b.x - e * 2.4f, a.y + e), ImVec2(-1, 0), ImVec2(0, 1), sc);
    draw_scroll(fg, ImVec2(a.x + e, a.y + e * 2.4f), ImVec2(0, 1), ImVec2(1, 0), sc);
    draw_scroll(fg, ImVec2(b.x - e, a.y + e * 2.4f), ImVec2(0, 1), ImVec2(-1, 0), sc);
    draw_scroll(fg, ImVec2(a.x + e, b.y - e * 3.2f), ImVec2(0, -1), ImVec2(1, 0), sc);
    draw_scroll(fg, ImVec2(b.x - e, b.y - e * 3.2f), ImVec2(0, -1), ImVec2(-1, 0), sc);

    // Gems: all four corners, the bottom centre and the side midpoints.
    draw_gem(fg, ImVec2(a.x + fs * 0.25f, a.y + fs * 0.25f), fs * 0.5f);
    draw_gem(fg, ImVec2(b.x - fs * 0.25f, a.y + fs * 0.25f), fs * 0.5f);
    draw_gem(fg, ImVec2(a.x + fs * 0.3f, b.y - fs * 0.3f), fs * 0.72f);
    draw_gem(fg, ImVec2(b.x - fs * 0.3f, b.y - fs * 0.3f), fs * 0.72f);
    draw_gem(fg, ImVec2((a.x + b.x) * 0.5f, b.y + fs * 0.05f), fs * 0.8f);
    draw_gem(fg, ImVec2(a.x, (a.y + b.y) * 0.5f), fs * 0.42f);
    draw_gem(fg, ImVec2(b.x, (a.y + b.y) * 0.5f), fs * 0.42f);

    if (!title) return;
    // Banner straddling the top edge, with wings and a gem.
    const float bw = std::min(size.x * 0.56f, fs * 17.0f), bh = fs * 2.7f;
    const float cx = (a.x + b.x) * 0.5f;
    const ImVec2 ba(cx - bw * 0.5f, a.y - bh * 0.62f), bb(cx + bw * 0.5f, a.y + bh * 0.38f);
    draw_wing(fg, ImVec2(ba.x + fs * 0.3f, (ba.y + bb.y) * 0.5f + fs * 0.2f), fs * 1.05f, -1.0f);
    draw_wing(fg, ImVec2(bb.x - fs * 0.3f, (ba.y + bb.y) * 0.5f + fs * 0.2f), fs * 1.05f, 1.0f);
    fg->AddRectFilled(ba, bb, IM_COL32(36, 12, 70, 255), fs * 0.5f);
    fg->AddRectFilledMultiColor(ImVec2(ba.x + fs * 0.3f, ba.y + fs * 0.3f),
                                ImVec2(bb.x - fs * 0.3f, bb.y - fs * 0.3f),
                                IM_COL32(92, 44, 150, 255), IM_COL32(92, 44, 150, 255),
                                IM_COL32(40, 14, 76, 255), IM_COL32(40, 14, 76, 255));
    draw_gold_band(fg, ba, bb, fs * 0.5f, fs * 0.22f);
    draw_gem(fg, ImVec2(cx, ba.y - fs * 0.2f), fs * 0.85f);
    ImFont *font = banner_font ? banner_font : ImGui::GetFont();
    const float ts = banner_font ? banner_font->FontSize : fs * 2.0f;
    const ImVec2 tsz = font->CalcTextSizeA(ts, FLT_MAX, 0.0f, title);
    outlined_text(fg, font, ts, ImVec2(cx - tsz.x * 0.5f, (ba.y + bb.y) * 0.5f - tsz.y * 0.5f),
                  kGoldLight, title);
}

} // namespace nwii::runtime::ui::theme
