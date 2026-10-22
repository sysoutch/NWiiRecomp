#include "recompiler/recompiler.h"
#include "ppc/instruction.h"
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <map>
#include <iostream>
#include <sstream>
#include <unordered_set>
#include <algorithm>

namespace nwii {
namespace recomp {

Recompiler::Recompiler(const analyzer::Analyzer &analyzer,
                       const SymbolTable *symbols,
                       const RecompilerConfig &config)
    : analyzer_(analyzer), symbols_(symbols), config_(config) {}

static const std::unordered_set<std::string> &hle_handlers() {
  // Native handlers that actually exist in nWiiRuntime. Binding is by symbol
  // name, so any game whose symbols the analyzer can name gets the whole seam
  // with no per-game configuration; [hle_hooks] in the TOML stays available for
  // overrides and for games where a symbol is missed.
  //
  // Deliberately conservative: only handlers that have been run and checked in
  // a real game are listed. Binding DVDReadAsyncPrio automatically loaded the
  // game's DLL as zeros and gave a black screen, so disc, audio, IOS and the
  // remaining VI entry points stay out until each is verified on its own. They
  // can still be forced per game through [hle_hooks].
  //
  // GXLoadTexMtxImm and GXCallDisplayList are deliberately absent: both are
  // implemented but broke rendering in Mario Party 7, so they stay opt-in via
  // the TOML until that is understood.
  //
  // GX state setters whose value the SDK also caches in __GXData (zmode,
  // cmode0/1, genMode, peCntrl, matIdxA) are NOT bound: the native versions
  // only write the register, so the SDK's later "restore from cache" --
  // GXCopyDisp/GXCopyTex put Z mode and blend state back after a clearing
  // copy -- wrote stale values. In Spyro every EFB copy reset Z mode to 0 and
  // the whole world drew without depth. The game's own setters write the same
  // FIFO commands and keep the cache coherent. (GXSetZMode, GXSetBlendMode,
  // GXSetZCompLoc, GXSetDstAlpha, GXSetCullMode, GXSetNumTevStages,
  // GXSetCurrentMtx remain available through [hle_hooks].)
  static const std::unordered_set<std::string> s = {
      "GXLoadNrmMtxImm",
      "GXLoadPosMtxImm",
      "GXSetAlphaCompare",
      "GXSetScissor",
      "OSAllocFromHeap",
      "OSCreateHeap",
      "OSFreeToHeap",
      "OSInitAlloc",
      "OSReport",
      "PADInit",
      "PADRead",
      // VIWaitForRetrace is deliberately absent: the native stub returned
      // at once instead of sleeping the thread until the next retrace, so
      // the main loop never yielded -- frames ran unthrottled and lower-
      // priority threads starved (Spyro's chunk streamer never finished the
      // title load). The game's own SDK routine sleeps on the VI queue.
  };
  return s;
}

// Symbols out of the signature database can carry the library they came from,
// e.g. "GXSetCullMode\tgx.a GXGeometry.o" -- match on the bare name.
static std::string bare_symbol(const std::string &name) {
  size_t cut = name.find_first_of(" \t");
  if (cut == std::string::npos) {
    // By this point the name has usually been sanitised into an identifier,
    // so the separator that followed the function name is now "__".
    cut = name.find("__", 1);
  }
  std::string out = cut == std::string::npos ? name : name.substr(0, cut);
  // Symbols also arrive carrying their own address, e.g. GXSetZMode_800C0CC0.
  if (out.size() > 9) {
    const size_t us = out.size() - 9;
    if (out[us] == '_' &&
        out.find_first_not_of("0123456789ABCDEFabcdef", us + 1) ==
            std::string::npos)
      out.resize(us);
  }
  return out;
}

static bool is_hle_function(const std::string &name) {
  return hle_handlers().count(bare_symbol(name)) != 0;
}

// The analyzer's built-in prologue signatures (signature_scanner.cpp) are
// loose; the IOS_* ones are plain function prologues. A GameCube game has no
// IOS, so such a match is always a false positive -- in Spyro it replaced a
// game function at 0x80134F60 with the Wii IOS_IoctlvAsync stub.
static bool g_gamecube = false;
static bool sig_hooked(const analyzer::Function &func) {
  if (func.hle_hook_name.empty()) return false;
  return !(g_gamecube && func.hle_hook_name.rfind("IOS_", 0) == 0);
}


std::vector<std::string> Recompiler::generate_cpp(uint32_t entry_point) {
  std::vector<std::string> generated_files;
  g_gamecube = config_.gamecube;

  std::cout << "[Recompiler] Starting C++ generation. Total functions to emit: "
            << analyzer_.get_functions().size() << "\n";

  auto emit_headers = [](std::ostream &out) {
    out << "#include \"runtime/cpu_context.h\"\n";
    out << "#include \"runtime/config.h\"\n";
    out << "#include <stdint.h>\n";
    out << "#include <bit>\n";
    out << "#include <thread>\n";
    out << "#include <chrono>\n";

    out << "#include <cmath>\n";
    out << "#include <iostream>\n";
    out << "#include <cstdlib>\n\n";
    out << "using namespace nwii::runtime;\n\n";
  };

  // Dispatch tables for main_output.cpp / output.cpp: every resumable guest
  // address (function entries, call return addresses, loop heads, jump-table
  // targets) -> its compiled function, plus function bounds and the native
  // handlers. The dispatcher loop itself lives in the runtime (dispatch.cpp);
  // emitting it as a ~46k-case switch made this the slowest file to build.
  auto emit_dispatch = [&](std::ostream &out,
                           const std::vector<std::string> &all_func_names,
                           const std::vector<std::string> &all_base_names) {
    out << "// --- Function Bounds Table ---\n";
    out << "static const nwii::runtime::FuncBound g_func_bounds[] = {\n";
    size_t func_idx = 0;
    size_t num_bounds = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      const std::string &func_name = all_func_names[func_idx];
      const bool is_hle = sig_hooked(func) ||
                          is_hle_function(all_base_names[func_idx]);
      ++func_idx;
      if (!is_hle) {
        out << "    {0x" << std::hex << std::uppercase << start_addr << ", 0x"
            << func.end_address << std::dec << ", " << func_name << "},\n";
        ++num_bounds;
      }
    }
    out << "    {0, 0, nullptr}\n";
    out << "};\n\n";

    // Native handlers bound to guest addresses, for the interpreter mode.
    out << "// --- Native (HLE) functions ---\n";
    out << "static const nwii::runtime::NativeFunc g_native_funcs[] = {\n";
    func_idx = 0;
    size_t num_natives = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      const std::string &fname = all_func_names[func_idx];
      const bool is_hle = sig_hooked(func) ||
                          is_hle_function(all_base_names[func_idx]) ||
                          config_.hle_hooks.count(start_addr);
      ++func_idx;
      if (is_hle) {
        out << "    {0x" << std::hex << std::uppercase << start_addr << std::dec
            << ", " << fname << "},\n";
        ++num_natives;
      }
    }
    out << "    {0, nullptr}\n};\n\n";

    // The first function that claims an address keeps it.
    std::map<uint32_t, std::pair<std::string, bool>> entries; // addr -> (fn, hle)
    func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      const std::string &func_name = all_func_names[func_idx];
      const bool is_hle = sig_hooked(func) ||
                          is_hle_function(all_base_names[func_idx]);
      ++func_idx;
      if (is_hle) {
        entries.emplace(start_addr,
                        std::make_pair(sig_hooked(func) ? func.hle_hook_name
                                                        : func_name,
                                       true));
        continue;
      }
      entries.emplace(start_addr, std::make_pair(func_name, false));
      for (const auto &inst : func.instructions) {
        ppc::Instruction ppc_inst(inst.opcode);
        if (ppc_inst.is_branch_link() || ppc_inst.opcode() == 17)
          entries.emplace(inst.address + 4, std::make_pair(func_name, false));
        if (ppc_inst.opcode() == 16 || ppc_inst.opcode() == 18) {
          uint32_t target = ppc_inst.branch_target(inst.address);
          bool is_local =
              std::find_if(func.instructions.begin(), func.instructions.end(),
                           [target](const auto &i) {
                             return i.address == target;
                           }) != func.instructions.end();
          if (target <= inst.address && is_local)
            entries.emplace(inst.address, std::make_pair(func_name, false));
        }
      }
      for (uint32_t jump_target : func.jump_table_targets)
        entries.emplace(jump_target, std::make_pair(func_name, false));
    }
    out << "// --- Dispatch entries (sorted) ---\n";
    out << "static const nwii::runtime::DispatchEntry g_dispatch[] = {\n";
    for (const auto &[addr, e] : entries)
      out << "    {0x" << std::hex << std::uppercase << addr << std::dec
          << (e.second ? ", nwii::runtime::kDispatchHle, " : ", 0, ")
          << e.first << "},\n";
    out << "};\n\n";

    out << "static const bool g_dispatch_registered =\n"
           "    nwii::runtime::register_dispatch_tables({g_dispatch, "
        << entries.size() << ", g_func_bounds, " << num_bounds
        << ", g_native_funcs, " << num_natives << ", 0x" << std::hex
        << std::uppercase << entry_point << std::dec << "});\n\n";

    out << "// --- Entry Point Wrapper ---\n";
    out << "extern \"C\" void run_game(nwii::runtime::CPUContext& ctx) {\n"
           "    nwii::runtime::run_dispatcher(ctx);\n"
           "}\n";
  };

  if (config_.split_output) {
    
    std::string header_path = config_.output_dir + "/functions.h";
    std::ofstream hout(header_path);
    hout << "#pragma once\n";
    hout << "namespace nwii::runtime { struct CPUContext; }\n\n";
    hout << "extern \"C\" {\n";
    hout << "void OSReport(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Open(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_OpenAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Close(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Read(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Ioctl(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_IoctlAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Ioctlv(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_IoctlvAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void iosAlloc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void iosFree(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIConfigure(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIConfigurePan(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void PADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VISetBlack(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIGetNextField(nwii::runtime::CPUContext& ctx);\n";
    hout << "void VIWaitForRetrace(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSDisableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSEnableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSRestoreInterrupts(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetTime(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSTicksToMilliseconds(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInitFifoBase(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXInitFifoPtrs(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXGetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCopyClear(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXBegin(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXEnd(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetVtxDesc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetVtxAttrFmt(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadPosMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXCallDisplayList(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadNrmMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXLoadTexMtxImm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void GXSetCurrentMtx(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Write(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_Seek(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_CloseAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_ReadAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_WriteAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void IOS_SeekAsync(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXAcquireVoice(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXFreeVoice(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceState(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceMix(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceAdpcm(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceSrc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void AXSetVoiceOffsets(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDOpen(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVD_Callback(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDReadAsyncPrio(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDClose(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDGetDriveStatus(nwii::runtime::CPUContext& ctx);\n";
    hout << "void DVDReadPrio(nwii::runtime::CPUContext& ctx);\n";
    hout << "void PADRead(nwii::runtime::CPUContext& ctx);\n";
    hout << "void WPADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void WPADRead(nwii::runtime::CPUContext& ctx);\n";
    hout << "void KPADInit(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSInitAlloc(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSCreateHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSAllocFromHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSFreeToHeap(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSGetCurrentThread(nwii::runtime::CPUContext& ctx);\n";
    hout << "void OSSleepThread(nwii::runtime::CPUContext& ctx);\n";
    // [hle_hooks] may name any native handler, not just the ones above;
    // declare them all (a repeated identical declaration is harmless).
    for (const auto &[addr, name] : config_.hle_hooks)
      hout << "void " << name << "(nwii::runtime::CPUContext& ctx);\n";
    hout << "}\n";

    std::set<std::string> emitted_names;
    std::vector<std::string> all_func_names;
    // The symbol name before any uniquifying suffix. The seam matches on this;
    // the wrapper itself must never carry it, or it would collide with the
    // native handler's own symbol at link time.
    std::vector<std::string> all_base_names;

    hout << "extern \"C\" {\n";
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name;
      if (symbols_ && symbols_->has_symbol(start_addr)) {
        func_name = symbols_->get_symbol(start_addr);
      } else {
        std::stringstream ss;
        ss << "func_" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << start_addr;
        func_name = ss.str();
      }
      all_base_names.push_back(bare_symbol(func_name));
      if (emitted_names.count(func_name)) {
        std::stringstream ss;
        ss << func_name << "_" << std::hex << std::uppercase << start_addr;
        func_name = ss.str();
      }
      emitted_names.insert(func_name);
      all_func_names.push_back(func_name);

      hout << "void " << func_name << "(nwii::runtime::CPUContext& ctx);\n";
    }
    hout << "}\n";
    hout.close();

    int file_idx = 0;
    int func_idx = 0;
    int count_in_current_file = 0;
    std::ofstream out;

    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      if (count_in_current_file == 0) {
        if (out.is_open()) {
          out.close();
          std::cout << "[Recompiler] Generated " << generated_files.back()
                    << "\n";
        }
        std::stringstream ss;
        ss << "output_" << file_idx++ << ".cpp";
        std::string fname = ss.str();
        generated_files.push_back(fname);
        out.open(config_.output_dir + "/" + fname);
        emit_headers(out);
        out << "#include \"functions.h\"\n\n";
      }

      std::string func_name = all_func_names[func_idx];
      const std::string &base_name = all_base_names[func_idx++];
      if (sig_hooked(func) || is_hle_function(base_name)) {
        std::string target =
            sig_hooked(func) ? func.hle_hook_name : base_name;
        if (target != func_name) {
          out << "void " << func_name << "(nwii::runtime::CPUContext& ctx) {\n";
          out << "    " << target << "(ctx);\n";
          out << "    ctx.pc = ctx.lr;\n";
          out << "}\n\n";
        }
      } else {
        cur_entry_ = start_addr;
        emit_function(out, func, func_name);
      }

      count_in_current_file += func.instructions.size();
      if (count_in_current_file >= config_.instructions_per_file) {
        count_in_current_file = 0;
      }
    }
    if (out.is_open()) {
      out.close();
      std::cout << "[Recompiler] Generated " << generated_files.back() << "\n";
    }

    std::string main_name = "main_output.cpp";
    generated_files.push_back(main_name);
    out.open(config_.output_dir + "/" + main_name);
    // Data only: needs the table types and the function declarations, not
    // the runtime headers (so runtime header edits don't rebuild it).
    out << "#include \"runtime/dispatch.h\"\n";
    out << "#include \"functions.h\"\n\n";
    emit_dispatch(out, all_func_names, all_base_names);
    out.close();
    std::cout << "[Recompiler] Generated " << main_name << "\n";
  } else {
    
    std::string fname = "output.cpp";
    generated_files.push_back(fname);
    std::ofstream out(config_.output_dir + "/" + fname);
    emit_headers(out);

    std::set<std::string> emitted_names;
    std::vector<std::string> all_func_names;
    std::vector<std::string> all_base_names;

    out << "// --- Forward Declarations ---\n";
    out << "extern \"C\" {\n";
    out << "void OSReport(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Open(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_OpenAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Close(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Read(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Ioctl(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_IoctlAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Ioctlv(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_IoctlvAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void iosAlloc(nwii::runtime::CPUContext& ctx);\n";
    out << "void iosFree(nwii::runtime::CPUContext& ctx);\n";
    out << "void VIInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void PADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void VISetBlack(nwii::runtime::CPUContext& ctx);\n";
    out << "void VIGetNextField(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSDisableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSEnableInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSRestoreInterrupts(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetTime(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSTicksToMilliseconds(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSSetArenaLo(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSSetArenaHi(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInitFifoBase(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXInitFifoPtrs(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXGetCPUFifo(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCopyClear(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXBegin(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXEnd(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetVtxDesc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetVtxAttrFmt(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadPosMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXCallDisplayList(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCullMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetNumTevStages(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetBlendMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZMode(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetZCompLoc(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetAlphaCompare(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetDstAlpha(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetScissor(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadNrmMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXLoadTexMtxImm(nwii::runtime::CPUContext& ctx);\n";
    out << "void GXSetCurrentMtx(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Write(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_Seek(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_CloseAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_ReadAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_WriteAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void IOS_SeekAsync(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXAcquireVoice(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXFreeVoice(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceState(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceMix(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceAdpcm(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceSrc(nwii::runtime::CPUContext& ctx);\n";
    out << "void AXSetVoiceOffsets(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDOpen(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDReadAsyncPrio(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDClose(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDGetDriveStatus(nwii::runtime::CPUContext& ctx);\n";
    out << "void DVDReadPrio(nwii::runtime::CPUContext& ctx);\n";
    out << "void PADRead(nwii::runtime::CPUContext& ctx);\n";
    out << "void WPADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void WPADRead(nwii::runtime::CPUContext& ctx);\n";
    out << "void KPADInit(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSInitAlloc(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSCreateHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSAllocFromHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSFreeToHeap(nwii::runtime::CPUContext& ctx);\n";
    out << "void OSGetCurrentThread(CPUContext& ctx);\n";
    out << "void OSSleepThread(CPUContext& ctx);\n";
    out << "}\n";
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name;
      if (symbols_ && symbols_->has_symbol(start_addr)) {
        func_name = symbols_->get_symbol(start_addr);
      } else {
        std::stringstream ss;
        ss << "func_" << std::hex << std::uppercase << std::setfill('0')
           << std::setw(8) << start_addr;
        func_name = ss.str();
      }
      all_base_names.push_back(bare_symbol(func_name));
      if (emitted_names.count(func_name)) {
        std::stringstream ss;
        ss << func_name << "_" << std::hex << std::uppercase << start_addr;
        func_name = ss.str();
      }
      emitted_names.insert(func_name);
      all_func_names.push_back(func_name);
      out << "void " << func_name << "(CPUContext& ctx);\n";
    }

    out << "\n// --- Function Bodies ---\n";
    int func_idx = 0;
    for (const auto &[start_addr, func] : analyzer_.get_functions()) {
      std::string func_name = all_func_names[func_idx];
      const std::string &base_name = all_base_names[func_idx++];
      if (sig_hooked(func) || is_hle_function(base_name)) {
        out << "// [HLE Hook] Skipping generation for " << func_name << "\n\n";
      } else {
        cur_entry_ = start_addr;
        emit_function(out, func, func_name);
      }
    }

    out << "\n#include \"runtime/dispatch.h\"\n\n";
    emit_dispatch(out, all_func_names, all_base_names);
  }

  return generated_files;
}

bool Recompiler::generate_cmake_project(uint32_t entry_point) {
  if (!std::filesystem::exists(config_.output_dir)) {
    std::filesystem::create_directories(config_.output_dir);
  }

  std::vector<std::string> generated_files = generate_cpp(entry_point);
  if (generated_files.empty()) {
    return false;
  }

  // Copy the runtime sources, and only the sources.
  //
  // A plain recursive copy takes the source tree's own `build` directory with
  // it — CMake caches, object files, fetched dependencies and their `.git`
  // packs. Those are read-only inside git object stores, so the copy dies on
  // the first pack file and the whole export fails. It also happens to be the
  // last step, so everything generated up to that point was silently thrown
  // away, including the SDK symbol report.
  std::string runtime_dest = config_.output_dir + "/nWiiRuntime";
  try {
    namespace fs = std::filesystem;
    const fs::path src = config_.runtime_source_dir;
    const auto opts = fs::copy_options::overwrite_existing;

    auto skip = [](const fs::path &rel) {
      for (const auto &part : rel)
      {
        const std::string s = part.string();
        if (s == "build" || s == ".git" || s == ".cache" || s == ".DS_Store")
          return true;
      }
      return false;
    };

    fs::create_directories(runtime_dest);
    for (auto it = fs::recursive_directory_iterator(src);
         it != fs::recursive_directory_iterator(); ++it)
    {
      const fs::path rel = fs::relative(it->path(), src);
      if (skip(rel))
      {
        if (it->is_directory()) it.disable_recursion_pending();
        continue;
      }
      const fs::path dest = fs::path(runtime_dest) / rel;
      if (it->is_directory())
        fs::create_directories(dest);
      else if (it->is_regular_file())
        fs::copy_file(it->path(), dest, opts);
    }
  } catch (const std::exception &e) {
    std::cerr << "Failed to copy runtime from " << config_.runtime_source_dir
              << ": " << e.what() << "\n";
    return false;
  }

  std::string cmake_path = config_.output_dir + "/CMakeLists.txt";
  std::ofstream out(cmake_path);
  if (!out.is_open())
    return false;

  out << "cmake_minimum_required(VERSION 3.20)\n";

  
  out << "project(" << config_.project_name << " LANGUAGES C CXX)\n";
  // The runtime's Metal renderer is Objective-C++; enable it project-wide.
  out << "if(APPLE)\n    enable_language(OBJC OBJCXX)\nendif()\n\n";
  out << "set(CMAKE_POLICY_VERSION_MINIMUM 3.5)\n";
  out << "set(CMAKE_CXX_STANDARD 20)\n";
  out << "set(CMAKE_CXX_STANDARD_REQUIRED ON)\n\n";

  
  
  out << "include(FetchContent)\n";
  out << "FetchContent_Declare(\n";
  out << "    tomlplusplus\n";
  out << "    GIT_REPOSITORY https://github.com/marzer/tomlplusplus.git\n";
  out << "    GIT_TAG        v3.4.0\n";
  out << ")\n";
  out << "FetchContent_MakeAvailable(tomlplusplus)\n\n";
  out << "FetchContent_Declare(\n";
  out << "    SDL2\n";
  out << "    GIT_REPOSITORY https://github.com/libsdl-org/SDL.git\n";
  out << "    GIT_TAG        release-2.28.5\n";
  out << ")\n";
  out << "set(SDL2_DISABLE_INSTALL ON CACHE BOOL \"\" FORCE)\n";
  out << "FetchContent_MakeAvailable(SDL2)\n\n";

  

  out << "if(APPLE)\n";
  out << "    set(_sdl_gl_src "
         "\"${sdl2_SOURCE_DIR}/src/video/cocoa/SDL_cocoaopengl.m\")\n";
  out << "    if(EXISTS \"${_sdl_gl_src}\")\n";
  out << "        file(READ \"${_sdl_gl_src}\" _sdl_gl_txt)\n";
  out << "        string(REPLACE \"attr[i++] = "
         "NSOpenGLPFAAllowOfflineRenderers;\"\n";
  out << "                       \"/* NWii: breaks pixel format on dual-GPU "
         "Macs */\"\n";
  out << "                       _sdl_gl_patched \"${_sdl_gl_txt}\")\n";
  out << "        if(NOT _sdl_gl_txt STREQUAL _sdl_gl_patched)\n";
  out << "            file(WRITE \"${_sdl_gl_src}\" \"${_sdl_gl_patched}\")\n";
  out << "        endif()\n";
  out << "    endif()\n";
  out << "endif()\n\n";

  out << "add_subdirectory(nWiiRuntime)\n\n";

  out << "add_executable(" << config_.project_name
      << " nWiiRuntime/src/core/main.cpp";
  for (const auto &file : generated_files) {
    out << " " << file;
  }
  out << ")\n";

  out << "target_include_directories(" << config_.project_name
      << " PRIVATE nWiiRuntime/src nWiiRuntime/include)\n";

  
  out << "target_link_libraries(" << config_.project_name
      << " PRIVATE nwiiruntime)\n";
  // A linker map names runtime code in NWII_NPROF profiles.
  // /GS-: generated functions keep guest state in ctx, not in stack buffers,
  // so the security cookie checks were pure overhead (~2% of the CPU thread).
  out << "if(MSVC)\n    target_link_options(" << config_.project_name
      << " PRIVATE /MAP)\n    target_compile_options(" << config_.project_name
      << " PRIVATE /GS-)\nendif()\n";

  // Optional differential test (recompiled instruction bodies vs the
  // interpreter), generated into ../difftest; never built by default.
  out << "\nif(EXISTS \"${CMAKE_SOURCE_DIR}/../difftest/gen/table.cpp\")\n"
         "    file(GLOB NWII_DIFFTEST_SRC \"${CMAKE_SOURCE_DIR}/../difftest/gen/*.cpp\")\n"
         "    add_executable(nwii_difftest EXCLUDE_FROM_ALL ../difftest/main.cpp ${NWII_DIFFTEST_SRC})\n"
         "    target_include_directories(nwii_difftest PRIVATE nWiiRuntime/src nWiiRuntime/include ../difftest)\n"
         "    target_link_libraries(nwii_difftest PRIVATE nwiiruntime)\n"
         "endif()\n";

  return true;
}

std::string Recompiler::generate_function_cpp(const analyzer::Function &func) {
  std::stringstream ss;

  std::string func_name;
  if (symbols_ && symbols_->has_symbol(func.start_address)) {
    func_name = symbols_->get_symbol(func.start_address);
  } else {
    std::stringstream ss_name;
    ss_name << "func_" << std::hex << std::uppercase << std::setfill('0')
            << std::setw(8) << func.start_address;
    func_name = ss_name.str();
  }

  emit_function(ss, func, func_name);
  return ss.str();
}

void Recompiler::emit_function(std::ostream &out,
                               const analyzer::Function &func,
                               const std::string &func_name) {
  // NWII_RESTRICT (__restrict where supported): guest RAM stores go through
  // byte pointers, so without it the compiler reloads guest registers from
  // ctx after every store. Calls that receive ctx still reload as usual.
  out << "void " << func_name << "(CPUContext& NWII_RESTRICT ctx) {\n";



  auto hook = config_.hle_hooks.find(func.start_address);
  if (hook != config_.hle_hooks.end()) {
    out << "    " << hook->second << "(ctx);\n";
    out << "    ctx.pc = ctx.lr; return;\n";
    // The replaced instructions, as comments only, in the usual format: tools
    // can derive a native for the hook from them (game code is never stored
    // with a project; it is regenerated from the user's disc).
    for (const auto &inst : func.instructions)
      out << "    // 0x" << std::hex << std::uppercase << std::setfill('0') << std::setw(8)
          << inst.address << ": " << std::setw(8) << inst.opcode << std::dec << "\n";
    out << "}\n\n";
    return;
  }

  

  
  
  bool all_zero = !func.instructions.empty();
  for (const auto &inst : func.instructions) {
    if (inst.opcode != 0) {
      all_zero = false;
      break;
    }
  }
  bool os_low_mem = (func.start_address & 0x3FFFFFFF) < 0x3100;
  if (all_zero || os_low_mem) {
    out << "    nwii::runtime::interpret_step(ctx);\n";
    out << "}\n\n";
    return;
  }

  
  out << "    if (nwii::runtime::g_trace_calls && ctx.pc == 0x" << std::hex
      << std::uppercase << func.start_address << std::dec
      << ") nwii::runtime::trace_call(0x" << std::hex << std::uppercase
      << func.start_address << std::dec << ", ctx);\n";
  out << "    if (nwii::runtime::g_interp_mode && 0x" << std::hex << std::uppercase
      << func.start_address << std::dec
      << "u < nwii::runtime::g_interp_limit) { nwii::runtime::interp_run(ctx); return; }\n";

  std::set<uint32_t> valid_addrs;
  for (const auto &inst : func.instructions) {
    valid_addrs.insert(inst.address);
  }

  // The body is generated first: with lean_entry_switch the entry switch
  // depends on which addresses the body stores into ctx.pc.
  std::ostringstream body;
  if (!config_.count_instructions)
    body << "    ++ctx.inst_count; // once per call (count_instructions = false)\n";
  for (const auto &inst : func.instructions) {
    if (config_.count_instructions)
      body << "    ++ctx.inst_count;\n";
    emit_instruction(body, inst, func);
  }
  const std::string body_text = body.str();

  std::set<uint32_t> entry_addrs;
  if (config_.lean_entry_switch) {
    if (!extern_targets_ready_) {
      extern_targets_ready_ = true;
      for (const auto &[start, f] : analyzer_.get_functions()) {
        std::set<uint32_t> own;
        for (const auto &in : f.instructions) own.insert(in.address);
        for (const auto &in : f.instructions) {
          ppc::Instruction pi(in.opcode);
          if (pi.opcode() == 18 || pi.opcode() == 16) {
            const uint32_t t = pi.branch_target(in.address);
            if (!own.count(t)) extern_targets_.insert(t);
          }
        }
      }
    }
    auto add = [&](uint32_t a) {
      if (valid_addrs.count(a)) entry_addrs.insert(a);
    };
    for (const auto &inst : func.instructions) {
      ppc::Instruction pi(inst.opcode);
      if (pi.is_branch_link() || pi.opcode() == 17) add(inst.address + 4); // return addresses
      if (pi.opcode() == 16 || pi.opcode() == 18) {
        const uint32_t t = pi.branch_target(inst.address);
        if (t <= inst.address && valid_addrs.count(t)) add(inst.address); // back-edge poll
      }
      if (extern_targets_.count(inst.address)) add(inst.address);
      // Addresses that are join points anyway cost nothing extra as entries,
      // and they cover jump tables the analyzer missed (switch cases are
      // reached via bctr): local branch targets, and addresses right after
      // an unconditional transfer (reachable only by a jump).
      if (pi.opcode() == 16 || pi.opcode() == 18) add(pi.branch_target(inst.address));
      const bool uncond_branch = pi.opcode() == 18 && !pi.lk();
      const bool uncond_indirect = pi.is_unconditional_indirect_branch() && !pi.lk();
      if (uncond_branch || uncond_indirect) add(inst.address + 4);
    }
    for (uint32_t t : func.jump_table_targets) add(t);
    add(cur_entry_); // the dispatcher routes the function's key here
    // Every constant the body stores into ctx.pc (poll/mtmsr/call resume).
    static const std::string kPc = "ctx.pc = 0x";
    for (size_t p = body_text.find(kPc); p != std::string::npos;
         p = body_text.find(kPc, p + kPc.size())) {
      add((uint32_t)std::strtoul(body_text.c_str() + p + kPc.size(), nullptr, 16));
    }
  } else {
    entry_addrs = valid_addrs;
  }

  if (valid_addrs.size() > 1) {
    out << "    if (ctx.pc != 0x" << std::hex << std::uppercase
        << func.start_address << std::dec << ") {\n";
    out << "        switch (ctx.pc) {\n";
    for (uint32_t addr : entry_addrs) {
      if (addr != func.start_address) {
        out << "            case 0x" << std::hex << std::uppercase << addr
            << std::dec << ": goto loc_" << std::hex << std::uppercase << addr
            << std::dec << ";\n";
      }
    }
    out << "            case 0x0: return; // idle: no runnable thread\n";

    
    
    out << "            default: nwii::runtime::interpret_step(ctx); return;\n";
    out << "        }\n";
    out << "    }\n";

  }
  out << body_text;

  out << "}\n\n";
}

void Recompiler::emit_instruction(std::ostream &out,
                                  const analyzer::Instruction &inst,
                                  const analyzer::Function &func) {
  ppc::Instruction ppc_inst(inst.opcode);

  out << "loc_" << std::hex << std::uppercase << inst.address << std::dec
      << ": ;\n";
  out << "    // 0x" << std::hex << std::setfill('0') << std::setw(8)
      << inst.address << ": ";
  out << std::setfill('0') << std::setw(8) << inst.opcode << std::dec << "\n";

  if (ppc_inst.opcode() == 14) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();

    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = " << simm << "; // li r" << rD << ", "
          << simm << "\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] + " << simm
          << "; // addi r" << rD << ", r" << rA << ", " << simm << "\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 444) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();
    if (rS == rB) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "]; // mr r" << rA
          << ", r" << rS << "\n";
    } else {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] | ctx.gpr["
          << rB << "]; // or r" << rA << ", r" << rS << ", r" << rB << "\n";
    }

    
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      out << "    ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 339) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
    uint32_t spr_top = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t spr = (spr_top << 5) | spr_bottom;
    if (spr == 8) {
      out << "    ctx.gpr[" << rD << "] = ctx.lr; // mflr r" << rD << "\n";
    } else if (spr == 9) {
      out << "    ctx.gpr[" << rD << "] = ctx.ctr; // mfctr r" << rD << "\n";
    } else if (spr == 26) {
      out << "    ctx.gpr[" << rD << "] = ctx.srr0; // mfsrr0 r" << rD << "\n";
    } else if (spr == 27) {
      out << "    ctx.gpr[" << rD << "] = ctx.srr1; // mfsrr1 r" << rD << "\n";
    } else if (spr == 22) {
      out << "    ctx.gpr[" << rD << "] = ctx.read_dec(); // mfdec r" << rD << "\n";
    } else if (spr >= 912 && spr <= 919) {
      out << "    ctx.gpr[" << rD << "] = ctx.gqr[" << (spr - 912)
          << "]; // mfgqr" << (spr - 912) << " r" << rD << "\n";
    } else if (spr == 1) {
      out << "    ctx.gpr[" << rD << "] = ctx.xer; // mfxer r" << rD << "\n";
    } else if (spr >= 920 && spr <= 924) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr HID " << spr << "\n";
    } else if (spr == 1008) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr HID0 (stub)\n";
    } else if (spr == 287) {
      out << "    ctx.gpr[" << rD
          << "] = (nwii::runtime::Config::get().platform == "
             "nwii::runtime::Platform::GameCube) ? 0x00083214 : 0x00087102; // "
             "mfspr PVR\n";
    } else if (spr == 1017) {
      out << "    ctx.gpr[" << rD << "] = 0; // mfspr L2CR\n";
    } else {
      out << "    std::cerr << \"[WARN] mfspr r\" << " << rD
          << " << \" spr=\" << " << spr << " << \" (stub=0)\\n\";\n";
      out << "    ctx.gpr[" << rD << "] = 0; // unknown spr stub\n";
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 467) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
    uint32_t spr_top = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t spr = (spr_top << 5) | spr_bottom;
    if (spr == 8) {
      out << "    ctx.lr = ctx.gpr[" << rS << "]; // mtlr r" << rS << "\n";
    } else if (spr == 9) {
      out << "    ctx.ctr = ctx.gpr[" << rS << "]; // mtctr r" << rS << "\n";
    } else if (spr == 26) {
      out << "    ctx.srr0 = ctx.gpr[" << rS << "]; // mtsrr0 r" << rS << "\n";
    } else if (spr == 27) {
      out << "    ctx.srr1 = ctx.gpr[" << rS << "]; // mtsrr1 r" << rS << "\n";
    } else if (spr == 22) {
      out << "    ctx.write_dec(ctx.gpr[" << rS << "]); // mtdec r" << rS << "\n";
    } else if (spr >= 912 && spr <= 919) {
      out << "    ctx.gqr[" << (spr - 912) << "] = ctx.gpr[" << rS
          << "]; // mtgqr" << (spr - 912) << " r" << rS << "\n";
    } else if (spr == 1) {
      out << "    ctx.xer = ctx.gpr[" << rS << "]; // mtxer r" << rS << "\n";
    } else {
      out << "    // unhandled mtspr " << spr
          << "\n"; 
    }
  } else if (ppc_inst.opcode() == 31 && ppc_inst.extended_opcode() == 0) {
    
    uint32_t crD = ppc_inst.rd() >> 2; 
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();

    out << "    ctx.cr[" << crD << "].lt = ((int32_t)ctx.gpr[" << rA
        << "] < (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].gt = ((int32_t)ctx.gpr[" << rA
        << "] > (int32_t)ctx.gpr[" << rB << "]);\n";
    out << "    ctx.cr[" << crD << "].eq = ((int32_t)ctx.gpr[" << rA
        << "] == (int32_t)ctx.gpr[" << rB << "]);\n";
  } else if (ppc_inst.opcode() == 32) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(" << simm
          << "); // lwz r" << rD << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
          << "] + " << simm << "); // lwz r" << rD << ", " << simm << "(r" << rA
          << ")\n";
    }
  } else if (ppc_inst.opcode() == 33) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
        << "]); // lwzu r" << rD << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 34) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(" << simm
          << "); // lbz r" << rD << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
          << "] + " << simm << "); // lbz r" << rD << ", " << simm << "(r" << rA
          << ")\n";
    }
  } else if (ppc_inst.opcode() == 35) {
    
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
        << "]); // lbzu r" << rD << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 36) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write32(" << simm << ", ctx.gpr[" << rS
          << "]); // stw r" << rS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + " << simm
          << ", ctx.gpr[" << rS << "]); // stw r" << rS << ", " << simm << "(r"
          << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 37) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t ea = ctx.gpr[" << rA << "] + " << simm << ";\n";
    out << "        ctx.mmu.write32(ea, ctx.gpr[" << rS << "]);\n";
    out << "        ctx.gpr[" << rA << "] = ea;\n";
    out << "    } // stwu r" << rS << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 38) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write8(" << simm << ", (uint8_t)ctx.gpr[" << rS
          << "]); // stb r" << rS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + " << simm
          << ", (uint8_t)ctx.gpr[" << rS << "]); // stb r" << rS << ", " << simm
          << "(r" << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 39) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t ea = ctx.gpr[" << rA << "] + " << simm << ";\n";
    out << "        ctx.mmu.write8(ea, (uint8_t)ctx.gpr[" << rS << "]);\n";
    out << "        ctx.gpr[" << rA << "] = ea;\n";
    out << "    } // stbu r" << rS << ", " << simm << "(r" << rA << ")\n";
  } else if (ppc_inst.opcode() == 16) {
    
    uint32_t bo = ppc_inst.bo();
    uint32_t bi = ppc_inst.bi();
    uint32_t cr_idx = bi / 4;
    uint32_t cr_bit = bi % 4;

    std::string cond_field;
    if (cr_bit == 0)
      cond_field = "lt";
    else if (cr_bit == 1)
      cond_field = "gt";
    else if (cr_bit == 2)
      cond_field = "eq";
    else
      cond_field = "so";

    int16_t bd = ppc_inst.simm() & 0xFFFC; 
    int32_t target = bd;                   
    if ((inst.opcode & 2) == 2) {          
      target = bd;                         
      out << "    // Absolute branches not fully supported in simple string "
             "emit\n";
    } else {
      target = inst.address + bd;
    }

    std::stringstream ss;
    ss << "loc_" << std::hex << std::uppercase << target;
    std::string target_lbl = ss.str();

    bool is_local =
        std::find_if(func.instructions.begin(), func.instructions.end(),
                     [target](const auto &i) { return i.address == target; }) !=
        func.instructions.end();

    if (target <= inst.address && is_local) {

      out << "    ctx.pc = 0x" << std::hex << std::uppercase << inst.address
          << std::dec << ";\n";
      out << "    if (nwii::runtime::poll_callbacks(ctx)) return;\n";
      out << "    if (nwii::runtime::g_verbose && (ctx.inst_count % 10000000) == 0) { std::cout << "
             "\"Spinning at PC: 0x\" << std::hex << 0x"
          << std::uppercase << std::hex << inst.address << std::dec
          << " << \" LR: 0x\" << std::hex << ctx.lr << std::dec << std::endl; "
             "}\n";
    }

    std::string ext_name;
    if (!is_local) {
      if (symbols_ && symbols_->has_symbol(target)) {
        ext_name = symbols_->get_symbol(target);
        if (ext_name.find("loc_") == 0)
          ext_name.replace(0, 4, "func_");
      } else {
        std::stringstream ss_ext;
        ss_ext << "func_" << std::hex << std::uppercase << std::setfill('0')
               << std::setw(8) << target;
        ext_name = ss_ext.str();
      }
    }

    bool bit0 = (bo & 0x10) != 0;
    bool bit1 = (bo & 0x08) != 0;
    bool bit2 = (bo & 0x04) != 0;
    bool bit3 = (bo & 0x02) != 0;

    bool dec_ctr = !bit2;
    bool test_ctr = !bit2;
    bool test_cr = !bit0;

    std::vector<std::string> conditions;
    if (test_ctr) {
      if (bit3)
        conditions.push_back("ctx.ctr == 0");
      else
        conditions.push_back("ctx.ctr != 0");
    }
    if (test_cr) {
      if (bit1)
        conditions.push_back("ctx.cr[" + std::to_string(cr_idx) + "]." +
                             cond_field);
      else
        conditions.push_back("!ctx.cr[" + std::to_string(cr_idx) + "]." +
                             cond_field);
    }

    std::string cond_expr;
    if (conditions.empty())
      cond_expr = "true";
    else if (conditions.size() == 1)
      cond_expr = conditions[0];
    else
      cond_expr = conditions[0] + " && " + conditions[1];

    if (dec_ctr)
      out << "    ctx.ctr--;\n";

    if (ppc_inst.lk())
      out << "    ctx.lr = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << "; // save return address\n";

    if (is_local) {
      if (cond_expr == "true")
        out << "    goto " << target_lbl << ";\n";
      else
        out << "    if (" << cond_expr << ") goto " << target_lbl << ";\n";
    } else {
      bool is_mapped = true;
      if (!symbols_ || !symbols_->has_symbol(target)) {
        if (analyzer_.get_functions().find(target) ==
            analyzer_.get_functions().end()) {
          is_mapped = false;
        }
      }
      if (is_mapped) {
        if (cond_expr == "true") {
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; " << ext_name << "(ctx); return;\n";
        } else {
          out << "    if (" << cond_expr << ") { ctx.pc = 0x" << std::hex
              << std::uppercase << target << std::dec << "; " << ext_name
              << "(ctx); return; }\n";
        }
      } else {
        if (cond_expr == "true") {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return;\n";
        } else {
          out << "    if (" << cond_expr << ") { ctx.pc = 0x" << std::hex
              << target << std::dec << "; return; }\n";
        }
      }
    }
  } else if (ppc_inst.opcode() == 18) {
    
    uint32_t target = ppc_inst.branch_target(inst.address);
    std::stringstream ss;
    std::string target_name;

    bool is_local =
        std::find_if(func.instructions.begin(), func.instructions.end(),
                     [target](const auto &i) { return i.address == target; }) !=
        func.instructions.end();

    if (target <= inst.address && is_local) {

      out << "    ctx.pc = 0x" << std::hex << std::uppercase << inst.address
          << std::dec << ";\n";
      out << "    if (nwii::runtime::poll_callbacks(ctx)) return;\n";
      out << "    if (nwii::runtime::g_verbose && (ctx.inst_count % 10000000) == 0) { std::cout << "
             "\"Spinning at PC: 0x\" << std::hex << 0x"
          << std::uppercase << std::hex << inst.address << std::dec
          << " << \" LR: 0x\" << std::hex << ctx.lr << std::dec << std::endl; "
             "}\n";
    }

    if (symbols_ && symbols_->has_symbol(target)) {
      target_name = symbols_->get_symbol(target);
      if (target_name.find("loc_") == 0)
        target_name.replace(0, 4, "func_");
    } else {
      std::stringstream ss_name;
      ss_name << "func_" << std::hex << std::uppercase << std::setfill('0')
              << std::setw(8) << target;
      target_name = ss_name.str();
    }

    if (ppc_inst.is_branch_link()) {
      out << "    ctx.lr = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << "; // save return address\n";
      if (is_local) {
        out << "    goto loc_" << std::hex << std::uppercase
            << std::setfill('0') << std::setw(8) << target << ";\n";
      } else {
        bool is_mapped = true;
        if (!symbols_ || !symbols_->has_symbol(target)) {
          if (analyzer_.get_functions().find(target) ==
              analyzer_.get_functions().end()) {
            is_mapped = false;
          }
        }
        if (is_mapped) {
          bool target_is_hle = (symbols_ && symbols_->has_symbol(target) &&
                                is_hle_function(symbols_->get_symbol(target)));
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; ";
          out << target_name << "(ctx);\n";
          out << "    if (ctx.pc != 0x" << std::hex << std::uppercase
              << (inst.address + 4) << std::dec << ") return;\n";
        } else {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return; // bl to unmapped target\n";
        }
      }
    } else {
      if (is_local) {
        out << "    goto loc_" << std::hex << std::uppercase
            << std::setfill('0') << std::setw(8) << target << ";\n";
      } else {
        bool is_mapped = true;
        if (!symbols_ || !symbols_->has_symbol(target)) {
          if (analyzer_.get_functions().find(target) ==
              analyzer_.get_functions().end()) {
            is_mapped = false;
          }
        }
        if (is_mapped) {
          out << "    ctx.pc = 0x" << std::hex << std::uppercase << target
              << std::dec << "; return;\n";
        } else {
          out << "    ctx.pc = 0x" << std::hex << target << std::dec
              << "; return;\n";
        }
      }
    }
  } else if (ppc_inst.opcode() == 19) {
    uint32_t xo = ppc_inst.extended_opcode();
    if (xo == 150) { 
      out << "    // isync\n";
    } else if (xo == 50) { 
        out << "    {\n";
        out << "        uint32_t ee_was = ctx.msr & 0x8000;\n";
        out << "        ctx.msr = ctx.srr1; // rfi: restore MSR from SRR1\n";
        out << "        ctx.pc = ctx.srr0;  // rfi: jump to SRR0\n";
        out << "        ctx.dispatch_saved_ctx = 0; // exit ISR\n";
        out << "        if (!ee_was && (ctx.msr & 0x8000)) {\n";
        out << "            if (process_pending_callbacks(ctx)) return;\n";
        out << "        }\n";
        out << "    }\n";
        out << "    return;\n";
    } else if (xo == 16 || xo == 528) { 
      uint32_t bo = ppc_inst.bo();
      uint32_t bi = ppc_inst.bi();
      uint32_t cr_idx = bi / 4;
      uint32_t cr_bit = bi % 4;

      std::string cond_field;
      if (cr_bit == 0)
        cond_field = "lt";
      else if (cr_bit == 1)
        cond_field = "gt";
      else if (cr_bit == 2)
        cond_field = "eq";
      else
        cond_field = "so";

      std::string target_reg = (xo == 16) ? "ctx.lr" : "ctx.ctr";

      bool bit0 = (bo & 0x10) != 0;
      bool bit1 = (bo & 0x08) != 0;
      bool bit2 = (bo & 0x04) != 0;
      bool bit3 = (bo & 0x02) != 0;

      bool dec_ctr = !bit2;
      bool test_ctr = !bit2;
      bool test_cr = !bit0;

      std::vector<std::string> conditions;
      if (test_ctr) {
        if (bit3)
          conditions.push_back("ctx.ctr == 0");
        else
          conditions.push_back("ctx.ctr != 0");
      }
      if (test_cr) {
        if (bit1)
          conditions.push_back("ctx.cr[" + std::to_string(cr_idx) + "]." +
                               cond_field);
        else
          conditions.push_back("!ctx.cr[" + std::to_string(cr_idx) + "]." +
                               cond_field);
      }

      std::string cond_expr;
      if (conditions.empty())
        cond_expr = "true";
      else if (conditions.size() == 1)
        cond_expr = conditions[0];
      else
        cond_expr = conditions[0] + " && " + conditions[1];

      if (dec_ctr)
        out << "    ctx.ctr--;\n";

      if (ppc_inst.lk() && target_reg == "ctx.lr") {
        out << "    {\n";
        out << "        uint32_t temp_lr = ctx.lr;\n";
        out << "        ctx.lr = 0x" << std::hex << std::uppercase
            << (inst.address + 4) << std::dec << "; // save return address\n";
        if (cond_expr == "true") {
          out << "        ctx.pc = temp_lr; return;\n";
        } else {
          out << "        if (" << cond_expr
              << ") { ctx.pc = temp_lr; return; }\n";
        }
        out << "    }\n";
      } else {
        if (ppc_inst.lk()) {
          out << "    ctx.lr = 0x" << std::hex << std::uppercase
              << (inst.address + 4) << std::dec << "; // save return address\n";
        }

        
        bool null_guard = ppc_inst.lk() && target_reg == "ctx.ctr";
        if (cond_expr == "true") {
          // bctrl: call the target in place like a direct bl (no unwind to
          // the dispatcher and back through its resume switch); unknown
          // targets still take the dispatcher route.
          if (null_guard)
            out << "    if (" << target_reg << ") { ctx.pc = " << target_reg
                << "; if (!nwii::runtime::call_indirect(ctx) || ctx.pc != 0x"
                << std::hex << std::uppercase << (inst.address + 4) << std::dec
                << ") return; } else nwii::runtime::note_null_call(0x"
                << std::hex << std::uppercase << inst.address << std::dec
                << ", ctx.lr);\n";
          else
            out << "    ctx.pc = " << target_reg << "; return;\n";
        } else {
          if (null_guard)
            out << "    if ((" << cond_expr << ") && " << target_reg
                << ") { ctx.pc = " << target_reg << "; return; }\n";
          else
            out << "    if (" << cond_expr << ") { ctx.pc = " << target_reg
                << "; return; }\n";
        }
      }
    } else if (xo == 257 || xo == 129 || xo == 289 || xo == 225 || xo == 33 ||
               xo == 449 || xo == 417 || xo == 193) {
      uint32_t crbD = ppc_inst.rd();
      uint32_t crbA = ppc_inst.ra();
      uint32_t crbB = ppc_inst.rb();

      auto get_cr_bit_str = [](uint32_t bit_idx) {
        std::string field = "ctx.cr[" + std::to_string(bit_idx / 4) + "]";
        uint32_t bit = bit_idx % 4;
        if (bit == 0)
          return field + ".lt";
        if (bit == 1)
          return field + ".gt";
        if (bit == 2)
          return field + ".eq";
        return field + ".so";
      };

      std::string op;
      if (xo == 257)
        op = get_cr_bit_str(crbA) + " & " + get_cr_bit_str(crbB); 
      else if (xo == 129)
        op = get_cr_bit_str(crbA) + " & !" + get_cr_bit_str(crbB); 
      else if (xo == 289)
        op = get_cr_bit_str(crbA) + " == " + get_cr_bit_str(crbB); 
      else if (xo == 225)
        op = "!(" + get_cr_bit_str(crbA) + " & " + get_cr_bit_str(crbB) +
             ")"; 
      else if (xo == 33)
        op = "!(" + get_cr_bit_str(crbA) + " | " + get_cr_bit_str(crbB) +
             ")"; 
      else if (xo == 449)
        op = get_cr_bit_str(crbA) + " | " + get_cr_bit_str(crbB); 
      else if (xo == 417)
        op = get_cr_bit_str(crbA) + " | !" + get_cr_bit_str(crbB); 
      else if (xo == 193)
        op = get_cr_bit_str(crbA) + " ^ " + get_cr_bit_str(crbB); 

      out << "    " << get_cr_bit_str(crbD) << " = " << op << ";\n";
    } else if (xo == 0) {
      uint32_t crfD = ppc_inst.rd() >> 2;
      uint32_t crfS = ppc_inst.ra() >> 2;
      out << "    ctx.cr[" << crfD << "] = ctx.cr[" << crfS << "]; // mcrf\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED OPCODE 19 XO \" << " << xo
          << " << \" at 0x" << std::hex << inst.address << std::dec
          << "\\n\"; std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 48) {
    
    uint32_t fD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();

    
    
    if (rA == 0) {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD
          << "] = ctx.mmu.read_f32(" << simm << "); // lfs\n";
    } else {
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD
          << "] = ctx.mmu.read_f32(ctx.gpr[" << rA << "] + " << simm
          << "); // lfs\n";
    }
  } else if (ppc_inst.opcode() == 52) {
    
    uint32_t fS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    if (rA == 0) {
      out << "    ctx.mmu.write_f32(" << simm << ", (float)ctx.fpr[" << fS
          << "]); // stfs f" << fS << ", " << simm << "(0)\n";
    } else {
      out << "    ctx.mmu.write_f32(ctx.gpr[" << rA << "] + " << simm
          << ", (float)ctx.fpr[" << fS << "]); // stfs f" << fS << ", " << simm
          << "(r" << rA << ")\n";
    }
  } else if (ppc_inst.opcode() == 59) {
    uint32_t xo = (ppc_inst.value() >> 1) & 0x1F;
    uint32_t fD = ppc_inst.rd();
    uint32_t fA = ppc_inst.ra();
    uint32_t fB = ppc_inst.rb();
    uint32_t fC = ppc_inst.rc();

    
    if (xo == 21) {
      out << "    ctx.fpr[" << fD << "] = (float)(ctx.fpr[" << fA
          << "] + ctx.fpr[" << fB << "]); // fadds\n";
    } else if (xo == 20) {
      out << "    ctx.fpr[" << fD << "] = (float)(ctx.fpr[" << fA
          << "] - ctx.fpr[" << fB << "]); // fsubs\n";
    } else if (xo == 25) {
      out << "    ctx.fpr[" << fD << "] = (float)(ctx.fpr[" << fA
          << "] * ctx.fpr[" << fC << "]); // fmuls\n";
    } else if (xo == 18) {
      out << "    ctx.fpr[" << fD << "] = (float)(ctx.fpr[" << fA
          << "] / ctx.fpr[" << fB << "]); // fdivs\n";
    } else if (xo == 22) {
      out << "    ctx.fpr[" << fD << "] = (float)std::sqrt(ctx.fpr[" << fB
          << "]); // fsqrts\n";
    } else if (xo == 24) {
      out << "    ctx.fpr[" << fD << "] = (float)(1.0f / ctx.fpr[" << fB
          << "]); // fres\n";
    } else if (xo == 29) {
      out << "    ctx.fpr[" << fD << "] = (float)std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << fC << "], ctx.fpr[" << fB << "]); // fmadds\n";
    } else if (xo == 28) {
      out << "    ctx.fpr[" << fD << "] = (float)std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << fC << "], -ctx.fpr[" << fB << "]); // fmsubs\n";
    } else if (xo == 31) {
      out << "    ctx.fpr[" << fD << "] = (float)(-std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << fC << "], ctx.fpr[" << fB << "])); // fnmadds\n";
    } else if (xo == 30) {
      out << "    ctx.fpr[" << fD << "] = (float)(-std::fma(ctx.fpr[" << fA
          << "], ctx.fpr[" << fC << "], -ctx.fpr[" << fB << "])); // fnmsubs\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 59 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
    // Gekko writes a single-precision result to both paired-single slots;
    // compiled code later reads ps1 (ps_merge*, psq_st with W=0).
    out << "    ctx.ps1[" << fD << "] = ctx.fpr[" << fD << "];\n";
  } else if (ppc_inst.opcode() == 63) {
    uint32_t xo = (ppc_inst.value() >> 1) & 0x3FF;
    uint32_t fD = ppc_inst.rd();
    uint32_t fB = ppc_inst.rb();
    uint32_t fA = ppc_inst.ra();
    if (xo == 72) {
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fB << "]; // fmr\n";
    } else if (xo == 40) { 
      out << "    ctx.fpr[" << fD << "] = -ctx.fpr[" << fB << "]; // fneg\n";
    } else if (xo == 264) { 
      out << "    ctx.fpr[" << fD << "] = std::fabs(ctx.fpr[" << fB
          << "]); // fabs\n";
    } else if (xo == 136) { 
      out << "    ctx.fpr[" << fD << "] = -std::fabs(ctx.fpr[" << fB
          << "]); // fnabs\n";
    } else if (xo == 12) { 
      out << "    ctx.fpr[" << fD << "] = ctx.ps1[" << fD
          << "] = (double)(float)ctx.fpr[" << fB << "]; // frsp\n";
    } else if (xo == 15) { 
      out << "    {\n";
      out << "        uint32_t i_val = nwii::runtime::ppc_cvt_s32(std::trunc(ctx.fpr["
          << fB << "]));\n";
      out << "        uint64_t val = i_val;\n";
      out << "        std::memcpy(&ctx.fpr[" << fD << "], &val, 8);\n";
      out << "    } // fctiwz\n";
    } else if (xo == 14) { 
      out << "    {\n";
      out << "        uint32_t i_val = "
             "nwii::runtime::ppc_cvt_s32(std::nearbyint(ctx.fpr["
          << fB << "]));\n";
      out << "        uint64_t val = i_val;\n";
      out << "        std::memcpy(&ctx.fpr[" << fD << "], &val, 8);\n";
      out << "    } // fctiw\n";
    } else if (xo == 583) { 
      out << "    ctx.fpr[" << fD << "] = 0.0; // mffs stub\n";
    } else if (xo == 711) { 
      out << "    // mtfsf stub (ignore FPSCR write)\n";
    } else if ((xo & 0x1F) == 26) { 
      out << "    ctx.fpr[" << fD << "] = 1.0 / std::sqrt(ctx.fpr[" << fB
          << "]); // frsqrte\n";
    } else if ((xo & 0x1F) == 24) { 
      out << "    ctx.fpr[" << fD << "] = 1.0 / ctx.fpr[" << fB
          << "]; // fre\n";
    } else if ((xo & 0x1F) == 18) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] / ctx.fpr["
          << fB << "]; // fdiv\n";
    } else if ((xo & 0x1F) == 20) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] - ctx.fpr["
          << fB << "]; // fsub\n";
    } else if ((xo & 0x1F) == 21) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] + ctx.fpr["
          << fB << "]; // fadd\n";
    } else if ((xo & 0x1F) == 25) { 
      out << "    ctx.fpr[" << fD << "] = ctx.fpr[" << fA << "] * ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "]; // fmul\n";
    } else if ((xo & 0x1F) == 29) { // fused, one rounding, like Gekko
      out << "    ctx.fpr[" << fD << "] = std::fma(ctx.fpr[" << fA << "], ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "], ctx.fpr[" << fB
          << "]); // fmadd\n";
    } else if ((xo & 0x1F) == 28) {
      out << "    ctx.fpr[" << fD << "] = std::fma(ctx.fpr[" << fA << "], ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "], -ctx.fpr[" << fB
          << "]); // fmsub\n";
    } else if ((xo & 0x1F) == 31) {
      out << "    ctx.fpr[" << fD << "] = -std::fma(ctx.fpr[" << fA << "], ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "], ctx.fpr[" << fB
          << "]); // fnmadd\n";
    } else if ((xo & 0x1F) == 30) {
      out << "    ctx.fpr[" << fD << "] = -std::fma(ctx.fpr[" << fA << "], ctx.fpr["
          << ((ppc_inst.value() >> 6) & 0x1F) << "], -ctx.fpr[" << fB
          << "]); // fnmsub\n";
    } else if ((xo & 0x1F) == 23) { 
      out << "    ctx.fpr[" << fD << "] = (ctx.fpr[" << fA
          << "] >= 0.0) ? ctx.fpr[" << ((ppc_inst.value() >> 6) & 0x1F)
          << "] : ctx.fpr[" << fB << "]; // fsel\n";
    } else if (xo == 38 ||
               xo == 70) { 
      out << "    // mtfsb1/mtfsb0 stub (FPSCR ignored)\n";
    } else if (xo == 64) { 
      out << "    // mcrfs stub\n";
    } else if (xo == 0 || xo == 32) { 
      uint32_t crfD = fD >> 2;
      out << "    {\n";
      out << "        bool un = std::isnan(ctx.fpr[" << fA
          << "]) || std::isnan(ctx.fpr[" << fB << "]);\n";
      out << "        ctx.cr[" << crfD << "].lt = !un && (ctx.fpr[" << fA
          << "] < ctx.fpr[" << fB << "]);\n";
      out << "        ctx.cr[" << crfD << "].gt = !un && (ctx.fpr[" << fA
          << "] > ctx.fpr[" << fB << "]);\n";
      out << "        ctx.cr[" << crfD << "].eq = !un && (ctx.fpr[" << fA
          << "] == ctx.fpr[" << fB << "]);\n";
      out << "        ctx.cr[" << crfD << "].so = un;\n";
      out << "    }\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 63 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 20) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t mb = (ppc_inst.value() >> 6) & 0x1F;
    uint32_t me = (ppc_inst.value() >> 1) & 0x1F;

    uint32_t mask = 0;
    if (mb <= me) {
      for (int i = mb; i <= me; ++i)
        mask |= (1 << (31 - i));
    } else {
      for (int i = 0; i <= me; ++i)
        mask |= (1 << (31 - i));
      for (int i = mb; i <= 31; ++i)
        mask |= (1 << (31 - i));
    }

    out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rA << "] & ~0x"
        << std::hex << std::uppercase << mask << std::dec
        << ") | (std::rotl(ctx.gpr[" << rS << "], " << sh << ") & 0x"
        << std::hex << std::uppercase << mask << std::dec << "); // rlwimi r"
        << rA << ", r" << rS << ", " << sh << ", " << mb << ", " << me << "\n";
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
    }
  } else if (ppc_inst.opcode() == 21) {
    
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
    uint32_t mb = (ppc_inst.value() >> 6) & 0x1F;
    uint32_t me = (ppc_inst.value() >> 1) & 0x1F;

    uint32_t mask = 0;
    if (mb <= me) {
      for (int i = mb; i <= me; ++i)
        mask |= (1 << (31 - i));
    } else {
      for (int i = 0; i <= me; ++i)
        mask |= (1 << (31 - i));
      for (int i = mb; i <= 31; ++i)
        mask |= (1 << (31 - i));
    }

    out << "    ctx.gpr[" << rA << "] = std::rotl(ctx.gpr[" << rS << "], " << sh
        << ") & 0x" << std::hex << std::uppercase << mask << std::dec
        << "; // rlwinm r" << rA << ", r" << rS << ", " << sh << ", " << mb
        << ", " << me << "\n";
    if (ppc_inst.value() & 1) {
      out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
      out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
      out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
    }
  } else if (ppc_inst.opcode() == 31) {
    uint32_t xo = ppc_inst.extended_opcode();
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    uint32_t rB = ppc_inst.rb();
    uint32_t rS = ppc_inst.rs();

    if (xo == 598 || xo == 854 || xo == 982 || xo == 54 || xo == 86 ||
        xo == 278 || xo == 246 || xo == 470) {
      
      out << "    // sync/cache instruction\n";
    } else if (xo == 0) { 
      uint32_t crfD = rD >> 2;
      out << "    ctx.cr[" << crfD << "].lt = ((int32_t)ctx.gpr[" << rA
          << "] < (int32_t)ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].gt = ((int32_t)ctx.gpr[" << rA
          << "] > (int32_t)ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].eq = (ctx.gpr[" << rA
          << "] == ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].so = (ctx.xer >> 31) & 1;\n";
    } else if (xo == 32) { 
      uint32_t crfD = rD >> 2;
      out << "    ctx.cr[" << crfD << "].lt = (ctx.gpr[" << rA << "] < ctx.gpr["
          << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].gt = (ctx.gpr[" << rA << "] > ctx.gpr["
          << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].eq = (ctx.gpr[" << rA
          << "] == ctx.gpr[" << rB << "]);\n";
      out << "    ctx.cr[" << crfD << "].so = (ctx.xer >> 31) & 1;\n";
    } else if (xo == 11) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(((uint64_t)ctx.gpr[" << rA
          << "] * (uint64_t)ctx.gpr[" << rB << "]) >> 32); // mulhwu\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 75) { 
      out << "    ctx.gpr[" << rD
          << "] = (uint32_t)(((int64_t)(int32_t)ctx.gpr[" << rA
          << "] * (int64_t)(int32_t)ctx.gpr[" << rB << "]) >> 32); // mulhw\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 87) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rB
            << "]); // lbzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read8(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lbzx\n";
    } else if (xo == 215) {
      if (rA == 0)
        out << "    ctx.mmu.write8(ctx.gpr[" << rB << "], (uint8_t)ctx.gpr["
            << rS << "]); // stbx\n";
      else
        out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], (uint8_t)ctx.gpr[" << rS << "]); // stbx\n";
    } else if (xo == 279) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read16(ctx.gpr[" << rB
            << "]); // lhzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read16(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lhzx\n";
    } else if (xo == 371) { 
      uint32_t spr_bottom = (ppc_inst.value() >> 16) & 0x1F;
      uint32_t spr_top    = (ppc_inst.value() >> 11) & 0x1F;
      uint32_t spr = (spr_top << 5) | spr_bottom;
      if (spr == 269) { 
        out << "    ctx.gpr[" << rD << "] = (uint32_t)(ctx.read_timebase() >> 32); // mftbu\n";
      } else { 
        out << "    ctx.gpr[" << rD << "] = (uint32_t)(ctx.read_timebase() & 0xFFFFFFFF); // mftb\n";
      }

      
      
    } else if (xo == 407) {
      if (rA == 0)
        out << "    ctx.mmu.write16(ctx.gpr[" << rB << "], (uint16_t)ctx.gpr["
            << rS << "]); // sthx\n";
      else
        out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], (uint16_t)ctx.gpr[" << rS << "]); // sthx\n";
    } else if (xo == 23) {
      if (rA == 0)
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rB
            << "]); // lwzx\n";
      else
        out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
            << "] + ctx.gpr[" << rB << "]); // lwzx\n";
    } else if (xo == 151) {
      if (rA == 0)
        out << "    ctx.mmu.write32(ctx.gpr[" << rB << "], ctx.gpr[" << rS
            << "]); // stwx\n";
      else
        out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
            << "], ctx.gpr[" << rS << "]); // stwx\n";
    } else if (xo == 40) {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rB << "] - ctx.gpr["
          << rA << "]; // subf\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 235) {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] * ctx.gpr["
          << rB << "]; // mullw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 491) {
      // /0 and INT_MIN/-1 (the latter traps on x86): Gekko gives the
      // dividend's sign, 0 or -1 (as Dolphin does).
      out << "    if (ctx.gpr[" << rB << "] != 0 && !(ctx.gpr[" << rA
          << "] == 0x80000000u && ctx.gpr[" << rB << "] == 0xFFFFFFFFu)) ctx.gpr["
          << rD << "] = (uint32_t)((int32_t)ctx.gpr[" << rA << "] / (int32_t)ctx.gpr["
          << rB << "]); else ctx.gpr[" << rD << "] = ((int32_t)ctx.gpr[" << rA
          << "] < 0) ? 0xFFFFFFFFu : 0u; // divw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 459) {
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rB << "] ? ctx.gpr[" << rA
          << "] / ctx.gpr[" << rB << "] : 0u; // divwu (0 on /0, like Gekko)\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 954) {
      out << "    ctx.gpr[" << rA << "] = (uint32_t)(int32_t)(int8_t)ctx.gpr["
          << rS << "]; // extsb\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 922) {
      out << "    ctx.gpr[" << rA << "] = (uint32_t)(int32_t)(int16_t)ctx.gpr["
          << rS << "]; // extsh\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 28) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] & ctx.gpr["
          << rB << "]; // and\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 60) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] & ~ctx.gpr["
          << rB << "]; // andc\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 316) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] ^ ctx.gpr["
          << rB << "]; // xor\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 24) {
      out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rB
          << "] & 0x20) ? 0 : (ctx.gpr[" << rS << "] << (ctx.gpr[" << rB
          << "] & 0x1F)); // slw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 536) {
      out << "    ctx.gpr[" << rA << "] = (ctx.gpr[" << rB
          << "] & 0x20) ? 0 : (ctx.gpr[" << rS << "] >> (ctx.gpr[" << rB
          << "] & 0x1F)); // srw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 792) {
      // CA = negative source with 1-bits shifted out (srawi below likewise).
      out << "    {\n";
      out << "        int32_t sv = (int32_t)ctx.gpr[" << rS << "];\n";
      out << "        uint32_t sh = ctx.gpr[" << rB << "] & 0x3F;\n";
      out << "        uint32_t ca;\n";
      out << "        if (sh >= 32) { ctx.gpr[" << rA
          << "] = (uint32_t)(sv >> 31); ca = sv < 0; }\n";
      out << "        else { ctx.gpr[" << rA << "] = (uint32_t)(sv >> sh); "
             "ca = sv < 0 && sh && ((uint32_t)sv & ((1u << sh) - 1)); }\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);\n";
      out << "    } // sraw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 26) {
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS
          << "] == 0 ? 32 : std::countl_zero(ctx.gpr[" << rS
          << "]); // cntlzw\n";
      if (ppc_inst.value() & 1) { 
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
        out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
        out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 10) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)ctx.gpr[" << rB << "];\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 8) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)ctx.gpr[" << rB << "] + 1;\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 104 || xo == 616) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(-(int32_t)ctx.gpr[" << rA
          << "]); // neg\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 266) { 
      out << "    ctx.gpr[" << rD << "] = ctx.gpr[" << rA << "] + ctx.gpr["
          << rB << "];\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 138) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)ctx.gpr[" << rB
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 136) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)ctx.gpr[" << rB
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 202) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 200) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 234) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA
          << "] + 0xFFFFFFFFULL + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 232) { 
      out << "    {\n";
      out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << rA
          << "]) + 0xFFFFFFFFULL + (uint64_t)((ctx.xer >> 29) & 1);\n";
      out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
      out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
             "29); // Set CA bit\n";
      if (ppc_inst.value() & 1) {
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
      out << "    }\n";
    } else if (xo == 83) { 
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.msr;\n";
    } else if (xo == 146) { 

      

      
      out << "    {\n";
      out << "        uint32_t ee_was = ctx.msr & 0x8000;\n";
      out << "        ctx.msr = ctx.gpr[" << ppc_inst.rs() << "];\n";
      out << "        if (!ee_was && (ctx.msr & 0x8000)) {\n";
      out << "            ctx.pc = 0x" << std::hex << std::uppercase
          << (inst.address + 4) << std::dec << ";\n";
      out << "            if (process_pending_callbacks(ctx)) return;\n";
      out << "        }\n";
      out << "    }\n";
    } else if (xo == 124) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] | ctx.gpr["
          << rB << "]); // nor\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 476) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] & ctx.gpr["
          << rB << "]); // nand\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 284) { 
      out << "    ctx.gpr[" << rA << "] = ~(ctx.gpr[" << rS << "] ^ ctx.gpr["
          << rB << "]); // eqv\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 412) { 
      out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rS << "] | ~ctx.gpr["
          << rB << "]; // orc\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      }
    } else if (xo == 20) { 
      out << "    ctx.gpr[" << rD << "] = ctx.mmu.read32(ctx.gpr[" << rA
          << "] + ctx.gpr[" << rB << "]); // lwarx\n";
    } else if (xo == 150) { 
      out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], ctx.gpr[" << ppc_inst.rs() << "]); // stwcx\n";
      out << "    ctx.cr[0].eq = true; ctx.cr[0].lt = false; ctx.cr[0].gt = "
             "false; // always succeed\n";
    } else if (xo == 119) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read8(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lbzux\n";
    } else if (xo == 311) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read16(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lhzux\n";
    } else if (xo == 343) { 
      out << "    ctx.gpr[" << rD
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
          << "] + ctx.gpr[" << rB << "]); // lhax\n";
    } else if (xo == 375) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lhaux\n";
    } else if (xo == 247) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write8(ea, (uint8_t)ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // stbux\n";
    } else if (xo == 439) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write16(ea, (uint16_t)ctx.gpr[" << ppc_inst.rs()
          << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // sthux\n";
    } else if (xo == 824) { 

      

      uint32_t sh = (ppc_inst.value() >> 11) & 0x1F;
      out << "    {\n";
      out << "        int32_t sv = (int32_t)ctx.gpr[" << rD << "];\n";
      out << "        ctx.gpr[" << rA << "] = (uint32_t)(sv >> " << sh << ");\n";
      out << "        uint32_t ca = (sv < 0 && (uint32_t)(sv & "
          << (sh == 0 ? 0u : ((1u << sh) - 1)) << "u) != 0) ? 1 : 0;\n";
      out << "        ctx.xer = (ctx.xer & ~(1u << 29)) | (ca << 29);\n";
      if (ppc_inst.value() & 1)
        out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
      out << "    } // srawi\n";
    } else if (xo == 922) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(int32_t)(int16_t)ctx.gpr["
          << rA << "]; // extsh\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 954) { 
      out << "    ctx.gpr[" << rD << "] = (uint32_t)(int32_t)(int8_t)ctx.gpr["
          << rA << "]; // extsb\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 1019) { 
      out << "    ctx.gpr[" << rD << "] = (ctx.gpr[" << rB
          << "] != 0) ? (uint32_t)((int32_t)ctx.gpr[" << rA
          << "] / (int32_t)ctx.gpr[" << rB << "]) : 0; // divw\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 491) { 
      out << "    ctx.gpr[" << rD << "] = (ctx.gpr[" << rB
          << "] != 0) ? ctx.gpr[" << rA << "] / ctx.gpr[" << rB
          << "] : 0; // divwu\n";
      if (ppc_inst.value() & 1) {
        out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD
            << "] < 0); ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD
            << "] > 0); ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
      }
    } else if (xo == 535) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    ctx.fpr[" << frD2 << "] = ctx.ps1[" << frD2
          << "] = (double)ctx.mmu.read_f32(ctx.gpr[" << rA << "] + ctx.gpr["
          << rB << "]); // lfsx (both PS lanes)\n";
    } else if (xo == 567) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.fpr[" << frD2 << "] = ctx.ps1[" << frD2
          << "] = (double)ctx.mmu.read_f32(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lfsux (both PS lanes)\n";
    } else if (xo == 599) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    ctx.fpr[" << frD2 << "] = ctx.mmu.read_f64(ctx.gpr[" << rA
          << "] + ctx.gpr[" << rB << "]); // lfdx\n";
    } else if (xo == 631) { 
      uint32_t frD2 = ppc_inst.rd();
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.fpr[" << frD2 << "] = ctx.mmu.read_f64(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lfdux\n";
    } else if (xo == 663) { 
      out << "    ctx.mmu.write_f32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], (float)ctx.fpr[" << ppc_inst.rs() << "]); // stfsx\n";
    } else if (xo == 695) { 
      out << "    ctx.mmu.write_f32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], (float)ctx.fpr[" << ppc_inst.rs() << "]); // stfsux\n";
      out << "    ctx.gpr[" << rA << "] += ctx.gpr[" << rB << "];\n";
    } else if (xo == 727) { 
      out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], ctx.fpr[" << ppc_inst.rs() << "]); // stfdx\n";
    } else if (xo == 759) { 
      out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], ctx.fpr[" << ppc_inst.rs() << "]); // stfdux\n";
      out << "    ctx.gpr[" << rA << "] += ctx.gpr[" << rB << "];\n";
    } else if (xo == 983) { 
      out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], nwii::runtime::fpr_low_word(ctx.fpr[" << ppc_inst.rs()
          << "])); // stfiwx (raw low word, e.g. an fctiwz result)\n";
    } else if (xo == 790) { 
      out << "    { uint16_t v = ctx.mmu.read16(ctx.gpr[" << rA
          << "] + ctx.gpr[" << rB << "]); ctx.gpr[" << rD
          << "] = ((v&0xFF)<<8)|((v>>8)&0xFF); } // lhbrx\n";
    } else if (xo == 534) { 
      out << "    { uint32_t v = ctx.mmu.read32(ctx.gpr[" << rA
          << "] + ctx.gpr[" << rB << "]); ctx.gpr[" << rD
          << "] = __builtin_bswap32(v); } // lwbrx\n";
    } else if (xo == 662) { 
      out << "    ctx.mmu.write32(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], __builtin_bswap32(ctx.gpr[" << ppc_inst.rs()
          << "])); // stwbrx\n";
    } else if (xo == 1014) { 
      out << "    // dcbz (data cache block zero) - stub\n";
    } else if (xo == 55) { 
      out << "    { uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "]; ";
      out << "ctx.gpr[" << rD << "] = ctx.mmu.read32(ea); ";
      out << "ctx.gpr[" << rA << "] = ea; } // lwzux\n";
    } else if (xo == 183) { 
      out << "    {\n";
      out << "        uint32_t ea = ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "];\n";
      out << "        ctx.mmu.write32(ea, ctx.gpr[" << ppc_inst.rs() << "]);\n";
      out << "        ctx.gpr[" << rA << "] = ea;\n";
      out << "    } // stwux\n";
    } else if (xo == 918) { 
      out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + ctx.gpr[" << rB
          << "], ((ctx.gpr[" << ppc_inst.rs() << "] & 0xFF) << 8) | ((ctx.gpr["
          << ppc_inst.rs() << "] >> 8) & 0xFF)); // sthbrx\n";
    } else if (xo == 595 || xo == 210) { 
      out << "    // mfsr/mtsr stub\n";
    } else if (xo == 19) { 
      out << "    {\n";
      out << "        uint32_t cr_val = 0;\n";
      out << "        for (int i = 0; i < 8; ++i) {\n";
      out << "            cr_val |= (ctx.cr[i].lt ? 8 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].gt ? 4 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].eq ? 2 : 0) << (28 - i * 4);\n";
      out << "            cr_val |= (ctx.cr[i].so ? 1 : 0) << (28 - i * 4);\n";
      out << "        }\n";
      out << "        ctx.gpr[" << rD << "] = cr_val;\n";
      out << "    }\n";
    } else if (xo == 144) { 
      uint32_t crm = (ppc_inst.value() >> 12) & 0xFF;
      out << "    {\n";
      out << "        uint32_t cr_val = ctx.gpr[" << ppc_inst.rs() << "];\n";
      out << "        uint32_t mask = 0x" << std::hex << crm << std::dec
          << ";\n";
      out << "        for (int i = 0; i < 8; ++i) {\n";
      out << "            if (mask & (1 << (7 - i))) {\n";
      out << "                uint32_t field = (cr_val >> (28 - i * 4)) & "
             "0xF;\n";
      out << "                ctx.cr[i].lt = (field & 8) != 0;\n";
      out << "                ctx.cr[i].gt = (field & 4) != 0;\n";
      out << "                ctx.cr[i].eq = (field & 2) != 0;\n";
      out << "                ctx.cr[i].so = (field & 1) != 0;\n";
      out << "            }\n";
      out << "        }\n";
      out << "    }\n";
    } else if (xo == 566) { 
      out << "    // tlbsync (no-op)\n";
    } else {
      out << "    std::cerr << \"UNIMPLEMENTED Opcode 31 XO \" << " << xo
          << " << \" at 0x\" << std::hex << ctx.pc << std::dec << \"\\n\"; "
             "std::exit(1);\n";
    }
  } else if (ppc_inst.opcode() == 24) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] | " << ppc_inst.uimm() << "; // ori\n";
  } else if (ppc_inst.opcode() == 25) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] | " << (ppc_inst.uimm() << 16) << "; // oris\n";
  } else if (ppc_inst.opcode() == 26) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] ^ " << ppc_inst.uimm() << "; // xori\n";
  } else if (ppc_inst.opcode() == 27) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] ^ " << (ppc_inst.uimm() << 16) << "; // xoris\n";
  } else if (ppc_inst.opcode() == 28) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << ppc_inst.rs() << "] & "
        << ppc_inst.uimm() << "; // andi.\n";
    out << "    ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rA << "] < 0);\n";
    out << "    ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rA << "] > 0);\n";
    out << "    ctx.cr[0].eq = (ctx.gpr[" << rA << "] == 0);\n";
  } else if (ppc_inst.opcode() == 29) { 
    out << "    ctx.gpr[" << ppc_inst.ra() << "] = ctx.gpr[" << ppc_inst.rs()
        << "] & " << (ppc_inst.uimm() << 16) << "; // andis.\n";
  } else if (ppc_inst.opcode() == 7) { 
    out << "    ctx.gpr[" << ppc_inst.rd() << "] = (uint32_t)((int32_t)ctx.gpr["
        << ppc_inst.ra() << "] * " << ppc_inst.simm() << "); // mulli\n";
  } else if (ppc_inst.opcode() == 8) {
    // subfic: rD = SIMM - rA, CA = carry out of ~rA + SIMM + 1 (32-bit
    // unsigned). CodeWarrior pairs it with adde/subfe for ==/!= tests, so a
    // missing CA silently flips those comparisons.
    char imm[16];
    std::snprintf(imm, sizeof(imm), "0x%08Xull", (uint32_t)ppc_inst.simm());
    out << "    {\n";
    out << "        uint64_t res = (uint64_t)(~ctx.gpr[" << ppc_inst.ra()
        << "]) + " << imm << " + 1;\n";
    out << "        ctx.gpr[" << ppc_inst.rd() << "] = (uint32_t)res; // subfic\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 12) {
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    // The immediate is sign-extended to 32 bits and then added as an
    // unsigned 32-bit value: "+ -1" would widen to 2^64-1 and invert CA
    // (EXIGetID's `start != now` test failed, so memory cards never probed).
    char imm[16];
    std::snprintf(imm, sizeof(imm), "0x%08Xull", (uint32_t)ppc_inst.simm());
    out << "    {\n";
    out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA << "] + " << imm
        << ";\n";
    out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 13) {
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    char imm[16];
    std::snprintf(imm, sizeof(imm), "0x%08Xull", (uint32_t)ppc_inst.simm());
    out << "    {\n";
    out << "        uint64_t res = (uint64_t)ctx.gpr[" << rA << "] + " << imm
        << ";\n";
    out << "        ctx.gpr[" << rD << "] = (uint32_t)res;\n";
    out << "        ctx.xer = (ctx.xer & ~(1 << 29)) | (((res >> 32) & 1) << "
           "29); // CA\n";
    out << "        ctx.cr[0].lt = ((int32_t)ctx.gpr[" << rD << "] < 0);\n";
    out << "        ctx.cr[0].gt = ((int32_t)ctx.gpr[" << rD << "] > 0);\n";
    out << "        ctx.cr[0].eq = (ctx.gpr[" << rD << "] == 0);\n";
    out << "        ctx.cr[0].so = (ctx.xer >> 31) & 1;\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 15) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = " << (ppc_inst.simm() << 16) << "; // lis\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.gpr[" << rA << "] + "
          << (ppc_inst.simm() << 16) << "; // addis\n";
  } else if (ppc_inst.opcode() == 17) { 
    out << "    // sc (System Call)\n";
    out << "    nwii::runtime::handle_syscall(ctx);\n";
  } else if (ppc_inst.opcode() == 4) { 
    uint32_t xo = (inst.opcode >> 1) & 0x1F;
    uint32_t xo_10 = (inst.opcode >> 1) & 0x3FF;
    uint32_t frD = ppc_inst.rd();
    uint32_t frA = ppc_inst.ra();
    uint32_t frB = ppc_inst.rb();
    uint32_t frC = ppc_inst.rc();

    if (xo == 21) {
      out << "    ctx.ps_add(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 20) {
      out << "    ctx.ps_sub(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 25) {
      out << "    ctx.ps_mul(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 29) {
      out << "    ctx.ps_madd(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 28) {
      out << "    ctx.ps_msub(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 31) {
      out << "    ctx.ps_nmadd(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 30) {
      out << "    ctx.ps_nmsub(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 18) {
      out << "    ctx.ps_div(" << frD << ", " << frA << ", " << frB << ");\n";
    } else if (xo == 10) {
      out << "    ctx.ps_sum0(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 11) {
      out << "    ctx.ps_sum1(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 12) {
      out << "    ctx.ps_muls0(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 13) {
      out << "    ctx.ps_muls1(" << frD << ", " << frA << ", " << frC << ");\n";
    } else if (xo == 14) {
      out << "    ctx.ps_madds0(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 15) {
      out << "    ctx.ps_madds1(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo == 23) {
      out << "    ctx.ps_sel(" << frD << ", " << frA << ", " << frC << ", "
          << frB << ");\n";
    } else if (xo_10 == 72) {
      out << "    ctx.ps_mr(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 40) {
      out << "    ctx.ps_neg(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 264) {
      out << "    ctx.ps_abs(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 136) {
      out << "    ctx.ps_nabs(" << frD << ", " << frB << ");\n";
    } else if (xo_10 == 528) {
      out << "    ctx.ps_merge00(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 560) {
      out << "    ctx.ps_merge01(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 592) {
      out << "    ctx.ps_merge10(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 624) {
      out << "    ctx.ps_merge11(" << frD << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 0) {
      out << "    ctx.ps_cmpu0(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 32) {
      out << "    ctx.ps_cmpo0(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 64) {
      out << "    ctx.ps_cmpu1(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 96) {
      out << "    ctx.ps_cmpo1(" << (frD >> 2) << ", " << frA << ", " << frB
          << ");\n";
    } else if (xo_10 == 1014) {
      out << "    // dcbz_l (no-op)\n";
    } else {
      // ps_res, ps_rsqrte, psq_lx/stx/lux/stux: the interpreter has them.
      out << "    nwii::runtime::micro_interpret(ctx, 0x" << std::hex
          << inst.opcode << ", 0x" << inst.address << std::dec << ");\n";
    }
  } else if (ppc_inst.opcode() == 10) { 
    uint32_t crD = ppc_inst.rd() >> 2;
    out << "    ctx.cr[" << crD << "].lt = (ctx.gpr[" << ppc_inst.ra() << "] < "
        << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].gt = (ctx.gpr[" << ppc_inst.ra() << "] > "
        << ppc_inst.uimm() << ");\n";
    out << "    ctx.cr[" << crD << "].eq = (ctx.gpr[" << ppc_inst.ra()
        << "] == " << ppc_inst.uimm() << ");\n";
  } else if (ppc_inst.opcode() == 11) { 
    uint32_t crD = ppc_inst.rd() >> 2;
    out << "    ctx.cr[" << crD << "].lt = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] < " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].gt = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] > " << ppc_inst.simm() << ");\n";
    out << "    ctx.cr[" << crD << "].eq = ((int32_t)ctx.gpr[" << ppc_inst.ra()
        << "] == " << ppc_inst.simm() << ");\n";
  } else if (ppc_inst.opcode() == 34) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read8("
          << ppc_inst.simm() << "); // lbz\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read8(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lbz\n";
  } else if (ppc_inst.opcode() == 38) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write8(" << ppc_inst.simm() << ", (uint8_t)ctx.gpr["
          << ppc_inst.rs() << "]); // stb\n";
    else
      out << "    ctx.mmu.write8(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", (uint8_t)ctx.gpr[" << ppc_inst.rs() << "]); // stb\n";
  } else if (ppc_inst.opcode() == 40) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16("
          << ppc_inst.simm() << "); // lhz\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lhz\n";
  } else if (ppc_inst.opcode() == 41) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << ppc_inst.rd() << "] = ctx.mmu.read16(ctx.gpr["
        << rA << "]); // lhzu\n";
  } else if (ppc_inst.opcode() == 42) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16("
          << ppc_inst.simm() << "); // lha\n";
    else
      out << "    ctx.gpr[" << ppc_inst.rd()
          << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
          << "] + " << ppc_inst.simm() << "); // lha\n";
  } else if (ppc_inst.opcode() == 43) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.gpr[" << ppc_inst.rd()
        << "] = (uint32_t)(int32_t)(int16_t)ctx.mmu.read16(ctx.gpr[" << rA
        << "]); // lhau\n";
  } else if (ppc_inst.opcode() == 44) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write16(" << ppc_inst.simm() << ", (uint16_t)ctx.gpr["
          << ppc_inst.rs() << "]); // sth\n";
    else
      out << "    ctx.mmu.write16(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", (uint16_t)ctx.gpr[" << ppc_inst.rs() << "]); // sth\n";
  } else if (ppc_inst.opcode() == 45) { 
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << simm
        << ";\n";
    out << "    ctx.mmu.write16(ctx.gpr[" << rA << "], (uint16_t)ctx.gpr["
        << ppc_inst.rs() << "]); // sthu\n";
  } else if (ppc_inst.opcode() == 46) { 
    uint32_t rD = ppc_inst.rd();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t addr = "
        << (rA == 0 ? std::string("0") : "ctx.gpr[" + std::to_string(rA) + "]")
        << " + " << simm << ";\n";
    out << "        for (int i = " << rD << "; i <= 31; ++i) {\n";
    out << "            ctx.gpr[i] = ctx.mmu.read32(addr);\n";
    out << "            addr += 4;\n";
    out << "        }\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 47) { 
    uint32_t rS = ppc_inst.rs();
    uint32_t rA = ppc_inst.ra();
    int16_t simm = ppc_inst.simm();
    out << "    {\n";
    out << "        uint32_t addr = "
        << (rA == 0 ? std::string("0") : "ctx.gpr[" + std::to_string(rA) + "]")
        << " + " << simm << ";\n";
    out << "        for (int i = " << rS << "; i <= 31; ++i) {\n";
    out << "            ctx.mmu.write32(addr, ctx.gpr[i]);\n";
    out << "            addr += 4;\n";
    out << "        }\n";
    out << "    }\n";
  } else if (ppc_inst.opcode() == 50) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64("
          << ppc_inst.simm() << "); // lfd\n";
    else
      out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64(ctx.gpr["
          << rA << "] + " << ppc_inst.simm() << "); // lfd\n";
  } else if (ppc_inst.opcode() == 51) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.fpr[" << ppc_inst.rd() << "] = ctx.mmu.read_f64(ctx.gpr["
        << rA << "] + " << ppc_inst.simm() << "); // lfdu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + "
        << ppc_inst.simm() << ";\n";
  } else if (ppc_inst.opcode() == 54) { 
    uint32_t rA = ppc_inst.ra();
    if (rA == 0)
      out << "    ctx.mmu.write_f64(" << ppc_inst.simm() << ", ctx.fpr["
          << ppc_inst.rs() << "]); // stfd\n";
    else
      out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
          << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfd\n";
  } else if (ppc_inst.opcode() == 55) { 
    uint32_t rA = ppc_inst.ra();
    out << "    ctx.mmu.write_f64(ctx.gpr[" << rA << "] + " << ppc_inst.simm()
        << ", ctx.fpr[" << ppc_inst.rs() << "]); // stfdu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + "
        << ppc_inst.simm() << ";\n";
  } else if (ppc_inst.opcode() == 56) { 
    uint32_t frD = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    if (rA == 0)
      out << "    ctx.psq_load(" << frD << ", " << d << ", " << W << ", " << I
          << "); // psq_l\n";
    else
      out << "    ctx.psq_load(" << frD << ", ctx.gpr[" << rA << "] + " << d
          << ", " << W << ", " << I << "); // psq_l\n";
  } else if (ppc_inst.opcode() == 57) { 
    uint32_t frD = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    out << "    ctx.psq_load(" << frD << ", ctx.gpr[" << rA << "] + " << d
        << ", " << W << ", " << I << "); // psq_lu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << d << ";\n";
  } else if (ppc_inst.opcode() == 60) { 
    uint32_t frS = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    if (rA == 0)
      out << "    ctx.psq_store(" << frS << ", " << d << ", " << W << ", " << I
          << "); // psq_st\n";
    else
      out << "    ctx.psq_store(" << frS << ", ctx.gpr[" << rA << "] + " << d
          << ", " << W << ", " << I << "); // psq_st\n";
  } else if (ppc_inst.opcode() == 61) { 
    uint32_t frS = (inst.opcode >> 21) & 0x1F;
    uint32_t rA = (inst.opcode >> 16) & 0x1F;
    uint32_t W = (inst.opcode >> 15) & 0x1;
    uint32_t I = (inst.opcode >> 12) & 0x7;
    int16_t d = inst.opcode & 0xFFF;
    if (d & 0x800)
      d |= 0xF000;
    out << "    ctx.psq_store(" << frS << ", ctx.gpr[" << rA << "] + " << d
        << ", " << W << ", " << I << "); // psq_stu\n";
    out << "    ctx.gpr[" << rA << "] = ctx.gpr[" << rA << "] + " << d << ";\n";
  } else {
    out << "    nwii::runtime::micro_interpret(ctx, 0x" << std::hex
        << inst.opcode << ", 0x" << inst.address << std::dec << ");\n";
    out << "    if (ctx.pc != 0x" << std::hex << (inst.address + 4) << std::dec
        << ") return;\n";
  }
}

} 
} 
