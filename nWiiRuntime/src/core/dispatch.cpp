// Guest-address dispatcher (see runtime/dispatch.h).
#include "runtime/dispatch.h"
#include "runtime/cpu_context.h"

#include <algorithm>
#include <chrono>
#include <csetjmp>
#include <iostream>
#include <thread>
#include <vector>

namespace nwii::runtime {

static DispatchTables s_tables{};
// Flat lookup over MEM1 code: entry index + 1 per word (0 = no entry).
// Function-local: it is filled from main_output.cpp's static initializer.
static std::vector<uint32_t> &flat() {
  static std::vector<uint32_t> v;
  return v;
}
static constexpr uint32_t kFlatBase = 0x80000000u;

bool register_dispatch_tables(const DispatchTables &t) {
  s_tables = t;
  for (size_t i = 0; i < t.num_natives; ++i)
    register_native_func(t.natives[i].addr, t.natives[i].fn);
  for (size_t i = 0; i < t.num_bounds; ++i)
    register_compiled_func(t.bounds[i].start, t.bounds[i].fn);

  uint32_t max_addr = kFlatBase;
  for (size_t i = 0; i < t.num_entries; ++i)
    if (t.entries[i].addr >= kFlatBase && t.entries[i].addr < 0x81800000u)
      max_addr = std::max(max_addr, t.entries[i].addr);
  std::vector<uint32_t> &s_flat = flat();
  s_flat.assign(((max_addr - kFlatBase) >> 2) + 1, 0);
  for (size_t i = 0; i < t.num_entries; ++i) {
    const uint32_t a = t.entries[i].addr;
    if (a >= kFlatBase && a <= max_addr && !(a & 3))
      s_flat[(a - kFlatBase) >> 2] = (uint32_t)i + 1;
  }
  return true;
}

static const DispatchEntry *find_entry(uint32_t addr) {
  static const std::vector<uint32_t> &s_flat = flat();
  if (addr >= kFlatBase && !(addr & 3)) {
    const uint32_t idx = (addr - kFlatBase) >> 2;
    if (idx < s_flat.size())
      return s_flat[idx] ? &s_tables.entries[s_flat[idx] - 1] : nullptr;
  }
  // Outside the flat range (low memory, unaligned): binary search.
  const DispatchEntry *b = s_tables.entries, *e = b + s_tables.num_entries;
  const DispatchEntry *it = std::lower_bound(
      b, e, addr, [](const DispatchEntry &d, uint32_t a) { return d.addr < a; });
  return it != e && it->addr == addr ? it : nullptr;
}

void run_dispatcher(CPUContext &ctx) {
  static uint32_t pc_history[10] = {0};
  static int pc_history_idx = 0;
  if (ctx.pc == 0) ctx.pc = s_tables.entry_pc;
  while (ctx.is_running) {
    pc_history[pc_history_idx] = ctx.pc;
    pc_history_idx = (pc_history_idx + 1) % 10;
    if (ctx.pc == 0) {
      static bool warned_zero = false;
      if (!warned_zero) {
        warned_zero = true;
        std::cout << "[run_game] PC=0 (branch to NULL), last PCs:";
        for (int i = 0; i < 10; ++i)
          std::cout << " 0x" << std::hex << pc_history[(pc_history_idx + 9 - i) % 10];
        std::cout << " lr=0x" << ctx.lr << std::dec << std::endl;
      }
      if ((ctx.msr & 0x8000) == 0) ctx.msr |= 0x8000; // Force EE=1 in idle
      process_pending_callbacks(ctx);
      if (ctx.pc == 0)
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
      continue;
    }
    if (_setjmp(ctx.exception_jmp_buf) != 0)
      continue;
    process_pending_callbacks(ctx);
    if (ctx.pc == 0xFFFFFFFC) {
      if (!ctx.backup_stack.empty()) {
        auto &bk = ctx.backup_stack.top();
        ctx.gpr = bk.gpr; ctx.fpr = bk.fpr; ctx.ps1 = bk.ps1;
        ctx.cr = bk.cr; ctx.lr = bk.lr; ctx.ctr = bk.ctr;
        ctx.xer = bk.xer; ctx.pc = bk.pc;
        ctx.srr0 = bk.srr0; ctx.srr1 = bk.srr1;
        ctx.msr = bk.msr; ctx.fpscr = bk.fpscr;
        ctx.gqr = bk.gqr; ctx.sprg = bk.sprg;
        ctx.backup_stack.pop();
      }
      ctx.callback_depth--;
      if (ctx.callback_depth <= 0) {
        ctx.in_callback = false;
        ctx.callback_depth = 0;
      }
      continue;
    }
    if ((ctx.pc & 0xF0000000) == 0xC0000000) ctx.pc = (ctx.pc & 0x0FFFFFFF) | 0x80000000;
    else if ((ctx.pc & 0xF0000000) == 0xD0000000) ctx.pc = (ctx.pc & 0x0FFFFFFF) | 0x90000000;
    else if (ctx.pc < 0x80000000) ctx.pc |= 0x80000000;
    const uint32_t target = ctx.pc;
    if ((++ctx.inst_count % 100000) == 0 && g_verbose)
      std::cout << "Dispatcher PC: 0x" << std::hex << target << std::endl;
    if (const DispatchEntry *d = find_entry(target)) {
      d->fn(ctx);
      if (d->flags & kDispatchHle) ctx.pc = ctx.lr;
      continue;
    }
    bool found = false;
    for (size_t i = 0; i < s_tables.num_bounds; ++i) {
      const FuncBound &fb = s_tables.bounds[i];
      if (target >= fb.start && target < fb.end) {
        ctx.pc = target;
        fb.fn(ctx);
        found = true;
        break;
      }
    }
    // Not in any recompiled function: runtime-generated code (copied to
    // low mem, trampolines). Interpret it from memory.
    if (!found) interpret_step(ctx);
  }
  std::cout << "[DEBUG] PC History before 0x0:\n";
  for (int i = 0; i < 10; ++i)
    std::cout << "  0x" << std::hex << pc_history[(pc_history_idx + i) % 10] << "\n";
}

} // namespace nwii::runtime
