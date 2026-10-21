// Native memcpy / __fill_mem for the MSL C library (opt-in via [hle_hooks]).
//
// The recompiled MSL routines copy one byte per guest loop iteration --
// several host instructions, two bounds-checked MMU accesses and a CR update
// per byte. They dominated Spyro's CPU profile (meshes, display lists and
// streamed data are copied every frame). Plain-RAM ranges are copied with the
// host's memmove; anything touching hardware registers or the GX write-gather
// pipe falls back to byte accesses through the MMU, exactly like the guest
// code would.

#include "runtime/cpu_context.h"

#include <cstring>

using nwii::runtime::CPUContext;

namespace {

// Host pointer for [addr, addr + n) when the whole range is plain RAM.
uint8_t *ram_range(CPUContext &ctx, uint32_t addr, uint32_t n) {
  if (n == 0) return nullptr;
  const uint32_t p = addr & 0x3FFFFFFF;
  if (ctx.mmu.is_hw_reg(p) || (p & 0xFFFFF000u) == 0x0C008000) return nullptr;
  uint8_t *a = ctx.mmu.get_ptr(addr);
  uint8_t *b = ctx.mmu.get_ptr(addr + n - 1);
  // Same backing block and contiguous (get_ptr maps MEM1/MEM2 linearly).
  return (a && b && b - a == (ptrdiff_t)(n - 1)) ? a : nullptr;
}

bool watched(uint32_t addr, uint32_t n) {
  if (!nwii::runtime::g_watch_addr) return false;
  const uint32_t w = nwii::runtime::g_watch_addr & 0x3FFFFFFF, p = addr & 0x3FFFFFFF;
  return w + 4 > p && w < p + n;
}

} // namespace

extern "C" {

// void *memcpy(void *dst, const void *src, size_t n) -- MSL's version copies
// backwards when src < dst, i.e. it is overlap-safe like memmove.
void nwii_memcpy(CPUContext &ctx) {
  const uint32_t dst = ctx.gpr[3], src = ctx.gpr[4], n = ctx.gpr[5];
  uint8_t *d = ram_range(ctx, dst, n), *s = ram_range(ctx, src, n);
  if (d && s && !watched(dst, n)) {
    std::memmove(d, s, n);
  } else if (n) {
    if (src < dst && dst < src + n) {
      for (uint32_t i = n; i-- > 0;) ctx.mmu.write8(dst + i, ctx.mmu.read8(src + i));
    } else {
      for (uint32_t i = 0; i < n; ++i) ctx.mmu.write8(dst + i, ctx.mmu.read8(src + i));
    }
  }
  // r3 (the return value) is still dst.
}

// void __fill_mem(void *dst, int val, size_t n) -- memset's worker.
void nwii_fill_mem(CPUContext &ctx) {
  const uint32_t dst = ctx.gpr[3], n = ctx.gpr[5];
  const uint8_t v = (uint8_t)ctx.gpr[4];
  uint8_t *d = ram_range(ctx, dst, n);
  if (d && !watched(dst, n)) {
    std::memset(d, v, n);
  } else {
    for (uint32_t i = 0; i < n; ++i) ctx.mmu.write8(dst + i, v);
  }
}

} // extern "C"
