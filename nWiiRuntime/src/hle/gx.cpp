#include <condition_variable>

#include <map>
#include "runtime/cpu_context.h"

namespace nwii::runtime { extern CPUContext *g_ctx_ptr; }

#include "runtime/gx/fifo_parser.h"
#include "runtime/gx/renderer.h"
#include "runtime/hw/hw.h"
#include "runtime/gx_state.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <cstdlib>
#include <vector>
#include <mutex>
#include <iterator>
#include <chrono>
#include <thread>
#include <deque>
#include <algorithm>
#include <execution>
#include <cstring>

// Cached: std::getenv takes a lock and scans the environment, and these
// checks sit on the per-branch interrupt-poll path.
static const bool s_env_sample = std::getenv("NWII_SAMPLE") != nullptr;

using namespace nwii::runtime;
using namespace nwii::runtime::gx;

GXState nwii::runtime::gx::g_state = {};

static std::unique_ptr<nwii::runtime::gx::IRenderer> g_renderer = nullptr;
static void* g_window = nullptr;

void GX_SetWindow(void* window) { g_window = window; }

void GX_Init() {
    g_renderer = nwii::runtime::gx::IRenderer::Create();
    if (g_renderer) g_renderer->Initialize(nullptr);
}

namespace nwii::runtime {
    extern MMU* g_mmu;
}

std::mutex g_fifo_mutex;
std::vector<uint8_t> g_hw_fifo;

// Display lists are read out of guest RAM when the FIFO is parsed, which is
// long after the game issued the call -- and the game double-buffers those
// buffers, so by then the bytes can already belong to the next frame. The
// GXCallDisplayList seam copies them at the moment of the call; the parser
// takes the copy when one exists.
static std::mutex g_dl_snap_mtx;
// Shared, immutable buffers: the parser takes a reference instead of copying
// every list again (that was ~45 MB/s in the hub).
// Each entry carries the sequence number of its snapshot.
struct DLSnap { std::shared_ptr<const std::vector<uint8_t>> buf; uint64_t seq; };
static std::map<uint64_t, DLSnap> g_dl_snap;
static uint64_t g_dl_snap_seq = 0;

namespace nwii::runtime {
void GX_DL_Snapshot(uint32_t addr, uint32_t size) {
    if (!nwii::runtime::g_mmu || size == 0 || size > (8u << 20)) return;
    auto buf_ptr = std::make_shared<std::vector<uint8_t>>(size);
    auto &buf = *buf_ptr;
    if (const uint8_t* p = nwii::runtime::g_mmu->get_ptr(0x80000000u | (addr & 0x03FFFFFF)))
        std::memcpy(buf.data(), p, size);
    else
        for (uint32_t i = 0; i < size; ++i) buf[i] = nwii::runtime::g_mmu->read8(addr + i);
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    // Evict only old snapshots. Clearing the whole map (it did at 1024
    // entries) also dropped lists the game had just called but the parser
    // had not reached yet; those were then read from guest RAM already
    // holding the next frame's list: the atlas/menu text lost its glyph
    // positions from some point on every ~2 s (lists in a buffer that moves,
    // ~8 new addresses per frame). The parser is at most a few frames behind.
    if (g_dl_snap.size() > 1024)
        for (auto it = g_dl_snap.begin(); it != g_dl_snap.end();)
            it = it->second.seq + 512 < g_dl_snap_seq ? g_dl_snap.erase(it) : std::next(it);
    g_dl_snap[((uint64_t)(addr & 0x03FFFFFF) << 24) ^ size] = {std::move(buf_ptr), ++g_dl_snap_seq};
}

std::shared_ptr<const std::vector<uint8_t>> GX_DL_Fetch(uint32_t addr, uint32_t size) {
    std::lock_guard<std::mutex> lk(g_dl_snap_mtx);
    auto it = g_dl_snap.find(((uint64_t)(addr & 0x03FFFFFF) << 24) ^ size);
    if (it == g_dl_snap.end() || it->second.buf->size() != size) return nullptr;
    return it->second.buf;
}
} // namespace nwii::runtime


// Parsed commands, split into frames by the parser thread: s_open collects
// the current frame (parser thread only); a frame is complete at the EFB->XFB
// copy that ends a run of draws, or at a copy with no draws since the last
// one (a clear-only segment, drew = false). Complete frames wait in s_ready
// for the render thread, which takes one per present in O(1) -- re-scanning
// one flat queue under the lock every present got slower the further
// rendering fell behind, so it never caught up again.
struct ReadyFrame {
    std::vector<nwii::runtime::gx::GXCommand> cmds;
    bool drew;
};
static std::vector<nwii::runtime::gx::GXCommand> s_open;
static bool s_open_drew = false;
static std::deque<ReadyFrame> s_ready; // guarded by s_carry_mtx
// Draw-done tokens (BP 0x45 = 2) the host has parsed and decoded, i.e. whose
// guest-RAM reads (LOAD_INDX matrices, indexed vertices) are done. The
// GXDrawDone seam waits on it (see GX_WaitDrawDoneParsed).
static std::atomic<uint64_t> s_drawdone_parsed{0};
static std::mutex s_drawdone_mtx;
static std::condition_variable s_drawdone_cv;
static std::atomic<uint64_t> s_prof_drawdone_wait_us{0};

static void KickParser(); // parser thread (below)
namespace nwii::runtime { void GX_WGPIPE_Flush(); } // gather-pipe staging (below)

namespace nwii::runtime {
// Blocks the CPU thread until `target` draw-done tokens were parsed. Gives up
// after 250 ms so a stalled host (or shutdown) cannot hang the game. Returns
// the time waited in microseconds.
uint64_t GX_WaitDrawDoneParsed(uint64_t target, CPUContext &ctx) {
    if (s_drawdone_parsed.load(std::memory_order_acquire) >= target) return 0;
    nwii::runtime::GX_WGPIPE_Flush();
    KickParser();
    const auto w0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(s_drawdone_mtx);
    s_drawdone_cv.wait_for(lk, std::chrono::milliseconds(250), [&] {
        return s_drawdone_parsed.load(std::memory_order_acquire) >= target ||
               !ctx.is_running;
    });
    const uint64_t us = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - w0).count();
    s_prof_drawdone_wait_us += us;
    return us;
}
} // namespace nwii::runtime

// Render back-pressure: drawn frames the parser has queued vs. those the
// render thread has taken. When the game outran rendering, the renderer
// dropped the older frames -- skipped frames made fast camera motion look
// shaky at the 120 setting.
static std::atomic<uint64_t> s_frames_published{0}, s_frames_taken{0};
static std::mutex s_render_mtx;
static std::condition_variable s_render_cv;
static std::atomic<uint64_t> s_prof_backlog_wait_us{0};

namespace nwii::runtime {
// Blocks the CPU thread while more than `max_backlog` drawn frames wait for
// the render thread (100 ms cap).
void GX_WaitRenderBacklog(uint64_t max_backlog, CPUContext &ctx) {
    auto backlog = [] {
        return s_frames_published.load(std::memory_order_acquire) -
               s_frames_taken.load(std::memory_order_acquire);
    };
    if (backlog() <= max_backlog) return;
    const auto w0 = std::chrono::steady_clock::now();
    std::unique_lock<std::mutex> lk(s_render_mtx);
    s_render_cv.wait_for(lk, std::chrono::milliseconds(100), [&] {
        return backlog() <= max_backlog || !ctx.is_running;
    });
    s_prof_backlog_wait_us += std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - w0).count();
}
} // namespace nwii::runtime

static bool s_frame_queued = false; // another drawn frame waits in s_ready
static uint64_t s_prof_dropped = 0, s_prof_queued = 0, s_prof_empty = 0;

uint64_t g_stat_frames = 0;
uint64_t g_stat_draws = 0;


uint64_t g_stat_parse_us = 0;
uint64_t g_stat_render_us = 0;

void GX_GetClearColor(unsigned char rgba[4]) {
    rgba[0] = (unsigned char)(g_state.clearAR & 0xFF);
    rgba[1] = (unsigned char)(g_state.clearGB >> 8);
    rgba[2] = (unsigned char)(g_state.clearGB & 0xFF);
    rgba[3] = (unsigned char)(g_state.clearAR >> 8);
}

void GX_GetXfb(uint32_t* addr, unsigned* w, unsigned* h, unsigned* stride) {
    *addr   = g_state.xfbAddr;
    *w      = g_state.xfbW;
    *h      = g_state.xfbH;
    *stride = g_state.xfbStride;
}

// Early-decode time not yet reported in [GXPROF] (already in parse time).
static std::atomic<uint64_t> s_early_decode_us{0};
// Guards s_ready: the parser thread appends frames, the render thread takes
// them out.
static std::mutex s_carry_mtx;

// Drains the hardware FIFO, parses it, decodes its draws, counts draw-done
// tokens and queues complete frames in s_ready. No rendering.
static std::vector<uint8_t> s_parse_buf; // unparsed FIFO bytes (incomplete tail)

static void ParseGXFifoIntoCarry() {
    {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        if (!g_hw_fifo.empty()) {
            s_parse_buf.insert(s_parse_buf.end(), g_hw_fifo.begin(),
                               g_hw_fifo.end());
            g_hw_fifo.clear();
        }
    }
    if (s_parse_buf.empty()) return;

    auto t0 = std::chrono::steady_clock::now();
    // Reused across calls: growing a fresh vector moved every command ~15x
    // per frame.
    static std::vector<nwii::runtime::gx::GXCommand> commands;
    commands.clear();
    if (!s_parse_buf.empty())
        nwii::runtime::gx::FifoParser::Parse(s_parse_buf, commands);
    // Decode draws now, not when their frame is rendered: indexed vertex
    // and matrix arrays are read from guest RAM, and a frame can wait a
    // present or two in s_ready -- by then the game has rewritten those
    // arrays for later frames (broken bits of objects here and there).
    // DecodeDraw is a no-op for already decoded draws.
    if (!commands.empty()) {
        namespace gxn = nwii::runtime::gx;
        auto e0 = std::chrono::steady_clock::now();
        // Draws with their own normals are independent of each other and are
        // decoded in parallel (decode was half of this thread's time).
        // NWII_PAR_DECODE=0: sequential.
        [[maybe_unused]] static const bool par = [] {
            const char *e = std::getenv("NWII_PAR_DECODE");
            return !(e && e[0] == '0');
        }();
        static std::vector<gxn::GXCommand *> with_normals;
        with_normals.clear();
        for (auto &c : commands)
            if (c.type == gxn::GXCommandType::DrawPrimitive && c.raw && c.vertices.empty() &&
                c.raw->vat.nrmMask != gxn::VtxAttrMask::None)
                with_normals.push_back(&c);
        auto decode_own = [](gxn::GXCommand *c) {
            float unused[3] = {0.0f, 0.0f, 0.0f};
            gxn::FifoParser::DecodeDraw(*c, unused);
        };
#if defined(__cpp_lib_parallel_algorithm)
        if (par && with_normals.size() >= 32)
            std::for_each(std::execution::par, with_normals.begin(), with_normals.end(),
                          decode_own);
        else
#endif
            // (Sequential where the library has no parallel algorithms, e.g. Apple's libc++.)
            std::for_each(with_normals.begin(), with_normals.end(), decode_own);
        // Then in stream order: XF's latched normal carries over to draws
        // without a normal attribute (Dolphin's "cached normal").
        static float s_cached_normal[3] = {0.0f, 0.0f, 0.0f};
        for (auto &c : commands) {
            if (c.type != gxn::GXCommandType::DrawPrimitive || !c.raw) continue;
            if (c.raw->vat.nrmMask != gxn::VtxAttrMask::None) {
                if (!c.vertices.empty())
                    std::memcpy(s_cached_normal, c.vertices.back().raw_norm,
                                sizeof(s_cached_normal));
            } else {
                gxn::FifoParser::DecodeDraw(c, s_cached_normal);
            }
        }
        s_early_decode_us += std::chrono::duration_cast<std::chrono::microseconds>(
                                 std::chrono::steady_clock::now() - e0).count();
        uint64_t tokens = 0;
        for (const auto &c : commands)
            if (c.type == nwii::runtime::gx::GXCommandType::BPRegister &&
                c.reg == 0x45 && (c.val & 0xFFFF) == 2)
                ++tokens;
        if (tokens) {
            {
                std::lock_guard<std::mutex> lk(s_drawdone_mtx);
                s_drawdone_parsed.fetch_add(tokens, std::memory_order_release);
            }
            s_drawdone_cv.notify_all();
        }
    }
    auto t1 = std::chrono::steady_clock::now();
    g_stat_parse_us +=
        std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();

    static const bool s_sample = s_env_sample;
    if (s_sample && !s_parse_buf.empty()) {
        static size_t last_size = 0;
        static int stuck_ticks = 0, dumps = 0;
        if (s_parse_buf.size() == last_size) {
            if (++stuck_ticks == 60 && dumps < 8) {
                dumps++;
                stuck_ticks = 0;
                std::cout << "[GX] stalled remainder " << std::dec
                          << s_parse_buf.size() << " bytes, head:" << std::hex;
                for (size_t i = 0; i < s_parse_buf.size() && i < 24; i++)
                    std::cout << " " << (unsigned)s_parse_buf[i];
                std::cout << std::dec << "\n";
            }
        } else {
            last_size = s_parse_buf.size();
            stuck_ticks = 0;
        }
    }

    if (s_parse_buf.size() > (4u << 20))
        s_parse_buf.clear();

    

    // "sometimes only Press any button" frames. Dolphin has no such boundary

    
    
    if (commands.empty()) return;
    // Split into frames (s_open is parser-thread only; just publishing a
    // finished frame takes the lock).
    for (auto &c : commands) {
        const bool draw = c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive;
        const bool end = c.type == nwii::runtime::gx::GXCommandType::BPRegister &&
                         c.reg == 0x52 && (c.val & 0x4000);
        s_open.push_back(std::move(c));
        if (draw) {
            s_open_drew = true;
        } else if (end) {
            const size_t n = s_open.size();
            ReadyFrame f{std::move(s_open), s_open_drew};
            s_open = {};
            s_open.reserve(n);
            s_open_drew = false;
            std::lock_guard<std::mutex> carry_lk(s_carry_mtx);
            if (f.drew) s_frames_published.fetch_add(1, std::memory_order_release);
            s_ready.push_back(std::move(f));
        }
    }
}

// Parser thread: parses and decodes the FIFO as it arrives (polled every
// 100 us, and right away when GXDrawDone has to wait for it), like Dolphin's
// dual-core GPU thread. Parsing once per present left GXDrawDone waiting up
// to a vblank, and the game missed VI retraces (33 ms frames); it also moves
// the decode work (~40% of the old render thread) off the present loop.
// NWII_PARSE_THREAD=0: parse inline in ProcessGXFifo (old behaviour).
static std::mutex s_parse_kick_mtx;
static std::condition_variable s_parse_kick_cv;
static bool s_parse_kick = false;
static std::atomic<bool> s_parse_stop{false};
static std::thread s_parse_thread;

// Rendered frames are destroyed on their own thread: freeing ~40k commands
// (each owning vectors) per present was ~14% of the render thread, and on
// the parser thread it delayed parsing (GXDrawDone waits).
static std::mutex s_trash_mtx;
static std::condition_variable s_trash_cv;
static std::vector<std::vector<nwii::runtime::gx::GXCommand>> s_trash;
static std::thread s_trash_thread;

static void TrashThreadMain() {
    while (!s_parse_stop.load(std::memory_order_relaxed)) {
        std::vector<std::vector<nwii::runtime::gx::GXCommand>> t;
        {
            std::unique_lock<std::mutex> lk(s_trash_mtx);
            s_trash_cv.wait_for(lk, std::chrono::milliseconds(20), [] {
                return !s_trash.empty() || s_parse_stop.load(std::memory_order_relaxed);
            });
            t.swap(s_trash);
        }
    } // t destroyed here, outside the lock
}

namespace nwii::runtime { void native_prof_attach_self(const char *name); }

static void ParseThreadMain() {
    nwii::runtime::native_prof_attach_self("parse"); // NWII_NPROF_THREAD=parse
    while (!s_parse_stop.load(std::memory_order_relaxed)) {
        {
            std::unique_lock<std::mutex> lk(s_parse_kick_mtx);
            // NWII_PARSE_POLL_US: poll interval (default 100). GPU sync points
            // (draw-done tokens) signal only once parsed, so the poll interval
            // adds directly to the game's frame time; 1000 cost ~10 fps.
            static const auto poll = std::chrono::microseconds([] {
                const char *e = std::getenv("NWII_PARSE_POLL_US");
                return e && std::atoi(e) > 0 ? std::atoi(e) : 100;
            }());
            s_parse_kick_cv.wait_for(lk, poll, [] {
                return s_parse_kick || s_parse_stop.load(std::memory_order_relaxed);
            });
            s_parse_kick = false;
        }
        ParseGXFifoIntoCarry();
    }
}

static bool ParseThreadEnabled() {
    static const bool on = [] {
        const char *e = std::getenv("NWII_PARSE_THREAD");
        return !(e && e[0] == '0');
    }();
    return on;
}

static void KickParser() {
    {
        std::lock_guard<std::mutex> lk(s_parse_kick_mtx);
        s_parse_kick = true;
    }
    s_parse_kick_cv.notify_one();
}

void ProcessGXFifo() {
    if (ParseThreadEnabled()) {
        static const bool started = [] {
            s_parse_thread = std::thread(ParseThreadMain);
            s_trash_thread = std::thread(TrashThreadMain);
            std::atexit([] {
                s_parse_stop = true;
                KickParser();
                if (s_parse_thread.joinable()) s_parse_thread.join();
                s_trash_cv.notify_all();
                if (s_trash_thread.joinable()) s_trash_thread.join();
            });
            return true;
        }();
        (void)started;
    } else {
        ParseGXFifoIntoCarry();
    }
    // One call renders ONE frame (the oldest queued one) and leaves later
    // complete frames for the next present. Rendering every queued frame in
    // one call only ever showed the last of them: at 30-60 game fps two
    // frames were often queued, so half the frames were never seen (judder).
    // Draws of frames beyond the newest kMaxQueued are dropped (catch-up when
    // rendering falls behind); their state commands still apply.
    // Clear-only segments go with the following drawn frame.
    // NWII_GX_DROP_OLD=1: old behaviour (render only the newest frame).
    static const bool s_drop_old = std::getenv("NWII_GX_DROP_OLD") != nullptr;
    constexpr size_t kMaxQueued = 2;
    // The parts are rendered in order (Render takes partial batches); only
    // dropped frames are rebuilt, without their draws.
    static std::vector<std::vector<nwii::runtime::gx::GXCommand>> parts;
    if (ParseThreadEnabled() && !parts.empty()) {
        {
            std::lock_guard<std::mutex> lk(s_trash_mtx);
            for (auto &p : parts) s_trash.push_back(std::move(p));
        }
        s_trash_cv.notify_one();
    }
    parts.clear();
    {
        std::unique_lock<std::mutex> carry_lk(s_carry_mtx);
        if (s_ready.empty()) return;
        size_t drawn = 0;
        for (const auto &f : s_ready) drawn += f.drew;
        const size_t keep = s_drop_old ? 1 : kMaxQueued;
        size_t drop = drawn > keep ? drawn - keep : 0;
        s_prof_dropped += drop;
        if (drawn == 0) ++s_prof_empty;
        // Take frames up to and including the first drawn frame that is
        // kept (or everything, if nothing drew).
        size_t take_drawn = drawn ? drop + 1 : 0;
        while (!s_ready.empty()) {
            ReadyFrame f = std::move(s_ready.front());
            s_ready.pop_front();
            const bool dropped = f.drew && drop > 0;
            if (dropped)
                f.cmds.erase(std::remove_if(f.cmds.begin(), f.cmds.end(),
                                            [](const nwii::runtime::gx::GXCommand &c) {
                                                return c.type == nwii::runtime::gx::
                                                                     GXCommandType::DrawPrimitive;
                                            }),
                             f.cmds.end());
            parts.push_back(std::move(f.cmds));
            if (f.drew) {
                {
                    std::lock_guard<std::mutex> rlk(s_render_mtx);
                    s_frames_taken.fetch_add(1, std::memory_order_release);
                }
                s_render_cv.notify_all();
                if (drop) --drop;
                if (--take_drawn == 0) break;
            }
        }
        size_t left = 0;
        for (const auto &f : s_ready) left += f.drew;
        s_frame_queued = left > 0;
        s_prof_queued += s_frame_queued;
    }

    // NWII_FRAMESIG: per-frame signature (draw count + TEV colour/konst regs
    // + alpha test). If the WARNING "press" blink exists in our command
    // stream, some field here must alternate between frames.
    static const bool framesig = std::getenv("NWII_FRAMESIG") != nullptr;
    if (framesig && g_stat_frames < 240) {
        size_t draws = 0, verts = 0;
        for (const auto &part : parts) for (const auto &c : part)
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
                draws++;
                verts += c.raw ? c.raw->count : c.vertices.size();
            }
        // Sampled hash of guest MEM1: distinguishes "game logic frozen" from
        // "logic running but the screen is legitimately static".
        uint32_t ramsig = 0;
        if (nwii::runtime::g_mmu) {
            for (uint32_t a = 0x80003000; a < 0x81800000; a += 4096)
                ramsig = ramsig * 33 + nwii::runtime::g_mmu->read32(a);
        }
        // Hash the actual draw payloads and XF matrix payloads: counts alone
        // can't distinguish "static screen" from "animation we drop".
        uint32_t drawsig = 0, xfsig = 0;
        for (const auto &part : parts) for (const auto &c : part) {
            if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive && c.raw)
                for (size_t i = 0; i < c.raw->bytes.size(); i += 4)
                    drawsig = drawsig * 33 + c.raw->bytes[i];
            if (c.type == nwii::runtime::gx::GXCommandType::XFRegister)
                for (float f : c.payload) {
                    uint32_t u; std::memcpy(&u, &f, 4);
                    xfsig = xfsig * 33 + u;
                }
        }
        uint32_t tevsig = 0;
        for (int r = 0xE0; r <= 0xE7; r++) tevsig = tevsig * 33 + g_state.bp[r];
        std::cout << "[FRAMESIG] f=" << g_stat_frames << " draws=" << draws
                  << " verts=" << verts << " tev=" << std::hex << tevsig
                  << " draw=" << drawsig << " xf=" << xfsig
                  << " ram=" << ramsig
                  << " alpha=" << g_state.bp[0xF3] << " cmode=" << g_state.bp[0x41]
                  << std::dec << "\n";
    }

    ++g_stat_frames;
    // Count the frame's draws (already decoded right after parsing).
    auto d0 = std::chrono::steady_clock::now();
    for (auto &part : parts) for (auto &c : part) {
        if (c.type == nwii::runtime::gx::GXCommandType::DrawPrimitive) {
            nwii::runtime::gx::FifoParser::DecodeDraw(c);
            ++g_stat_draws;
        }
    }
    auto d1 = std::chrono::steady_clock::now();
    const uint64_t late_decode_us =
        std::chrono::duration_cast<std::chrono::microseconds>(d1 - d0).count();
    g_stat_parse_us += late_decode_us; // early decode is in the parse time
    const uint64_t decode_us = s_early_decode_us.exchange(0) + late_decode_us;

    // [GXPROF]: once a second, split the drain cost by cause. Prints in
    // headless too (the [STAT] line is window-only), so the 3D-scene
    // bottleneck can be measured without a display.
    {
        namespace fp = nwii::runtime::gx;
        extern uint64_t g_prof_decode_us_accum;
        g_prof_decode_us_accum += decode_us;
        static auto last = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - last).count() >= 2) {
            last = now;
            std::cout << "[GXPROF] parse+decode_ms=" << g_stat_parse_us / 1000
                      << " (decode_ms=" << g_prof_decode_us_accum / 1000 << ")"
                      << " draws_snap=" << fp::g_prof_draws
                      << " draw_bytes=" << fp::g_prof_draw_bytes
                      << " dl_calls=" << fp::g_prof_dl_calls
                      << " dl_bytes=" << fp::g_prof_dl_bytes
                      << " dl_ms=" << fp::g_prof_dl_us / 1000
                      << " snap_ms=" << fp::g_prof_snap_us / 1000
                      << " dl_loadindx=" << fp::g_prof_dl_loadindx
                      << " dl_hits=" << fp::g_prof_dl_hits
                      << " unknown=" << fp::g_prof_unknown
                      << " ready=" << [] {
                             std::lock_guard<std::mutex> lk(s_carry_mtx);
                             return s_ready.size();
                         }()
                      << " dropped=" << s_prof_dropped << " queued=" << s_prof_queued
                      << " nodraw=" << s_prof_empty
                      << " ddwait_ms=" << s_prof_drawdone_wait_us.exchange(0) / 1000
                      << " backlog_wait_ms=" << s_prof_backlog_wait_us.exchange(0) / 1000
                      << " buf=" << s_parse_buf.size() << "\n";
            g_prof_decode_us_accum = 0;
            s_prof_dropped = s_prof_queued = s_prof_empty = 0;
            fp::g_prof_draws = 0;
            fp::g_prof_draw_bytes = 0;
            fp::g_prof_dl_calls = 0;
            fp::g_prof_dl_bytes = 0;
            fp::g_prof_unknown = 0;
            fp::g_prof_dl_us = 0;
            fp::g_prof_snap_us = 0;
            fp::g_prof_dl_loadindx = 0;
            fp::g_prof_dl_hits = 0;
        }
    }

    if (g_window) {
        if (!g_renderer) {
            g_renderer = nwii::runtime::gx::IRenderer::Create();
            g_renderer->Initialize(nullptr);
        }
        if (g_renderer) {
            auto r0 = std::chrono::steady_clock::now();
            extern uint32_t g_hitch_compiles, g_hitch_texdecodes;
            g_hitch_compiles = g_hitch_texdecodes = 0;
            for (const auto &part : parts) g_renderer->Render(part);
            auto r1 = std::chrono::steady_clock::now();
            // NWII_HITCHLOG=1: render passes over 15 ms and what they did.
            static const bool hitchlog = std::getenv("NWII_HITCHLOG") != nullptr;
            const double rms = std::chrono::duration<double, std::milli>(r1 - r0).count();
            if (hitchlog && rms > 15.0)
                std::printf("[HITCH] render %.1f ms: %u shader compiles, %u texture decodes\n",
                            rms, g_hitch_compiles, g_hitch_texdecodes);
            g_stat_render_us +=
                std::chrono::duration_cast<std::chrono::microseconds>(r1 - r0)
                    .count();
        }
    }
}

namespace nwii::runtime {

// Debug taps on the write-gather pipe (any of them forces the slow path).
static const bool s_wgp_debug = std::getenv("NWII_GXDUMP") || std::getenv("NWII_WGSEG") ||
                                std::getenv("NWII_WGTRACE");
static void wgp_push_debug(uint8_t b);
static inline void wgp_push(uint8_t b) {
    if (!s_wgp_debug) [[likely]] { g_hw_fifo.push_back(b); return; }
    wgp_push_debug(b);
}
static void wgp_push_debug(uint8_t b) {

    static const char* s_dump_path = std::getenv("NWII_GXDUMP");
    static const char* s_seg_path = std::getenv("NWII_WGSEG");
    if (s_dump_path) {
        static FILE* f = fopen(s_dump_path, "wb");
        static size_t n = 0;
        if (f && n < 4000000) {
            fputc(b, f);
            fflush(f);
            ++n;
        }
    }

    
    if (const char* segp = s_seg_path) {
        static FILE* sf = fopen(segp, "w");
        static uint64_t off = 0;
        static uint32_t last_pc = 0;
        extern CPUContext* g_ctx_ptr;
        uint32_t pc = g_ctx_ptr ? g_ctx_ptr->pc : 0;
        if (sf && pc != last_pc) {
            fprintf(sf, "%llx %x\n", (unsigned long long)off, pc);
            last_pc = pc;
        }
        ++off;
    }

    
    static const bool wgtrace = std::getenv("NWII_WGTRACE") != nullptr;
    if (wgtrace) {
        static int zeros = 0;
        static int logged = 0;
        static uint8_t prev = 0xFF;
        static int vcd_collect = 0;      
        static uint32_t vcd_val = 0;
        static uint8_t vcd_reg = 0;
        extern CPUContext* g_ctx_ptr;
        if (vcd_collect > 0) {
            vcd_val = (vcd_val << 8) | b;
            if (--vcd_collect == 0)
                std::cout << "[WGP] VCD 0x" << std::hex << (unsigned)vcd_reg
                          << " = 0x" << vcd_val << " lr=0x"
                          << (g_ctx_ptr ? g_ctx_ptr->lr : 0) << std::dec << "\n";
        } else if (prev == 0x08 && (b == 0x50 || b == 0x60) && g_ctx_ptr &&
                   (g_ctx_ptr->pc == 0x800bbbac)) {
            vcd_collect = 4; vcd_val = 0; vcd_reg = b;
        }
        prev = b;
        if (b == 0) {
            ++zeros;
        } else {
            if (zeros >= 24 && b >= 0x80 && logged < 16 && g_ctx_ptr) {
                std::cout << "[WGP] draw 0x" << std::hex << (unsigned)b
                          << " after " << std::dec << zeros << " zeros pc=0x"
                          << std::hex << g_ctx_ptr->pc << " lr=0x"
                          << g_ctx_ptr->lr << std::dec << "\n";
                ++logged;
            }
            zeros = 0;
        }
    }
    g_hw_fifo.push_back(b);
}





// Gather-pipe staging: the CPU thread is the only writer, so bytes collect
// here and reach g_hw_fifo under one lock per flush. Taking g_fifo_mutex for
// every Write8/16/32 (several per vertex) was ~15% of the CPU thread.
// Flushed when full, by process_pending_callbacks (~every 20 us) and before
// GXDrawDone waits, so the parser still sees data within microseconds. The
// debug taps (NWII_GXDUMP/WGSEG/WGTRACE) keep the unstaged path.
static uint8_t s_wgp_stage[4096];
static size_t s_wgp_staged = 0;

static void wgp_flush_stage() {
    if (!s_wgp_staged) return;
    std::lock_guard<std::mutex> lock(g_fifo_mutex);
    g_hw_fifo.insert(g_hw_fifo.end(), s_wgp_stage, s_wgp_stage + s_wgp_staged);
    s_wgp_staged = 0;
}

static inline void wgp_stage(uint8_t b) {
    if (s_wgp_debug) [[unlikely]] {
        std::lock_guard<std::mutex> lock(g_fifo_mutex);
        wgp_push_debug(b);
        return;
    }
    s_wgp_stage[s_wgp_staged++] = b;
    if (s_wgp_staged == sizeof(s_wgp_stage)) wgp_flush_stage();
}

// A whole big-endian word in one store (most gather-pipe traffic is 32-bit).
static inline void wgp_stage32(uint32_t v) {
    if (!s_wgp_debug && s_wgp_staged + 4 <= sizeof(s_wgp_stage)) [[likely]] {
        const uint32_t be = nwii::runtime::nwii_bswap32(v);
        std::memcpy(s_wgp_stage + s_wgp_staged, &be, 4);
        s_wgp_staged += 4;
        if (s_wgp_staged == sizeof(s_wgp_stage)) wgp_flush_stage();
        return;
    }
    wgp_stage(v >> 24);
    wgp_stage((v >> 16) & 0xFF);
    wgp_stage((v >> 8) & 0xFF);
    wgp_stage(v & 0xFF);
}

static bool wgp_redirect(uint8_t b) {
    uint32_t base, end, wptr;
    nwii::runtime::hw::pi_fifo_get(base, end, wptr);
    if (base == 0) return false; 
    uint32_t cp_base = nwii::runtime::hw::cp_fifo_base_reg();
    if ((base & 0x03FFFFFF) == (cp_base & 0x03FFFFFF))
        return false; 
    if (!nwii::runtime::g_mmu) return false;
    nwii::runtime::g_mmu->write8(0x80000000u | (wptr & 0x01FFFFFFu), b);
    wptr += 1;
    if ((wptr & 0x03FFFFFF) > (end & 0x03FFFFFF))
        wptr = base;
    nwii::runtime::hw::pi_fifo_set_wptr(wptr);
    return true;
}

void GX_WGPIPE_Flush() { wgp_flush_stage(); }

void GX_WGPIPE_Write8(uint8_t val) {
    if (wgp_redirect(val)) return;
    wgp_stage(val);
}

void GX_WGPIPE_Write16(uint16_t val) {
    if (wgp_redirect(val >> 8)) { wgp_redirect(val & 0xFF); return; }
    wgp_stage(val >> 8);
    wgp_stage(val & 0xFF);
}

void GX_WGPIPE_Write32(uint32_t val) {
    if (wgp_redirect(val >> 24)) {
        wgp_redirect((val >> 16) & 0xFF);
        wgp_redirect((val >> 8) & 0xFF);
        wgp_redirect(val & 0xFF);
        return;
    }
    if (s_env_sample) {
        static uint64_t n = 0;
        ++n;
        if (n == 1 || n == 10 || n == 100 || n == 1000 || n == 10000 ||
            (n % 100000) == 0)
            std::cout << "[GX] WGP write32 #" << std::dec << n
                      << " fifo_bytes=" << g_hw_fifo.size() << "\n";
    }
    wgp_stage32(val);
}

void GX_WGPIPE_WriteF32(float val) {
    union { float f; uint32_t i; } u; u.f = val;
    GX_WGPIPE_Write32(u.i);
}

void GX_WGPIPE_WriteF64(double val) {
    union { double d; uint64_t i; } u; u.d = val;
    if (wgp_redirect((u.i >> 56) & 0xFF)) {
        for (int s = 48; s >= 0; s -= 8)
            wgp_redirect((u.i >> s) & 0xFF);
        return;
    }
    for (int s = 56; s >= 0; s -= 8)
        wgp_stage((u.i >> s) & 0xFF);
}

} 
uint64_t g_prof_decode_us_accum = 0;
