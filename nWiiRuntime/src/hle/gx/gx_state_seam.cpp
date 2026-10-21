// GX render state served at the seam.
//
// Full-register setters emit the exact BP write the SDK would have produced
// (framing: 0x61, u8 register, 24-bit value). Setters that own only part of a
// register must NOT merge here: the shadow state they would merge against is
// updated by the parser, which runs up to a frame behind this call. Those emit
// a 0x78 RMW marker instead -- mask + value -- and the parser performs the
// merge at the marker's position in the stream, where the base is current.

#include "runtime/cpu_context.h"
#include "runtime/gx_state.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iterator>

using nwii::runtime::CPUContext;

namespace {

void BPWrite(uint8_t reg, uint32_t val) {
  nwii::runtime::GX_WGPIPE_Write8(0x61);
  nwii::runtime::GX_WGPIPE_Write8(reg);
  nwii::runtime::GX_WGPIPE_Write8((val >> 16) & 0xFF);
  nwii::runtime::GX_WGPIPE_Write8((val >> 8) & 0xFF);
  nwii::runtime::GX_WGPIPE_Write8(val & 0xFF);
}

// kind 0 = BP register, 1 = CP register.
void RMWWrite(uint8_t kind, uint8_t reg, uint32_t mask, uint32_t val) {
  nwii::runtime::GX_WGPIPE_Write8(0x78);
  nwii::runtime::GX_WGPIPE_Write8(kind);
  nwii::runtime::GX_WGPIPE_Write8(reg);
  nwii::runtime::GX_WGPIPE_Write32(mask);
  nwii::runtime::GX_WGPIPE_Write32(val);
}

} // namespace

namespace nwii::runtime {
uint64_t GX_WaitDrawDoneParsed(uint64_t target, CPUContext &ctx); // gx.cpp
void GX_WaitRenderBacklog(uint64_t max_backlog, CPUContext &ctx);  // gx.cpp
void vi_request_retrace(CPUContext &ctx);                          // ios.cpp
}

extern "C" {

// void GXSetCullMode(GXCullMode mode)
// API order is none/front/back/all; GEN_MODE stores none/back/front/all, so
// 1 and 2 swap on the way in -- the same table libogc uses.
void GXSetCullMode(CPUContext &ctx) {
  static const uint8_t to_hw[4] = {0, 2, 1, 3};
  const uint32_t m = to_hw[ctx.gpr[3] & 3];
  RMWWrite(0, 0x00, 3u << 14, m << 14);
}

// void GXSetNumTevStages(u8 num) -- GEN_MODE bits 10-13 hold num-1.
void GXSetNumTevStages(CPUContext &ctx) {
  const uint32_t n = (ctx.gpr[3] - 1) & 0xF;
  RMWWrite(0, 0x00, 0xFu << 10, n << 10);
}

// void GXSetBlendMode(GXBlendMode type, GXBlendFactor src, GXBlendFactor dst,
//                     GXLogicOp op) -- BP 0x41 (PE_CMODE0). Bits 2-4 (dither,
// colour/alpha update) belong to other setters and stay untouched.
void GXSetBlendMode(CPUContext &ctx) {
  const uint32_t type = ctx.gpr[3], src = ctx.gpr[4] & 7, dst = ctx.gpr[5] & 7,
                 op = ctx.gpr[6] & 0xF;
  uint32_t v = 0;
  if (type == 1 || type == 3) v |= 1;        // blend / subtract enable
  if (type == 2) v |= 2;                     // logic op
  v |= dst << 5;
  v |= src << 8;
  if (type == 3) v |= 1u << 11;              // subtract
  v |= op << 12;
  RMWWrite(0, 0x41, 0xFFE3u, v);
}

// void GXSetZMode(GXBool compare_enable, GXCompare func, GXBool update_enable)
void GXSetZMode(CPUContext &ctx) {
  const uint32_t v =
      (ctx.gpr[3] & 1) | ((ctx.gpr[4] & 7) << 1) | ((ctx.gpr[5] & 1) << 4);
  BPWrite(0x40, v);
}

// void GXDrawDone(void) -- opt-in via [hle_hooks]. The SDK version sets the
// draw-done token (BP 0x45 = 2) and then sleeps until the PE finish
// interrupt, which here only fires once the host thread has parsed that far
// in the FIFO: CPU frame time + host parse latency + render time ran back to
// back, holding Spyro at ~25 fps. The real GPU works in parallel with the
// CPU; this lets the CPU build the next frame while the host renders this
// one. The finish interrupt still arrives later (and runs the game's
// draw-done callback, if any).
//
// Not waiting at all let the host fall several frames behind, and the parser
// reads guest RAM (LOAD_INDX matrices, indexed vertex arrays) that the game
// had already rewritten for later frames: broken bits of objects for single
// frames. So the CPU waits until the host has parsed the token from
// NWII_DRAWDONE_LAG frames ago (default 1 = one frame of overlap; 0 = full
// sync like the SDK; -1 = never wait, the old behaviour).
void GXDrawDone(CPUContext &ctx) {
  static const int lag = [] {
    const char *e = std::getenv("NWII_DRAWDONE_LAG");
    return e ? std::atoi(e) : 1;
  }();
  static uint64_t issued = 0;
  // NWII_FRAMEHIST=1: histogram of the game's frame intervals (GXDrawDone to
  // GXDrawDone, 2 ms buckets) every 5 s -- 16.7 vs 33.3 ms shows missed
  // VI retraces.
  static const bool hist = std::getenv("NWII_FRAMEHIST") != nullptr;
  // Wait at the end of the previous call: it falls inside this interval.
  static uint64_t s_last_wait_us = 0;
  if (hist) {
    using clk = std::chrono::steady_clock;
    static clk::time_point last = clk::now(), report = clk::now();
    static uint32_t buckets[24] = {};
    const auto now = clk::now();
    const double ms = std::chrono::duration<double, std::milli>(now - last).count();
    last = now;
    ++buckets[std::min(23, (int)(ms / 2.0))];
    static int slow_logged = 0;
    if (ms > 20.0 && slow_logged < 60) {
      ++slow_logged;
      std::printf("[SLOWFRAME] %.1f ms (draw-done wait in it %.1f ms)\n", ms,
                  s_last_wait_us / 1000.0);
    }
    if (now - report >= std::chrono::seconds(5)) {
      report = now;
      std::printf("[FRAMEHIST]");
      for (int i = 0; i < 24; ++i)
        if (buckets[i]) std::printf(" %d-%dms:%u", i * 2, i * 2 + 2, buckets[i]);
      std::printf("\n");
      std::fflush(stdout);
      std::fill(std::begin(buckets), std::end(buckets), 0u);
    }
  }
  BPWrite(0x45, 0x000002);
  ++issued;
  uint64_t waited = 0;
  if (lag >= 0 && issued > (uint64_t)lag)
    waited = nwii::runtime::GX_WaitDrawDoneParsed(issued - lag, ctx);
  if (hist) s_last_wait_us = waited;
  // Keep the game at most NWII_RENDER_BACKLOG drawn frames (default 1) ahead
  // of the render thread instead of letting the renderer drop frames.
  // -1 = no back-pressure (frames are dropped when rendering falls behind).
  static const int backlog = [] {
    const char *e = std::getenv("NWII_RENDER_BACKLOG");
    return e ? std::atoi(e) : 1;
  }();
  if (backlog >= 0) nwii::runtime::GX_WaitRenderBacklog((uint64_t)backlog, ctx);
  // End of a game frame: the game's VIFlush + VIWaitForRetrace follow.
  nwii::runtime::vi_request_retrace(ctx);
}

// void GXSetZCompLoc(GXBool before_tex) -- BP 0x43 bit 6.
void GXSetZCompLoc(CPUContext &ctx) {
  RMWWrite(0, 0x43, 1u << 6, (ctx.gpr[3] & 1) << 6);
}

// void GXSetAlphaCompare(GXCompare c0, u8 ref0, GXAlphaOp op, GXCompare c1,
//                        u8 ref1) -- BP 0xF3, full register.
void GXSetAlphaCompare(CPUContext &ctx) {
  const uint32_t v = (ctx.gpr[4] & 0xFF) | ((ctx.gpr[7] & 0xFF) << 8) |
                     ((ctx.gpr[3] & 7) << 16) | ((ctx.gpr[6] & 7) << 19) |
                     ((ctx.gpr[5] & 3) << 22);
  BPWrite(0xF3, v);
}

// void GXSetDstAlpha(GXBool enable, u8 a) -- BP 0x42, full register.
void GXSetDstAlpha(CPUContext &ctx) {
  BPWrite(0x42, (ctx.gpr[4] & 0xFF) | ((ctx.gpr[3] & 1) << 8));
}

// void GXSetScissor(u32 x, u32 y, u32 wd, u32 ht)
// Scissor coordinates carry the hardware's 342 pixel origin offset.
void GXSetScissor(CPUContext &ctx) {
  const uint32_t xo = ctx.gpr[3] + 342, yo = ctx.gpr[4] + 342;
  const uint32_t xe = xo + ctx.gpr[5] - 1, ye = yo + ctx.gpr[6] - 1;
  BPWrite(0x20, (yo & 0x7FF) | ((xo & 0x7FF) << 12));
  BPWrite(0x21, (ye & 0x7FF) | ((xe & 0x7FF) << 12));
}

} // extern "C"
