// NWII_NPROF=1: native sampling profiler for the CPU thread (Windows).
//
// A helper thread suspends the CPU thread about every 0.5 ms, reads its
// instruction pointer and resumes it. Samples inside the recompiled code are
// attributed to the guest function whose compiled body contains them (the
// entry points registered by main_output, sorted by native address -- no PDB
// needed); everything else is grouped by module, or by RVA inside the exe.
// Every NWII_NPROF_SECS seconds (default 10) the top entries are printed.
#include "runtime/cpu_context.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#endif

namespace nwii::runtime {

std::vector<std::pair<uintptr_t, uint32_t>> compiled_func_natives(); // interpreter.cpp

#ifdef _WIN32
static void profiler_loop(HANDLE cpu) {
  const char *secs_env = std::getenv("NWII_NPROF_SECS");
  const int secs = secs_env ? std::max(1, std::atoi(secs_env)) : 10;
  std::vector<std::pair<uintptr_t, uint32_t>> funcs = compiled_func_natives();
  std::sort(funcs.begin(), funcs.end());
  HMODULE exe = GetModuleHandleW(nullptr);
  std::unordered_map<uintptr_t, uint64_t> hits; // raw rip -> count
  uint64_t total = 0;
  auto t0 = std::chrono::steady_clock::now();
  timeBeginPeriod(1);
  for (;;) {
    std::this_thread::sleep_for(std::chrono::microseconds(500));
    if (SuspendThread(cpu) == (DWORD)-1) return; // thread gone
    CONTEXT c{};
    c.ContextFlags = CONTEXT_CONTROL;
    const bool got = GetThreadContext(cpu, &c) != 0;
    // Resume before touching the heap: the target may hold the heap lock.
    ResumeThread(cpu);
    if (got) {
      ++hits[(uintptr_t)c.Rip];
      ++total;
    }
    const auto now = std::chrono::steady_clock::now();
    if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() < secs) continue;
    t0 = now;
    // Attribute.
    std::map<std::string, uint64_t> by;
    for (const auto &[rip, n] : hits) {
      std::string key;
      auto it = std::upper_bound(funcs.begin(), funcs.end(), std::make_pair(rip, UINT32_MAX));
      const bool in_guest = it != funcs.begin() && !funcs.empty() && rip < funcs.back().first + 0x20000;
      HMODULE mod = nullptr;
      GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                             GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                         (LPCWSTR)rip, &mod);
      char buf[64];
      if (mod == exe && in_guest && rip >= funcs.front().first) {
        std::snprintf(buf, sizeof buf, "guest %08X", (--it)->second);
        key = buf;
      } else if (mod == exe) {
        // Runtime code: bucket by 256-byte RVA to keep it readable.
        std::snprintf(buf, sizeof buf, "exe+%06zX", (size_t)((rip - (uintptr_t)exe) & ~0xFFull));
        key = buf;
      } else if (mod) {
        wchar_t path[MAX_PATH];
        GetModuleFileNameW(mod, path, MAX_PATH);
        std::wstring w(path);
        const size_t s = w.find_last_of(L"\\/");
        std::wstring base = s == std::wstring::npos ? w : w.substr(s + 1);
        key = "dll " + std::string(base.begin(), base.end());
      } else {
        key = "unknown";
      }
      by[key] += n;
    }
    std::vector<std::pair<uint64_t, std::string>> v;
    uint64_t guest_total = 0;
    for (auto &[k, n] : by) {
      v.push_back({n, k});
      if (k.rfind("guest", 0) == 0) guest_total += n;
    }
    std::sort(v.rbegin(), v.rend());
    std::printf("[NPROF] %llu samples, guest code %.1f%%\n", (unsigned long long)total,
                total ? 100.0 * guest_total / total : 0.0);
    for (size_t i = 0; i < v.size() && i < 30; ++i)
      std::printf("[NPROF] %5.1f%% %s\n", 100.0 * v[i].first / total, v[i].second.c_str());
    std::fflush(stdout);
    // NWII_NPROF_OUT=<file>: raw samples as "rva count" (exe) or "dll name
    // count" lines, appended per interval -- resolve with the linker map.
    if (const char *out = std::getenv("NWII_NPROF_OUT")) {
      if (FILE *f = std::fopen(out, "a")) {
        std::fprintf(f, "# interval %llu samples\n", (unsigned long long)total);
        for (const auto &[rip, n] : hits) {
          HMODULE mod = nullptr;
          GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                                 GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             (LPCWSTR)rip, &mod);
          if (mod == exe)
            std::fprintf(f, "%zx %llu\n", (size_t)(rip - (uintptr_t)exe), (unsigned long long)n);
          else // module base, offset in it (resolve with its export table)
            std::fprintf(f, "dll %p %llu %zx\n", (void *)mod, (unsigned long long)n,
                         (size_t)(rip - (uintptr_t)mod));
        }
        std::fclose(f);
      }
    }
    hits.clear();
    total = 0;
  }
}
#endif

// Called by a runtime thread on start (e.g. the GX parser, name "parse"):
// with NWII_NPROF_THREAD=<name> that thread is sampled instead.
void native_prof_attach_self(const char *name) {
#ifdef _WIN32
  const char *which = std::getenv("NWII_NPROF_THREAD");
  if (!std::getenv("NWII_NPROF") || !which || std::string(which) != name) return;
  HANDLE h = nullptr;
  DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &h,
                  THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
  if (!h) return;
  std::thread(profiler_loop, h).detach();
  std::printf("[NPROF] sampling the %s thread\n", name);
#else
  (void)name;
#endif
}

void native_prof_start(std::thread &cpu_thread) {
#ifdef _WIN32
  if (!std::getenv("NWII_NPROF")) return;
  // NWII_NPROF_THREAD=main: sample the calling (main/render) thread instead;
  // other names are attached by their threads (native_prof_attach_self).
  const char *which = std::getenv("NWII_NPROF_THREAD");
  if (which && std::string(which) != "main") return;
  const bool main_thread = which && std::string(which) == "main";
  HANDLE h = nullptr;
  DuplicateHandle(GetCurrentProcess(),
                  main_thread ? GetCurrentThread() : (HANDLE)cpu_thread.native_handle(),
                  GetCurrentProcess(), &h, THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT, FALSE, 0);
  if (!h) return;
  std::thread(profiler_loop, h).detach();
  std::printf("[NPROF] sampling the CPU thread\n");
#else
  (void)cpu_thread;
#endif
}

} // namespace nwii::runtime
