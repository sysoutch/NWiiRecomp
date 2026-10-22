#pragma once
#include "analyzer/analyzer.h"
#include "recompiler/symbols.h"
#include <map>
#include <set>
#include <string>
#include <vector>

namespace nwii {
namespace recomp {

struct RecompilerConfig {
    std::string project_name = "RecompiledGame";
    std::string input_game_dir = "";
    std::string output_dir = "export";
    std::string runtime_source_dir = "../nWiiRuntime";
    std::string symbols_csv = "";

    bool split_output = false;
    int instructions_per_file = 20000;
    // Emit ++ctx.inst_count per guest instruction. That counter is the
    // timebase unless the runtime uses the wall clock ([hle] wall_timebase);
    // with the wall clock it is only statistics, and the per-instruction
    // memory increment costs real speed -- then once per function suffices.
    bool count_instructions = true;
    // [system] platform = "GameCube": Wii-only HLE signatures are ignored.
    bool gamecube = false;
    // Each function's entry switch (switch (ctx.pc) { case ...: goto ...; })
    // lists only real re-entry points -- the start, call return addresses,
    // loop back-edges (interrupt polls), jump-table targets, branch targets
    // from other functions and any address the function stores into ctx.pc
    // -- instead of every instruction. Every case is a join point the
    // optimiser cannot see through; with all of them, guest values could not
    // stay in host registers. Other addresses fall to interpret_step.
    bool lean_entry_switch = false;

    

    std::map<uint32_t, std::string> hle_hooks;
    // [hle_post_hooks]: the function runs as recompiled, then the named
    // native is called as name(ctx, args) with args = r3..r10 at entry
    // (to adjust what the function produced).
    std::map<uint32_t, std::string> hle_post_hooks;

    

    struct RelAot { std::string path; uint32_t module_base; uint32_t bss_base; };
    std::vector<RelAot> rel_aot;
};

class Recompiler {
public:
    Recompiler(const analyzer::Analyzer& analyzer, const SymbolTable* symbols = nullptr, const RecompilerConfig& config = RecompilerConfig());

    std::vector<std::string> generate_cpp(uint32_t entry_point);

    bool generate_cmake_project(uint32_t entry_point);

    std::string generate_function_cpp(const analyzer::Function& func);

private:
    void emit_function(std::ostream& out, const analyzer::Function& func, const std::string& func_name);
    void emit_instruction(std::ostream& out, const analyzer::Instruction& inst, const analyzer::Function& func);

    const analyzer::Analyzer& analyzer_;
    const SymbolTable* symbols_;
    RecompilerConfig config_;
    // Branch targets that leave their own function (lean_entry_switch).
    std::set<uint32_t> extern_targets_;
    bool extern_targets_ready_ = false;
    // Address the dispatcher maps to the function being emitted (its key in
    // the analyzer's map). Can differ from func.start_address.
    uint32_t cur_entry_ = 0;
};

} 
} 
