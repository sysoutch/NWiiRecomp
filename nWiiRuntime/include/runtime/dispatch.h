#pragma once
// Guest-address dispatch tables emitted by the recompiler (main_output.cpp)
// and the dispatcher loop that runs them.
//
// main_output.cpp used to contain the dispatcher itself: one function with a
// switch of ~46k cases (every function entry, call return address, loop head
// and jump-table target). That single function was the slowest object in the
// whole build, and it was recompiled whenever a runtime header changed. Now
// the generated file is plain data plus a two-line run_game, and only needs
// this header -- no CPUContext definition.
#include <cstddef>
#include <cstdint>

namespace nwii::runtime {

struct CPUContext;
using GuestFn = void (*)(CPUContext &);

enum : uint32_t {
  kDispatchHle = 1, // native handler: continue at ctx.lr after the call
};

// One resumable guest address and the compiled function that handles it.
struct DispatchEntry {
  uint32_t addr;
  uint32_t flags;
  GuestFn fn;
};

// Address range [start, end) of a compiled function (fallback for addresses
// that are not entries, e.g. interrupt returns into a function body).
struct FuncBound {
  uint32_t start;
  uint32_t end;
  GuestFn fn;
};

// Native (HLE) handler bound to a guest function, for the interpreter mode.
struct NativeFunc {
  uint32_t addr;
  GuestFn fn;
};

struct DispatchTables {
  const DispatchEntry *entries; // sorted by addr, unique
  size_t num_entries;
  const FuncBound *bounds;      // emission order (first match wins)
  size_t num_bounds;
  const NativeFunc *natives;
  size_t num_natives;
  uint32_t entry_pc;
};

// Called from a static initializer in main_output.cpp: registers the compiled
// and native functions (call_indirect, interpreter, profiler) and builds the
// dispatch lookup.
bool register_dispatch_tables(const DispatchTables &t);

// The dispatcher loop: runs guest code from ctx.pc (or the entry point) until
// ctx.is_running goes false.
void run_dispatcher(CPUContext &ctx);

} // namespace nwii::runtime
