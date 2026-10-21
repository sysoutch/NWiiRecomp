#pragma once
#include <array>
#include <cmath>
#include <chrono>
#include <csetjmp>
#include <cstdint>
#include <cstring>
#include <memory>
#include <iostream>
#include <vector>
#include <mutex>
#include <atomic>
#include <queue>
#include <stack>

// Cheap monotonic cycle counter for the interrupt-poll gate (x86 TSC). Other
// hosts return 0, which makes the gate always open (old behaviour).
#if defined(_M_X64) || defined(__x86_64__)
#if defined(_MSC_VER)
#include <intrin.h>
#else
#include <x86intrin.h>
#endif
static inline uint64_t nwii_cycles() { return __rdtsc(); }
#else
static inline uint64_t nwii_cycles() { return 0; }
#endif

namespace nwii::runtime {
extern bool g_wall_timebase; // defined in config.cpp
// NWII_VERBOSE=1: developer chatter (dispatcher/heartbeat/interrupt traces,
// every unhandled MMIO access). Off by default -- console output is slow and
// the flood made the game crawl with a visible console window.
extern bool g_verbose;

// fctiw/fctiwz: saturate like Gekko (NaN and too-negative -> INT_MIN, too
// positive -> INT_MAX). A plain cast is UB and x86 gives INT_MIN for both
// overflow directions.
inline uint32_t ppc_cvt_s32(double v) {
  if (std::isnan(v) || v <= -2147483648.0) return 0x80000000u;
  if (v >= 2147483647.0) return 0x7FFFFFFFu;
  return (uint32_t)(int32_t)v;
}
// stfiwx stores an FPR's raw low word (fctiw* results live there as bits).
inline uint32_t fpr_low_word(double v) {
  uint64_t bits;
  std::memcpy(&bits, &v, 8);
  return (uint32_t)bits;
}
}

namespace nwii {
namespace runtime {

struct ConditionField {
  bool lt; 
  bool gt; 
  bool eq; 
  bool so; 

  ConditionField() : lt(false), gt(false), eq(false), so(false) {}
};

void GX_WGPIPE_Write8(uint8_t val);
void GX_WGPIPE_Write16(uint16_t val);
void GX_WGPIPE_Write32(uint32_t val);
void GX_DL_Snapshot(uint32_t addr, uint32_t size);
std::shared_ptr<const std::vector<uint8_t>> GX_DL_Fetch(uint32_t addr, uint32_t size);
void GX_WGPIPE_WriteF32(float val);
void GX_WGPIPE_WriteF64(double val);

uint16_t HW_Reg_Read16(uint32_t addr);
uint32_t HW_Reg_Read32(uint32_t addr);
void HW_Reg_Write16(uint32_t addr, uint16_t val);
void HW_Reg_Write32(uint32_t addr, uint32_t val);

struct CPUContext;
void init_ipc_client(CPUContext &ctx);
bool handle_syscall(CPUContext &ctx);
bool process_pending_callbacks(CPUContext &ctx);

struct CallbackInfo {
    uint32_t cb_addr;
    uint32_t arg1;
    uint32_t arg2;
    bool is_irq;
};



void watch_hit(uint32_t addr, uint32_t value, int width);
extern uint32_t g_watch_addr;

// Generated function definitions take CPUContext& NWII_RESTRICT ctx (see
// the recompiler's emit_function). -DNWII_NO_RESTRICT turns it off.
#if defined(NWII_NO_RESTRICT)
#define NWII_RESTRICT
#elif defined(_MSC_VER) || defined(__GNUC__) || defined(__clang__)
#define NWII_RESTRICT __restrict
#else
#define NWII_RESTRICT
#endif

#if defined(_MSC_VER)
#include <stdlib.h>
#define NWII_FORCEINLINE __forceinline
#define NWII_NOINLINE __declspec(noinline)
static inline uint16_t nwii_bswap16(uint16_t v) { return _byteswap_ushort(v); }
static inline uint32_t nwii_bswap32(uint32_t v) { return _byteswap_ulong(v); }
static inline uint64_t nwii_bswap64(uint64_t v) { return _byteswap_uint64(v); }
#else
#define NWII_FORCEINLINE inline __attribute__((always_inline))
#define NWII_NOINLINE __attribute__((noinline))
static inline uint16_t nwii_bswap16(uint16_t v) { return __builtin_bswap16(v); }
static inline uint32_t nwii_bswap32(uint32_t v) { return __builtin_bswap32(v); }
static inline uint64_t nwii_bswap64(uint64_t v) { return __builtin_bswap64(v); }
#endif

// Cycles between full interrupt checks at loop back-edges (NWII_POLL_US,
// default 20 us; 0 = every 16th back-edge as before). Calibrated at startup.
extern uint64_t g_poll_cycles;

// Strict Dolphin-accurate MMU
struct MMU {
  std::vector<uint8_t> mem1;
  std::vector<uint8_t> mem2;

  MMU() {
    mem1.resize(24 * 1024 * 1024 + 8, 0); 
    mem2.resize(64 * 1024 * 1024 + 8, 0); 
  }

  inline uint8_t *get_ptr(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF; 
    if (paddr < mem1.size() - 8) {
        return &mem1[paddr];
    } else if (paddr >= 0x10000000 && paddr < 0x10000000 + mem2.size() - 8) {
        return &mem2[paddr - 0x10000000];
    }
    return nullptr;
  }

  inline bool is_hw_reg(uint32_t paddr) const {
      return (paddr >= 0x0C000000 && paddr < 0x0C010000) || 
             (paddr >= 0x0D000000 && paddr < 0x0D010000);
  }

  NWII_NOINLINE uint8_t read8_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;

    if (is_hw_reg(paddr)) {
        
        uint32_t val = HW_Reg_Read32(paddr & ~3);
        int shift = 24 - ((paddr & 3) * 8);
        return (val >> shift) & 0xFF;
    }
    uint8_t *ptr = get_ptr(addr);
    return ptr ? *ptr : 0;
  }

  void custom_watch(uint32_t, uint32_t, int) {}

  NWII_NOINLINE void write8_slow(uint32_t addr, uint8_t value) {
    custom_watch(addr, value, 8);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 8);

    // The whole 0x0C008xxx page mirrors the write-gather pipe (Dolphin

    
    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write8(value); return; } 
    if (is_hw_reg(paddr)) return; 

    uint8_t *ptr = get_ptr(addr);
    if (ptr) *ptr = value;
  }

  NWII_NOINLINE uint16_t read16_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;

    if (is_hw_reg(paddr)) return HW_Reg_Read16(paddr);
    
    uint8_t *ptr = get_ptr(addr);
    if (ptr) return (ptr[0] << 8) | ptr[1];
    return 0;
  }

  NWII_NOINLINE void write16_slow(uint32_t addr, uint16_t value) {
    custom_watch(addr, value, 16);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 16);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write16(value); return; } 
    if (is_hw_reg(paddr)) { HW_Reg_Write16(paddr, value); return; }

    uint8_t *ptr = get_ptr(addr);
    if (ptr) { ptr[0] = value >> 8; ptr[1] = value & 0xFF; }
  }

  NWII_NOINLINE uint32_t read32_slow(uint32_t addr) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (paddr == 0x0000000C) return 0; 

    if (is_hw_reg(paddr)) return HW_Reg_Read32(paddr);

    uint8_t *ptr = get_ptr(addr);
    if (!ptr) return 0;
    uint32_t val = ((uint32_t)ptr[0] << 24) | ((uint32_t)ptr[1] << 16) | ((uint32_t)ptr[2] << 8) | ptr[3];

    return val;
  }

  NWII_NOINLINE void write32_slow(uint32_t addr, uint32_t value) {
    custom_watch(addr, value, 32);
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (paddr == 0x0000000C) return; 
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, value, 32);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_Write32(value); return; }
    if (is_hw_reg(paddr)) { HW_Reg_Write32(paddr, value); return; }

    uint8_t *ptr = get_ptr(addr);
    if (ptr) { ptr[0] = value >> 24; ptr[1] = (value >> 16) & 0xFF; ptr[2] = (value >> 8) & 0xFF; ptr[3] = value & 0xFF; }
  }

  NWII_FORCEINLINE float read_f32(uint32_t addr) { uint32_t val = read32(addr); float f; std::memcpy(&f, &val, 4); return f; }
  NWII_NOINLINE void write_f32_slow(uint32_t addr, float value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    
    uint32_t v;
    std::memcpy(&v, &value, sizeof(float));
    if (g_watch_addr && (paddr & ~3u) == (g_watch_addr & 0x3FFFFFFC))
      watch_hit(addr, v, 32);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_WriteF32(value); return; } 

    uint8_t *ptr = get_ptr(addr);
    if (ptr) {
      ptr[0] = v >> 24; ptr[1] = (v >> 16) & 0xFF; ptr[2] = (v >> 8) & 0xFF; ptr[3] = v & 0xFF;
    }
  }

  NWII_NOINLINE void write_f64_slow(uint32_t addr, double value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    
    uint64_t v;
    std::memcpy(&v, &value, sizeof(double));
    if (g_watch_addr && (paddr & ~7u) == (g_watch_addr & 0x3FFFFFF8))
      watch_hit(addr, v >> 32, 64);

    if ((paddr & 0xFFFFF000u) == 0x0C008000) { GX_WGPIPE_WriteF64(value); return; } 

    uint8_t *ptr = get_ptr(addr);
    if (ptr) {
      ptr[0] = v >> 56; ptr[1] = (v >> 48) & 0xFF; ptr[2] = (v >> 40) & 0xFF; ptr[3] = (v >> 32) & 0xFF;
      ptr[4] = (v >> 24) & 0xFF; ptr[5] = (v >> 16) & 0xFF; ptr[6] = (v >> 8) & 0xFF; ptr[7] = v & 0xFF;
    }
  }

  NWII_NOINLINE uint64_t read64_slow(uint32_t addr) {
    uint8_t *ptr = get_ptr(addr);
    if (!ptr) return 0;
    return ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
           ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | ptr[7];
  }
  NWII_NOINLINE void write64_slow(uint32_t addr, uint64_t value) {
    uint32_t paddr = addr & 0x3FFFFFFF;
    if (g_watch_addr && (paddr & ~7u) == (g_watch_addr & 0x3FFFFFF8))
      watch_hit(addr, value >> 32, 64);

    uint8_t *ptr = get_ptr(addr);
    if (!ptr) return;
    ptr[0] = value >> 56; ptr[1] = (value >> 48) & 0xFF; ptr[2] = (value >> 40) & 0xFF; ptr[3] = (value >> 32) & 0xFF;
    ptr[4] = (value >> 24) & 0xFF; ptr[5] = (value >> 16) & 0xFF; ptr[6] = (value >> 8) & 0xFF; ptr[7] = value & 0xFF;
  }
  NWII_NOINLINE double read_f64_slow(uint32_t addr) { 
      uint8_t *ptr = get_ptr(addr);
      if (ptr) {
          uint64_t val = ((uint64_t)ptr[0] << 56) | ((uint64_t)ptr[1] << 48) | ((uint64_t)ptr[2] << 40) | ((uint64_t)ptr[3] << 32) |
                         ((uint64_t)ptr[4] << 24) | ((uint64_t)ptr[5] << 16) | ((uint64_t)ptr[6] << 8) | ptr[7];
          double f; std::memcpy(&f, &val, 8); return f;
      }
      return 0.0;
  }


  // Fast paths: plain MEM1 RAM (past the low vectors, which keep their
  // special cases) is a bounds check plus a byte-swapped load/store; anything
  // else -- hardware registers, the write-gather pipe, MEM2, watchpoints --
  // takes the out-of-line *_slow path. Every guest load/store goes through
  // here, and the out-of-line versions were ~25% of the CPU thread.
  static constexpr uint32_t kRamSize = 24u * 1024 * 1024;
  static NWII_FORCEINLINE bool fast_ram(uint32_t paddr, uint32_t len) {
    return paddr - 0x100u < kRamSize - 0x100u - (len - 1);
  }
  NWII_FORCEINLINE uint8_t read8(uint32_t addr) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (fast_ram(p, 1)) [[likely]] return mem1.data()[p];
    return read8_slow(addr);
  }
  NWII_FORCEINLINE uint16_t read16(uint32_t addr) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (fast_ram(p, 2)) [[likely]] { uint16_t v; std::memcpy(&v, mem1.data() + p, 2); return nwii_bswap16(v); }
    return read16_slow(addr);
  }
  NWII_FORCEINLINE uint32_t read32(uint32_t addr) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (fast_ram(p, 4)) [[likely]] { uint32_t v; std::memcpy(&v, mem1.data() + p, 4); return nwii_bswap32(v); }
    return read32_slow(addr);
  }
  NWII_FORCEINLINE uint64_t read64(uint32_t addr) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (fast_ram(p, 8)) [[likely]] { uint64_t v; std::memcpy(&v, mem1.data() + p, 8); return nwii_bswap64(v); }
    return read64_slow(addr);
  }
  NWII_FORCEINLINE double read_f64(uint32_t addr) {
    const uint64_t v = read64(addr); double f; std::memcpy(&f, &v, 8); return f;
  }
  NWII_FORCEINLINE void write8(uint32_t addr, uint8_t value) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (!g_watch_addr && fast_ram(p, 1)) [[likely]] { mem1.data()[p] = value; return; }
    write8_slow(addr, value);
  }
  NWII_FORCEINLINE void write16(uint32_t addr, uint16_t value) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (!g_watch_addr && fast_ram(p, 2)) [[likely]] { const uint16_t v = nwii_bswap16(value); std::memcpy(mem1.data() + p, &v, 2); return; }
    write16_slow(addr, value);
  }
  NWII_FORCEINLINE void write32(uint32_t addr, uint32_t value) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (!g_watch_addr && fast_ram(p, 4)) [[likely]] { const uint32_t v = nwii_bswap32(value); std::memcpy(mem1.data() + p, &v, 4); return; }
    write32_slow(addr, value);
  }
  NWII_FORCEINLINE void write64(uint32_t addr, uint64_t value) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (!g_watch_addr && fast_ram(p, 8)) [[likely]] { const uint64_t v = nwii_bswap64(value); std::memcpy(mem1.data() + p, &v, 8); return; }
    write64_slow(addr, value);
  }
  NWII_FORCEINLINE void write_f32(uint32_t addr, float value) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (!g_watch_addr && fast_ram(p, 4)) [[likely]] { uint32_t v; std::memcpy(&v, &value, 4); v = nwii_bswap32(v); std::memcpy(mem1.data() + p, &v, 4); return; }
    write_f32_slow(addr, value);
  }
  NWII_FORCEINLINE void write_f64(uint32_t addr, double value) {
    const uint32_t p = addr & 0x3FFFFFFF;
    if (!g_watch_addr && fast_ram(p, 8)) [[likely]] { uint64_t v; std::memcpy(&v, &value, 8); v = nwii_bswap64(v); std::memcpy(mem1.data() + p, &v, 8); return; }
    write_f64_slow(addr, value);
  }
};

struct CPUContext {
  
  std::array<uint32_t, 32> gpr;

  std::array<double, 32> fpr;

  std::array<double, 32> ps1;

  std::array<ConditionField, 8> cr;

  uint32_t pc;    
  uint32_t lr;    
  uint32_t ctr;   
  uint32_t xer;   
  uint32_t msr;   
  uint32_t fpscr; 
  uint32_t srr0;  
  uint32_t srr1;  

  std::array<uint32_t, 8> gqr;
  std::array<uint32_t, 4> sprg; 

  struct BackupState {
    std::array<uint32_t, 32> gpr;
    std::array<double, 32> fpr;
    std::array<double, 32> ps1;
    std::array<ConditionField, 8> cr;
    uint32_t lr;
    uint32_t ctr;
    uint32_t xer;
    uint32_t pc;
    uint32_t srr0;
    uint32_t srr1;
    uint32_t msr;
    uint32_t fpscr;
    std::array<uint32_t, 8> gqr;
    std::array<uint32_t, 4> sprg;
  };
  std::stack<BackupState> backup_stack;
  bool in_callback = false;
  int callback_depth = 0;

  MMU mmu;

  uint32_t exception_pc;
  uint64_t inst_count = 0;
  
  std::atomic<bool> is_running;
  std::atomic<bool> vblank_pending;
  std::mutex cb_mutex;
  std::queue<CallbackInfo> pending_callbacks;
  
  std::atomic<int> pending_cb_count{0};
  uint32_t poll_tick = 0; // poll_callbacks() throttle
  uint64_t poll_deadline = 0; // nwii_cycles() value of the next full check
  jmp_buf exception_jmp_buf;

  uint32_t reservation_addr = 0xFFFFFFFF;

  

  uint32_t dispatch_saved_ctx = 0;

  
  bool ext_resched_pending = false;

  



  

  
  uint64_t tb_freq = 40500000;
  std::chrono::steady_clock::time_point tb_start =
      std::chrono::steady_clock::now();

  

  
  // Instruction count is a virtual clock: the guest gets a fixed number of
  // instructions per emulated frame and never falls behind. Real elapsed time
  // (NWII_WALLTB=1) is the hardware-accurate source, but while we run slower
  // than real time it starves the game — measured on MP7: same input script,
  // 122 DVD reads with the virtual clock vs 52 with the wall clock. Switch the
  // default once the drain is fast enough to keep up.
  uint64_t read_timebase() const {
    if (g_wall_timebase) {
      uint64_t us = (uint64_t)std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - tb_start)
                        .count();
      return us / 1000000 * tb_freq + us % 1000000 * tb_freq / 1000000;
    }
    return inst_count;
  }

  
  uint64_t dec_value = 0xFFFFFFFF;
  uint64_t dec_written_tb = 0;
  bool dec_irq_pending = false;

  void write_dec(uint32_t v) {
    if (std::getenv("NWII_SAMPLE")) {
      static uint64_t n = 0;
      ++n;
      if (n <= 10 || (n % 500) == 0)
        std::cout << "[DEC] mtdec #" << n << " val=" << v << " pc=0x"
                  << std::hex << pc << " lr=0x" << lr << std::dec
                  << " inst=" << inst_count << "\n";
    }
    dec_value = v;
    dec_written_tb = read_timebase();
    dec_irq_pending = (v != 0xFFFFFFFF);
  }

  uint32_t read_dec() {
    uint64_t elapsed = read_timebase() - dec_written_tb;
    if (elapsed >= dec_value)
      return 0;
    return (uint32_t)(dec_value - elapsed);
  }

  bool dec_expired() {
    return dec_irq_pending && (read_timebase() - dec_written_tb) >= dec_value;
  }

  CPUContext() : gpr{0}, fpr{0.0}, ps1{0.0}, cr{}, pc(0), lr(0), ctr(0), xer(0), 
                 msr(0), fpscr(0), srr0(0), srr1(0), gqr{0}, exception_pc(0), is_running(true), vblank_pending(false) {}

  void queue_callback(uint32_t cb, uint32_t arg1, uint32_t arg2, bool is_irq = false) {
    std::lock_guard<std::mutex> lock(cb_mutex);
    pending_callbacks.push({cb, arg1, arg2, is_irq});
    pending_cb_count.fetch_add(1, std::memory_order_relaxed);
  }

  // GQR type 0 (plain floats) is what games use almost exclusively (Spyro:
  // every psq uses GQR0 = 0); keep it inline, quantised forms out of line.
  NWII_FORCEINLINE void psq_load(uint32_t frD, uint32_t addr, uint32_t W, uint32_t I) {
    if (((gqr[I] >> 16) & 7) == 0) [[likely]] {
      fpr[frD] = mmu.read_f32(addr);
      ps1[frD] = W ? 1.0 : (double)mmu.read_f32(addr + 4);
      return;
    }
    psq_load_slow(frD, addr, W, I);
  }
  NWII_FORCEINLINE void psq_store(uint32_t frS, uint32_t addr, uint32_t W, uint32_t I) {
    if ((gqr[I] & 7) == 0) [[likely]] {
      mmu.write_f32(addr, (float)fpr[frS]);
      if (!W) mmu.write_f32(addr + 4, (float)ps1[frS]);
      return;
    }
    psq_store_slow(frS, addr, W, I);
  }
  NWII_NOINLINE void psq_load_slow(uint32_t frD, uint32_t addr, uint32_t W, uint32_t I) {
    uint32_t gqr_val = gqr[I];
    uint32_t ld_type = (gqr_val >> 16) & 0x7;
    // Scale is a signed 6-bit exponent: value = raw * 2^-scale.
    int ld_scale = (int)((gqr_val >> 24) & 0x3F);
    if (ld_scale & 0x20) ld_scale -= 64;
    float scale = std::ldexp(1.0f, -ld_scale);

    auto load_element = [&](uint32_t a) -> double {
      switch (ld_type) {
      case 0:
        return mmu.read_f32(a);
      case 4:
        return mmu.read8(a) * scale;
      case 5:
        return mmu.read16(a) * scale;
      case 6:
        return (int8_t)mmu.read8(a) * scale;
      case 7:
        return (int16_t)mmu.read16(a) * scale;
      default:
        return 0.0;
      }
    };

    uint32_t elem_size =
        (ld_type == 0) ? 4 : ((ld_type == 4 || ld_type == 6) ? 1 : 2);

    fpr[frD] = load_element(addr);
    if (W == 0) { 
      ps1[frD] = load_element(addr + elem_size);
    } else { 
      ps1[frD] = 1.0;
    }
  }

  NWII_NOINLINE void psq_store_slow(uint32_t frS, uint32_t addr, uint32_t W, uint32_t I) {
    uint32_t gqr_val = gqr[I];
    uint32_t st_type = gqr_val & 0x7;
    int st_scale = (int)((gqr_val >> 8) & 0x3F);
    if (st_scale & 0x20) st_scale -= 64;
    float scale = std::ldexp(1.0f, st_scale);
    // Quantized stores saturate to the integer type's range (truncating).
    auto q = [&](double v, double lo, double hi) -> int32_t {
      v *= scale;
      if (!(v > lo)) return (int32_t)lo;  // also NaN
      if (v > hi) return (int32_t)hi;
      return (int32_t)v;
    };

    auto store_element = [&](uint32_t a, double val) {
      switch (st_type) {
      case 0:
        mmu.write_f32(a, (float)val);
        break;
      case 4:
        mmu.write8(a, (uint8_t)q(val, 0, 255));
        break;
      case 5:
        mmu.write16(a, (uint16_t)q(val, 0, 65535));
        break;
      case 6:
        mmu.write8(a, (uint8_t)(int8_t)q(val, -128, 127));
        break;
      case 7:
        mmu.write16(a, (uint16_t)(int16_t)q(val, -32768, 32767));
        break;
      }
    };

    uint32_t elem_size =
        (st_type == 0) ? 4 : ((st_type == 4 || st_type == 6) ? 1 : 2);

    store_element(addr, fpr[frS]);
    if (W == 0) {
      store_element(addr + elem_size, ps1[frS]);
    }
  }

  inline void ps_add(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = (float)(fpr[A] + fpr[B]);
    ps1[D] = (float)(ps1[A] + ps1[B]);
  }
  inline void ps_sub(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = (float)(fpr[A] - fpr[B]);
    ps1[D] = (float)(ps1[A] - ps1[B]);
  }
  inline void ps_mul(uint32_t D, uint32_t A, uint32_t C) {
    fpr[D] = (float)(fpr[A] * fpr[C]);
    ps1[D] = (float)(ps1[A] * ps1[C]);
  }
  inline void ps_madd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (float)(std::fma(fpr[A], fpr[C], fpr[B]));
    ps1[D] = (float)(std::fma(ps1[A], ps1[C], ps1[B]));
  }
  inline void ps_msub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (float)(std::fma(fpr[A], fpr[C], -fpr[B]));
    ps1[D] = (float)(std::fma(ps1[A], ps1[C], -ps1[B]));
  }
  inline void ps_nmadd(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (float)(-std::fma(fpr[A], fpr[C], fpr[B]));
    ps1[D] = (float)(-std::fma(ps1[A], ps1[C], ps1[B]));
  }
  inline void ps_nmsub(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (float)(-std::fma(fpr[A], fpr[C], -fpr[B]));
    ps1[D] = (float)(-std::fma(ps1[A], ps1[C], -ps1[B]));
  }
  inline void ps_div(uint32_t D, uint32_t A, uint32_t B) {
    fpr[D] = (float)(fpr[A] / fpr[B]);
    ps1[D] = (float)(ps1[A] / ps1[B]);
  }
  inline void ps_sum0(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {

    
    double s = fpr[A] + ps1[B];
    double t = ps1[C];
    fpr[D] = (float)s;
    ps1[D] = (float)t;
  }
  inline void ps_sum1(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    
    double s = fpr[C];
    double t = fpr[A] + ps1[B];
    fpr[D] = (float)s;
    ps1[D] = (float)t;
  }
  inline void ps_muls0(uint32_t D, uint32_t A, uint32_t C) {
    fpr[D] = (float)(fpr[A] * fpr[C]);
    ps1[D] = (float)(ps1[A] * fpr[C]);
  }
  inline void ps_muls1(uint32_t D, uint32_t A, uint32_t C) {
    fpr[D] = (float)(fpr[A] * ps1[C]);
    ps1[D] = (float)(ps1[A] * ps1[C]);
  }
  inline void ps_madds0(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (float)(std::fma(fpr[A], fpr[C], fpr[B]));
    ps1[D] = (float)(std::fma(ps1[A], fpr[C], ps1[B]));
  }
  inline void ps_madds1(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (float)(std::fma(fpr[A], ps1[C], fpr[B]));
    ps1[D] = (float)(std::fma(ps1[A], ps1[C], ps1[B]));
  }
  inline void ps_sel(uint32_t D, uint32_t A, uint32_t C, uint32_t B) {
    fpr[D] = (fpr[A] >= 0.0) ? fpr[C] : fpr[B];
    ps1[D] = (ps1[A] >= 0.0) ? ps1[C] : ps1[B];
  }
  inline void ps_mr(uint32_t D, uint32_t B) {
    fpr[D] = fpr[B];
    ps1[D] = ps1[B];
  }
  inline void ps_neg(uint32_t D, uint32_t B) {
    fpr[D] = -fpr[B];
    ps1[D] = -ps1[B];
  }
  inline void ps_abs(uint32_t D, uint32_t B) {
    fpr[D] = std::abs(fpr[B]);
    ps1[D] = std::abs(ps1[B]);
  }
  inline void ps_nabs(uint32_t D, uint32_t B) {
    fpr[D] = -std::abs(fpr[B]);
    ps1[D] = -std::abs(ps1[B]);
  }

  
  inline void ps_merge00(uint32_t D, uint32_t A, uint32_t B) {
    double a = fpr[A], b = fpr[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge01(uint32_t D, uint32_t A, uint32_t B) {
    double a = fpr[A], b = ps1[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge10(uint32_t D, uint32_t A, uint32_t B) {
    double a = ps1[A], b = fpr[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_merge11(uint32_t D, uint32_t A, uint32_t B) {
    double a = ps1[A], b = ps1[B];
    fpr[D] = a; ps1[D] = b;
  }
  inline void ps_cmpu0(uint32_t crD, uint32_t A, uint32_t B) {
    bool un = std::isnan(fpr[A]) || std::isnan(fpr[B]);
    cr[crD].lt = !un && (fpr[A] < fpr[B]);
    cr[crD].gt = !un && (fpr[A] > fpr[B]);
    cr[crD].eq = !un && (fpr[A] == fpr[B]);
    cr[crD].so = un;
  }
  inline void ps_cmpo0(uint32_t crD, uint32_t A, uint32_t B) {
    bool un = std::isnan(fpr[A]) || std::isnan(fpr[B]);
    cr[crD].lt = !un && (fpr[A] < fpr[B]);
    cr[crD].gt = !un && (fpr[A] > fpr[B]);
    cr[crD].eq = !un && (fpr[A] == fpr[B]);
    cr[crD].so = un;
  }
  inline void ps_cmpu1(uint32_t crD, uint32_t A, uint32_t B) {
    bool un = std::isnan(ps1[A]) || std::isnan(ps1[B]);
    cr[crD].lt = !un && (ps1[A] < ps1[B]);
    cr[crD].gt = !un && (ps1[A] > ps1[B]);
    cr[crD].eq = !un && (ps1[A] == ps1[B]);
    cr[crD].so = un;
  }
  inline void ps_cmpo1(uint32_t crD, uint32_t A, uint32_t B) {
    bool un = std::isnan(ps1[A]) || std::isnan(ps1[B]);
    cr[crD].lt = !un && (ps1[A] < ps1[B]);
    cr[crD].gt = !un && (ps1[A] > ps1[B]);
    cr[crD].eq = !un && (ps1[A] == ps1[B]);
    cr[crD].so = un;
  }
};

// Loop back-edges in compiled code poll for interrupts/callbacks. The full
// check (wall clock, scheduler, decrementer, device ticks) costs far more
// than a loop iteration, so it runs only every 16th back-edge AND once
// g_poll_cycles have passed -- or at once when a callback is queued. Calling
// it on every 16th back-edge alone was ~7% of the CPU thread (a tight loop
// iterates many times per microsecond); interrupt latency stays ~20 us,
// small next to the 1-8 ms hardware timers involved.
NWII_FORCEINLINE bool poll_callbacks(CPUContext &c) {
  if ((++c.poll_tick & 15) != 0 &&
      c.pending_cb_count.load(std::memory_order_relaxed) == 0) [[likely]]
    return false;
  if (c.pending_cb_count.load(std::memory_order_relaxed) == 0) {
    const uint64_t now = nwii_cycles();
    if (now < c.poll_deadline) return false;
    c.poll_deadline = now + nwii::runtime::g_poll_cycles;
  }
  return process_pending_callbacks(c);
}

void hle_set_ipc_arm_msg(uint32_t req_addr);
void micro_interpret(CPUContext& ctx, uint32_t opcode, uint32_t pc);

bool interpret_one(CPUContext& ctx);
void interpret_step(CPUContext& ctx);
void add_recompiled_range(uint32_t start, uint32_t end);
bool in_recompiled_code(uint32_t pc);

extern bool g_trace_calls;
// Interpreter mode (NWII_CTRL "interp on"): every recompiled function hands
// execution to interp_run() instead of its compiled body. For telling
// recompilation bugs from runtime/HLE ones.
extern volatile bool g_interp_mode;
// Only functions below this address are interpreted; the rest (the SDK:
// OS threads, DVD, GX...) keep running compiled, and interp_run calls them.
extern volatile uint32_t g_interp_limit;
void register_compiled_func(uint32_t addr, void (*fn)(CPUContext&));
// Call the compiled function at ctx.pc in place (indirect calls); false when
// ctx.pc is not a known function entry.
bool call_indirect(CPUContext& ctx);
void interp_run(CPUContext& ctx);
// Guest addresses bound to native handlers ([hle_hooks], HLE seams); the
// interpreter calls these instead of interpreting the original SDK code.
void register_native_func(uint32_t addr, void (*fn)(CPUContext&));
void trace_call(uint32_t func_addr, CPUContext& ctx);




void note_null_call(uint32_t site, uint32_t lr);

} 
} 
