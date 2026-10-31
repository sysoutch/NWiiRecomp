#pragma once
#include <cstdint>

// The host render target standing in for the GameCube EFB (640x480). It is
// rendered at `scale` times the native size, optionally multisampled and
// optionally widened to 16:9. Game-side coordinates (viewports, EFB copies,
// readbacks) stay in native 640x480 units with a top-left origin and are
// mapped to host pixels with sx/sy (GL itself is bottom-left origin).
namespace nwii::runtime::efb {

extern int scale;            // internal resolution multiplier ([graphics] internal_scale)
extern int samples;          // MSAA samples, 1 = off ([graphics] msaa)
extern bool widescreen;      // EFB wider than 4:3 ([graphics] aspect_ratio)
extern float wide;           // its width relative to 4:3 (1 = 4:3, 4/3 = 16:9)
// 3D x scale on top of 1/wide: the game's own widescreen option scales x by
// 0.6633 (Spyro: 1.42815 -> 0.947307) where 16:9 alone would be 3/4.
constexpr float kSceneWiden = 0.8844f;
extern int width, height;    // host size
extern float sx, sy;         // host pixels per native EFB unit
extern unsigned draw_fbo;    // what GX draws into (multisampled when samples > 1)
extern unsigned resolve_fbo; // single-sample colour texture for copies/present

// Source rectangle (native units) of the last EFB-to-XFB copy: what the game
// actually displays. Presented with its display aspect ratio.
extern int xfb_x, xfb_y, xfb_w, xfb_h;

// (Re)build the render targets from Config (internal_scale, msaa,
// widescreen). Main thread with the GL context current.
void create();

// Bring resolve_fbo up to date with draw_fbo (a no-op without MSAA). GL
// framebuffer bindings are left as draw_fbo for both read and draw.
void resolve();

// Display buffer: the EFB as it was at the game's last EFB->XFB copy. The
// window and screenshots present this, never the EFB being drawn.
extern unsigned display_fbo;
void snapshot(uint32_t dest);
void select_display(uint32_t vi_tfbl);

// The framebuffer to read pixels from after resolve().
inline unsigned read_fbo() { return samples > 1 ? resolve_fbo : draw_fbo; }

// Native rect (top-left origin) -> host GL rect (bottom-left origin).
struct Rect { int x, y, w, h; };
inline Rect to_host(float x, float y, float w, float h) {
    const int hx = (int)(x * sx + 0.5f), hw = (int)(w * sx + 0.5f);
    const int hh = (int)(h * sy + 0.5f);
    const int hy = height - (int)((y + h) * sy + 0.5f);
    return {hx, hy, hw, hh};
}

} // namespace nwii::runtime::efb
