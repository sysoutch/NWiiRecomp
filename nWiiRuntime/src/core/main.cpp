#include "input/input_manager.h"
#include "input/sources/gamepad_source.h"
#include "input/sources/mouse_keyboard_source.h"
#include "input/sources/phone_source.h"
#include "loader/loader.h"
#include "runtime/config.h"
#include "runtime/efb.h"
#include "runtime/launcher.h"
#include "runtime/settings_overlay.h"
#include "runtime/cpu_context.h"
#include "runtime/virtual_disc.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <SDL.h>
#include <glad/glad.h>
#include <thread>
#include <vector>

#include "runtime/gx_state.h"
#include "runtime/hw/hw.h"

// Cached: std::getenv takes a lock and scans the environment, and these
// checks sit on the per-branch interrupt-poll path.
static const bool s_env_sample = std::getenv("NWII_SAMPLE") != nullptr;

#ifdef _WIN32
// timeBeginPeriod without pulling in all of windows.h.
extern "C" __declspec(dllimport) unsigned int __stdcall timeBeginPeriod(unsigned int);
#pragma comment(lib, "winmm.lib")
#endif

namespace nwii::runtime {
    void native_prof_start(std::thread &cpu_thread); // native_prof.cpp
    namespace hw { extern uint32_t g_vi_top_field_base; }
}

namespace nwii::runtime {
uint32_t g_debug_pc = 0;
MMU *g_mmu = nullptr;
CPUContext *g_ctx_ptr = nullptr;

bool init(const char *config_path = "config.toml") {
  Config::get().load(config_path);
  hw::register_all_hw(MMIODispatcher::get());
  return true;
}
void shutdown() {}
} 

extern "C" void run_game(nwii::runtime::CPUContext &ctx);
extern uint64_t g_stat_frames; // gx.cpp

namespace nwii::runtime::efb {
int scale = 1, samples = 1, width = 640, height = 480;
bool widescreen = false;
float sx = 1.0f, sy = 1.0f;
unsigned draw_fbo = 0, resolve_fbo = 0, display_fbo = 0;
int xfb_x = 0, xfb_y = 0, xfb_w = 640, xfb_h = 480;

// XFB model: games double-buffer the external framebuffer and the VI scans
// out only the one VISetNextFrameBuffer named. One snapshot of the EFB per
// XFB address (taken at the EFB->XFB copy); the window shows the one the VI
// points at. Showing simply the latest copy flashed frames the game never
// displayed (the fade-in after loading flickered bright/dark).
struct XfbSlot { uint32_t dest = 0; unsigned fbo = 0, tex = 0; uint64_t stamp = 0; };
static XfbSlot s_xfb[3];
static uint64_t s_xfb_clock = 0;
static void delete_xfb_slots() {
  for (auto &x : s_xfb) {
    if (x.fbo) glDeleteFramebuffers(1, &x.fbo);
    if (x.tex) glDeleteTextures(1, &x.tex);
    x = XfbSlot{};
  }
  display_fbo = 0;
}
static void create_xfb_slots() {
  for (auto &x : s_xfb) {
    glGenFramebuffers(1, &x.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, x.fbo);
    glGenTextures(1, &x.tex);
    glBindTexture(GL_TEXTURE_2D, x.tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, x.tex, 0);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT);
  }
  display_fbo = s_xfb[0].fbo;
}

// Copy the finished frame to the snapshot for XFB address `dest`.
void snapshot(uint32_t dest) {
  if (!s_xfb[0].fbo) return;
  dest &= 0x01FFFFFF;
  XfbSlot *slot = nullptr;
  for (auto &x : s_xfb) if (x.stamp && x.dest == dest) slot = &x;
  if (!slot) {
    slot = &s_xfb[0];
    for (auto &x : s_xfb) if (x.stamp < slot->stamp) slot = &x;
  }
  slot->dest = dest;
  slot->stamp = ++s_xfb_clock;
  resolve();
  glBindFramebuffer(GL_READ_FRAMEBUFFER, read_fbo());
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, slot->fbo);
  glBlitFramebuffer(0, 0, width, height, 0, 0, width, height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
  static const bool xlog = std::getenv("NWII_XFBLOG") != nullptr;
  if (xlog) {
    // Mean of a 8x8 grid of pixels: is this copy a bright or a dark frame?
    glBindFramebuffer(GL_READ_FRAMEBUFFER, slot->fbo);
    unsigned sum = 0;
    for (int gy = 0; gy < 8; ++gy)
      for (int gx = 0; gx < 8; ++gx) {
        unsigned char px[4];
        glReadPixels(width * (2 * gx + 1) / 16, height * (2 * gy + 1) / 16, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, px);
        sum += (px[0] + px[1] + px[2]) / 3;
      }
    std::printf("[XFBCOPY] frame=%llu dest=%08X mean=%u\n", (unsigned long long)::g_stat_frames, dest, sum / 64);
  }
  glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
}

// Pick what the window shows: the snapshot of the XFB the VI scans out
// (TFBL register, POFF bit 28 = address in 32-byte units), else the newest.
void select_display(uint32_t vi_tfbl) {
  const uint32_t a = ((vi_tfbl & 0x10000000u) ? (vi_tfbl & 0x00FFFFFFu) << 5
                                               : (vi_tfbl & 0x00FFFFFFu)) & 0x01FFFFFF;
  XfbSlot *best = nullptr, *newest = nullptr;
  for (auto &x : s_xfb) {
    if (!x.stamp) continue;
    if (!newest || x.stamp > newest->stamp) newest = &x;
    if (a && x.dest == a) best = &x;
  }
  // The VI framebuffer register is written as two 16-bit halves and only
  // the upper one reaches us here (e.g. 0x38 for XFB 0x384E80): match the
  // address' high half when the full address didn't match.
  if (!best && vi_tfbl && vi_tfbl < 0x200u)
    for (auto &x : s_xfb)
      if (x.stamp && (x.dest >> 16) == vi_tfbl) best = &x;
  // Present the newest complete frame. The VI pointer is set by the CPU
  // before the (lagging) renderer has executed that frame's copy, so
  // following it showed older frames out of order -- judder once the game
  // ran above 30 fps. Clear-only copies never reach a slot (see the XFB copy
  // path), so the newest slot is always a finished frame.
  // NWII_XFB_VI=1: follow the VI pointer as before.
  static const bool follow_vi = std::getenv("NWII_XFB_VI") != nullptr;
  if (!follow_vi && newest) display_fbo = newest->fbo;
  else if (best) display_fbo = best->fbo;
  else if (newest) display_fbo = newest->fbo;
  // NWII_XFBLOG=1: which XFB the VI points at and which snapshot is shown.
  static const bool xlog = std::getenv("NWII_XFBLOG") != nullptr;
  if (xlog) {
    static uint32_t last_a = 0; static unsigned last_fbo = 0;
    if (a != last_a || display_fbo != last_fbo) {
      std::printf("[XFB] vi=%08X -> %s slot fbo=%u (slots:", a, best ? "match" : "NEWEST", display_fbo);
      for (auto &x : s_xfb) std::printf(" %08X@%llu", x.dest, (unsigned long long)x.stamp);
      std::printf(")\n");
      last_a = a; last_fbo = display_fbo;
    }
  }
}

void resolve() {
  if (samples <= 1 || draw_fbo == resolve_fbo)
    return;
  glBindFramebuffer(GL_READ_FRAMEBUFFER, draw_fbo);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, resolve_fbo);
  glBlitFramebuffer(0, 0, width, height, 0, 0, width, height,
                    GL_COLOR_BUFFER_BIT, GL_NEAREST);
  glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
}
// (Re)create the EFB render targets from [graphics] internal_scale / msaa /
// widescreen: at startup and whenever the settings overlay changes them (the
// renderer reads these globals per draw, so a rebuild between frames is safe;
// the game redraws the next frame).
void create() {
  static GLuint fbo = 0, tex = 0, depth = 0, ms_fbo = 0, ms_color = 0, ms_depth = 0;
  delete_xfb_slots();
  if (fbo) {
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &tex);
    glDeleteRenderbuffers(1, &depth);
    fbo = tex = depth = 0;
  }
  if (ms_fbo) {
    glDeleteFramebuffers(1, &ms_fbo);
    glDeleteRenderbuffers(1, &ms_color);
    glDeleteRenderbuffers(1, &ms_depth);
    ms_fbo = ms_color = ms_depth = 0;
  }
  const auto &cfg = nwii::runtime::Config::get();
  scale = cfg.internal_scale;
  widescreen = cfg.widescreen;
  // Widescreen widens the target by 4/3 (4:3 -> 16:9); the renderer widens
  // the 3D field of view to match instead of stretching.
  width = widescreen ? (640 * scale * 4 + 2) / 3 : 640 * scale;
  height = 480 * scale;
  sx = width / 640.0f;
  sy = height / 480.0f;
  GLint max_samples = 1;
  glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
  samples = std::max(1, std::min(cfg.msaa, (int)max_samples));

  // With MSAA, GX draws into a multisampled framebuffer and resolve() blits
  // it into the single-sample texture framebuffer that copies, readbacks and
  // present use.
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
  glGenRenderbuffers(1, &depth);
  glBindRenderbuffer(GL_RENDERBUFFER, depth);
  glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, width, height);
  glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, depth);
  resolve_fbo = draw_fbo = fbo;
  if (samples > 1) {
    glGenFramebuffers(1, &ms_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ms_fbo);
    glGenRenderbuffers(1, &ms_color);
    glBindRenderbuffer(GL_RENDERBUFFER, ms_color);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_RGBA8, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ms_color);
    glGenRenderbuffers(1, &ms_depth);
    glBindRenderbuffer(GL_RENDERBUFFER, ms_depth);
    glRenderbufferStorageMultisample(GL_RENDERBUFFER, samples, GL_DEPTH_COMPONENT24, width, height);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, ms_depth);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE) {
      draw_fbo = ms_fbo;
    } else {
      std::cout << "[GL] MSAA framebuffer incomplete, MSAA off" << std::endl;
      samples = 1;
    }
  }
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  const GLenum st = glCheckFramebufferStatus(GL_FRAMEBUFFER);
  std::cout << "[GL] EFB " << width << "x" << height << " (" << scale << "x), MSAA "
            << samples << "x, FBO " << (st == GL_FRAMEBUFFER_COMPLETE ? "complete" : "INCOMPLETE!")
            << std::endl;
  // XFB snapshots (what the window shows), same size as the EFB.
  create_xfb_slots();
  // Start black rather than with stale/undefined contents.
  glBindFramebuffer(GL_FRAMEBUFFER, draw_fbo);
  glClearColor(0, 0, 0, 1);
  glDepthMask(GL_TRUE); // GX Z mode may have left depth writes off
  glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
  glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
} // namespace nwii::runtime::efb

void cpu_thread_func(nwii::runtime::CPUContext *ctx) {
  nwii::runtime::g_ctx_ptr = ctx;
  std::cout << "[Thread] CPU Core started. pc=" << std::hex << ctx->pc << " is_running=" << ctx->is_running << std::endl;
  std::flush(std::cout);
  run_game(*ctx);

  
  
  if (!ctx->is_running) {
    std::cout << "[Thread] CPU Core exited: shutdown requested (window closed "
                 "or quit). NOT a crash." << std::endl;
  } else {
    std::cout << "[Thread] CPU Core CRASHED (jumped to 0). Final PC: 0x"
              << std::hex << ctx->pc << ", LR: 0x" << ctx->lr << std::dec
              << std::endl;
  }
}

int main(int argc, char **argv) {
  // [disc] [config]: the disc (extracted folder or .iso/.gcm) may be left out
  // -- the launcher then asks for one (a lone *.toml argument is the config).
  std::string game_path = argc >= 2 ? argv[1] : "";
  std::string config_path = (argc >= 3) ? argv[2] : "config.toml";
  if (argc == 2 && std::filesystem::path(game_path).extension() == ".toml") {
    config_path = argv[1];
    game_path.clear();
  }
  // Started without a config (double-clicked, or a disc dropped on the exe):
  // look next to the exe and up to 4 folders above it for the game's config
  // (a *.toml with a [game] section), creating it from <name>.default.toml
  // when only that exists.
  if (argc < 3 && config_path == "config.toml" && !std::filesystem::exists(config_path)) {
    namespace fs = std::filesystem;
    std::error_code ec;
    fs::path dir = fs::absolute(fs::path(argv[0]), ec).parent_path();
    auto is_game_config = [](const fs::path &p) {
      std::ifstream f(p);
      std::string line;
      while (std::getline(f, line))
        if (line.rfind("[game]", 0) == 0) return true;
      return false;
    };
    for (int up = 0; up <= 4 && !dir.empty(); ++up, dir = dir.parent_path()) {
      fs::path found, def;
      for (const auto &e : fs::directory_iterator(dir, ec)) {
        const fs::path p = e.path();
        if (p.extension() != ".toml" || !is_game_config(p)) continue;
        if (p.stem().extension() == ".default") def = p;
        else found = p;
      }
      if (found.empty() && !def.empty()) {
        found = def.parent_path() / (def.stem().stem().string() + ".toml");
        fs::copy_file(def, found, ec);
      }
      if (!found.empty() && fs::exists(found)) {
        config_path = found.string();
        std::cout << "[Config] using " << config_path << std::endl;
        break;
      }
      if (dir == dir.parent_path()) break;
    }
  }

#ifdef _WIN32
  // The idle loop sleeps 1 ms waiting for the next interrupt; at Windows'
  // default 15.6 ms timer resolution every wake-up came ~15 ms late and a
  // game whose threads sleep on VI/DSP/DVD between frames ran at ~3 fps.
  timeBeginPeriod(1);
#endif

  if (!nwii::runtime::init(config_path.c_str()))
    return 1;

  {
    using namespace nwii::runtime::input;
    auto& im = InputManager::get();
    int mode = nwii::runtime::Config::get().input_mode;
    switch (mode) {
    case 1: 
      std::cout << "[Input] Mode 1 (Bluetooth Wiimote) not implemented, "
                   "falling back to keyboard+mouse" << std::endl;
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 2: 
    case 3: 
    case 4: 
      im.add_source(std::make_unique<GamepadSource>());
      
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 5: 
      im.add_source(std::make_unique<PhoneSource>());
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 7: 
      std::cout << "[Input] Mode 7 (touch): using pointer-as-touch mapping"
                << std::endl;
      im.add_source(std::make_unique<MouseKeyboardSource>());
      break;
    case 6:
    default:
      im.add_source(std::make_unique<MouseKeyboardSource>());
      im.add_source(std::make_unique<GamepadSource>());
      break;
    }
  }

  const char *headless_env = std::getenv("NWII_HEADLESS");
  bool headless = headless_env && headless_env[0] == '1';

  SDL_Window* window = nullptr;
  SDL_GLContext gl_ctx = nullptr;
  SDL_AudioDeviceID audio_dev = 0;
  GLuint xfb_tex = 0;

  if (!headless) {
    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO | SDL_INIT_GAMECONTROLLER) < 0) {
      std::cerr << "SDL_Init failed: " << SDL_GetError() << "\n";
      return 1;
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);

    
    SDL_GL_SetAttribute(SDL_GL_RED_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_ALPHA_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
    SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
    {
      SDL_DisplayMode dm;
      if (SDL_GetCurrentDisplayMode(0, &dm) == 0)
        std::cout << "[GL] display 0: " << dm.w << "x" << dm.h << " fmt="
                  << SDL_GetPixelFormatName(dm.format) << std::endl;
      else
        std::cerr << "[GL] no display mode: " << SDL_GetError()
                  << " (screen locked or no display attached?)" << std::endl;
    }
#ifdef __APPLE__

    
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS,
                        SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
#endif
    bool use_gl = (nwii::runtime::Config::get().backend == nwii::runtime::Backend::OpenGL);
    uint32_t window_flags = SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI;
    if (use_gl) {
        window_flags |= SDL_WINDOW_OPENGL;
    } else {
#ifdef SDL_WINDOW_METAL
        window_flags |= SDL_WINDOW_METAL;
#endif
    }
    // NWII_HIDDEN=1: no visible window (automated test runs). Everything
    // renders into the EFB/XFB framebuffers, so screenshots still work.
    if (std::getenv("NWII_HIDDEN")) window_flags |= SDL_WINDOW_HIDDEN;
    window = SDL_CreateWindow("NWiiRecomp", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED,
                              nwii::runtime::Config::get().window_width,
                              nwii::runtime::Config::get().window_height,
                              window_flags);
    if (!window) {
      std::cerr << "SDL_CreateWindow failed: " << SDL_GetError() << "\n";
      return 1;
    }

    
    extern void GX_SetWindow(void *);
    GX_SetWindow(window);
    if (use_gl) {
      gl_ctx = SDL_GL_CreateContext(window);
      if (!gl_ctx) {
        std::cerr << "SDL_GL_CreateContext failed: " << SDL_GetError() << "\n";
        return 1;
      }
      if (!gladLoadGLLoader((GLADloadproc)SDL_GL_GetProcAddress)) {
        std::cerr << "glad: failed to load GL functions\n";
        return 1;
      }
      std::cout << "[GL] " << (const char *)glGetString(GL_RENDERER) << " | "
                << (const char *)glGetString(GL_VERSION) << std::endl;
      SDL_GL_SetSwapInterval(nwii::runtime::Config::get().enable_vsync ? 1 : 0);
    }
    if (nwii::runtime::Config::get().fullscreen)
      SDL_SetWindowFullscreen(window, SDL_WINDOW_FULLSCREEN_DESKTOP);

    nwii::runtime::efb::create();
    if (use_gl) {
      GLint max_samples = 1;
      GLfloat max_aniso = 1.0f;
      glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
      glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &max_aniso);
      glGetError(); // the anisotropy query fails harmlessly without the extension
      nwii::runtime::ui::init(window, gl_ctx, max_samples, (int)max_aniso);
    }

    glGenTextures(1, &xfb_tex);
    glBindTexture(GL_TEXTURE_2D, xfb_tex);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

    // Launcher (title menu) before the game boots: always unless disabled
    // ([launcher] enabled = false) or a scripted test run; also whenever no
    // usable disc was given.
    {
      auto &cfg = nwii::runtime::Config::get();
      if (game_path.empty() && !cfg.recent_discs.empty())
        game_path = cfg.recent_discs.front();
      const bool scripted = std::getenv("NWII_SKIP_LAUNCHER") || std::getenv("NWII_CTRL") ||
                            std::getenv("NWII_INPUT_SCRIPT") || std::getenv("NWII_HIDDEN") ||
                            std::getenv("NWII_UI_OPEN");
      const auto info = nwii::runtime::launcher::probe(game_path, cfg.game_id);
      const bool usable = (info.kind == info.Image || info.kind == info.Extracted) && info.error.empty();
      if (use_gl && ((cfg.launcher && !scripted) || !usable)) {
        game_path = nwii::runtime::launcher::run(window, game_path);
        if (game_path.empty()) { // quit from the launcher
          nwii::runtime::ui::shutdown();
          SDL_GL_DeleteContext(gl_ctx);
          SDL_DestroyWindow(window);
          SDL_Quit();
          return 0;
        }
      }
    }

    SDL_AudioSpec want = {0}, have;
    want.freq = 32000;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 1024;
    audio_dev = SDL_OpenAudioDevice(NULL, 0, &want, &have, 0);
    if (audio_dev > 0) SDL_PauseAudioDevice(audio_dev, 0);
  }

  // Audio feeder thread: keeps a cushion (80-250 ms) queued on the host device, taking
  // only what the game's audio DMA has produced. It used to run in the
  // present loop and always queued 1024 frames, padding with zeros whenever
  // the game was momentarily behind -- inserted silence that crackled, mostly
  // around camera cuts (dialogue start/end) where rendering or the game thread
  // stall for a moment.
  // The game produces exactly real time, so the queue only holds whatever
  // cushion it was given: feeding each block as it arrived kept it near
  // 0 ms, and every 32 ms device pull came up short and was padded with
  // silence (voice stutter, worse at high frame rates). So playback starts,
  // and restarts after the queue runs dry, only once prime_frames are
  // waiting.
  std::atomic<bool> audio_stop{false};
  std::thread audio_thread;
  if (audio_dev > 0) {
    audio_thread = std::thread([&audio_stop, audio_dev] {
      // 32 kHz stereo s16: 32 frames per ms. Queue target = cushion + 40 ms.
      constexpr size_t kHeadroomFrames = 1280;  // 40 ms
      constexpr size_t kMaxPrimeFrames = 8000;  // 250 ms
      size_t prime_frames = 2560;               // 80 ms, grows on each dry run
      bool priming = true;
      static int16_t abuf[1024 * 2], tbuf[1032 * 2];
      double trim_acc = 0.0;
      // NWII_AUDIOSTAT=1: once a second, the frames the game produced, the
      // lowest host queue level and how often the queue ran dry.
      const bool stat = std::getenv("NWII_AUDIOSTAT") != nullptr;
      auto stat_t0 = std::chrono::steady_clock::now();
      size_t stat_frames = 0, stat_dry = 0;
      Uint32 stat_min = ~0u;
      bool was_dry = true;
      while (!audio_stop.load(std::memory_order_relaxed)) {
        const Uint32 queued = SDL_GetQueuedAudioSize(audio_dev);
        if (stat) {
          stat_min = std::min(stat_min, queued);
          if (queued == 0 && !was_dry) ++stat_dry;
          was_dry = queued == 0;
          const auto t = std::chrono::steady_clock::now();
          if (t - stat_t0 >= std::chrono::seconds(1)) {
            std::cout << "[AudioStat] produced=" << stat_frames << " min_queued_ms="
                      << stat_min / 128 << " dry=" << stat_dry << std::endl;
            stat_t0 = t;
            stat_frames = stat_dry = 0;
            stat_min = ~0u;
          }
        }
        // Ran dry: the game stalled for longer than the cushion (level loads,
        // the first seconds of a level). Each time, wait for 1.5x more before
        // resuming (up to kMaxPrimeFrames), so a stalling stretch gives one
        // longer gap instead of crackling on every stall.
        if (queued == 0 && !priming) {
          priming = true;
          prime_frames = std::min(prime_frames * 3 / 2, kMaxPrimeFrames);
        }
        if (priming && nwii::runtime::hw::dsp_audio_available() >= prime_frames)
          priming = false;
        const Uint32 target_bytes = (Uint32)(prime_frames + kHeadroomFrames) * 4;
        if (!priming && queued < target_bytes) {
          const size_t want_frames =
              std::min<size_t>(1024, (target_bytes - queued) / 4);
          const size_t got = nwii::runtime::hw::dsp_audio_take(abuf, want_frames);
          stat_frames += got;
          if (got) {
            // Clock drift: the game's 32 kHz (wall clock) and the device's
            // differ slightly, which slowly drained the cushion (~35 ms in
            // 80 s). Below 3/4 of it, stretch by 0.2%; with more than 80 ms
            // still waiting after this take (the game ahead), shrink: an
            // extra or dropped frame every ~500, spread over the chunk by
            // linear interpolation so first and last frames stay.
            const int16_t *out = abuf;
            size_t n = got;
            const int trim = queued < prime_frames * 3 ? 1
                           : nwii::runtime::hw::dsp_audio_available() > 2 * kHeadroomFrames ? -1 : 0;
            trim_acc += trim * got / 500.0;
            const long extra = std::lround(trim_acc);
            if (extra != 0 && got > 2) {
              trim_acc -= extra;
              n = got + extra;
              for (size_t i = 0; i < n; ++i) {
                const double pos = (double)i * (got - 1) / (n - 1);
                const size_t k = std::min((size_t)pos, got - 2);
                const double f = pos - k;
                for (int ch = 0; ch < 2; ++ch)
                  tbuf[i * 2 + ch] = (int16_t)std::lround(abuf[k * 2 + ch] * (1 - f) +
                                                          abuf[(k + 1) * 2 + ch] * f);
              }
              out = tbuf;
            }
            // [audio] volume / mute from the settings overlay.
            const auto &acfg = nwii::runtime::Config::get();
            const int vol = acfg.mute ? 0 : acfg.volume;
            if (vol < 100) {
              if (out == abuf) std::copy(abuf, abuf + n * 2, tbuf);
              for (size_t i = 0; i < n * 2; ++i) tbuf[i] = (int16_t)(tbuf[i] * vol / 100);
              out = tbuf;
            }
            SDL_QueueAudio(audio_dev, out, (Uint32)(n * 4));
            continue;
          }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
      }
    });
  }

  if (game_path.empty()) {
    std::cerr << "Usage: " << argv[0] << " <extracted game dir or disc.iso> [config_path]\n";
    return 1;
  }
  nwii::runtime::Config::get().game_dir = game_path;

  auto ctx = std::make_unique<nwii::runtime::CPUContext>();
  nwii::runtime::g_mmu = &ctx->mmu;

  nwii::loader::Executable exe;
  if (!exe.load_unpacked_game(game_path))
    return 1;

  uint32_t arena_lo = 0x80000000;
  
  for (const auto &sec : exe.sections) {
    uint32_t end_addr = sec.address + sec.size;
    if (end_addr > arena_lo)
      arena_lo = end_addr;

    if (sec.is_bss) {
      for (size_t i = 0; i < sec.size; ++i)
        ctx->mmu.write8(sec.address + i, 0);
    }
  }

  for (const auto &sec : exe.sections) {
    if (!sec.is_bss) {
      std::cout << "[Loader] Loading section at 0x" << std::hex << sec.address << " size 0x" << sec.size << std::dec << std::endl;
      for (size_t i = 0; i < sec.size; ++i) {
        if ((sec.address + i) == 0x80003448) {
            std::cout << "[Loader] Found 0x80003448 inside section! val=" << std::hex << (uint32_t)sec.data[i] << std::dec << std::endl;
        }
        ctx->mmu.write8(sec.address + i, sec.data[i]);
      }
    }

    

    
    
    if (sec.is_text) {
      uint32_t start = sec.address;
      if (std::getenv("NWII_INTERP_LOW"))
        start = std::max<uint32_t>(sec.address, 0x80004000);
      uint32_t end = sec.address + sec.size;
      if (end > start)
        nwii::runtime::add_recompiled_range(start, end);
    }
  }

  arena_lo = (arena_lo + 31) & ~31;

  

  {
    uint32_t stack_top = 0;
    uint32_t hi = 0;
    bool have_hi = false;
    for (uint32_t off = 0; off < 0x200; off += 4) {
      uint32_t insn = ctx->mmu.read32(exe.entry_point + off);
      if ((insn & 0xFFFF0000) == 0x3C200000) { 
        hi = (insn & 0xFFFF) << 16;
        have_hi = true;
      } else if (have_hi && (insn & 0xFFFF0000) == 0x60210000) { 
        stack_top = hi | (insn & 0xFFFF);
        break;
      } else if (have_hi && (insn & 0xFFFF0000) == 0x38210000) { 
        stack_top = hi + (int16_t)(insn & 0xFFFF);
        break;
      }
    }
    if (stack_top >= arena_lo && stack_top < 0x81800000) {
      std::cout << "[Loader] crt stack top 0x" << std::hex << stack_top
                << " above section end 0x" << arena_lo
                << ", raising ArenaLo" << std::dec << std::endl;

      arena_lo = (stack_top + 0x100 + 31) & ~31u;
    }
  }

  bool is_gc = nwii::runtime::Config::get().platform ==
               nwii::runtime::Platform::GameCube;

  
  uint32_t arena_hi = 0x81700000;
  auto &vdisc = nwii::runtime::VirtualDisc::get();
  if (vdisc.init(game_path)) {
    const auto &boot = vdisc.boot_data();
    
    for (size_t i = 0; i < 0x20 && i < boot.size(); ++i)
      ctx->mmu.write8(0x80000000 + (uint32_t)i, boot[i]);

    const auto &fst = vdisc.fst_data();
    uint32_t fst_size = (uint32_t)fst.size();
    uint32_t fst_addr = (0x81800000 - fst_size) & ~63u;
    for (uint32_t i = 0; i < fst_size; ++i)
      ctx->mmu.write8(fst_addr + i, fst[i]);

    ctx->mmu.write32(0x80000038, fst_addr); 
    ctx->mmu.write32(0x8000003C, fst_size); 
    if (fst_addr < 0x81700000)
      arena_hi = fst_addr & ~31u;
    std::cout << "[Loader] FST loaded at 0x" << std::hex << fst_addr
              << " size 0x" << fst_size << std::dec << std::endl;
  }

  // NWII_DISC_VERIFY=<extracted dir>: compare every file read through the
  // disc layer (e.g. an RVZ) with the extracted copy, report, and exit.
  if (const char *ref = std::getenv("NWII_DISC_VERIFY"); ref && vdisc.valid()) {
    const auto &fst = vdisc.fst_data();
    auto be = [&](size_t o) { return (uint32_t)fst[o] << 24 | (uint32_t)fst[o + 1] << 16 | (uint32_t)fst[o + 2] << 8 | fst[o + 3]; };
    const uint32_t n = be(8);
    const char *names = reinterpret_cast<const char *>(fst.data() + n * 12);
    std::vector<std::pair<uint32_t, std::filesystem::path>> dirs{{n, std::filesystem::path(ref) / "files"}};
    int files = 0, bad = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (uint32_t i = 1; i < n; ++i) {
      while (dirs.size() > 1 && i >= dirs.back().first) dirs.pop_back();
      const std::string name = names + (be(i * 12) & 0xFFFFFF);
      if (fst[i * 12]) { dirs.push_back({be(i * 12 + 8), dirs.back().second / name}); continue; }
      std::ifstream f(dirs.back().second / name, std::ios::binary);
      std::vector<uint8_t> want((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
      std::vector<uint8_t> got(be(i * 12 + 8));
      ++files;
      if (vdisc.read(be(i * 12 + 4), (uint32_t)got.size(), got.data()) != got.size() || got != want) {
        ++bad;
        std::cout << "[VERIFY] MISMATCH " << (dirs.back().second / name).string() << std::endl;
      }
    }
    std::cout << "[VERIFY] " << files << " files, " << bad << " mismatches, "
              << std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count() << " s" << std::endl;
    return bad ? 2 : 0;
  }

  // Console type (Dolphin BootManager): Wii retail Hollywood = 0x10000006,
  
  uint32_t console_type = is_gc ? 0x00000003 : 0x10000006;
  uint32_t mem1_size = 24 * 1024 * 1024; 
  uint32_t mem2_size = 64 * 1024 * 1024; 

  ctx->mmu.write32(0x80000020, is_gc ? 0x0D15EA5E : console_type); 
  
  ctx->mmu.write32(0x80000024, mem1_size);
  
  ctx->mmu.write32(0x80000028, mem1_size);
  ctx->mmu.write32(0x8000002C, console_type);
  // Arena bounds, apploader-style (matches Dolphin BS2 emulation):

  

  if (!is_gc) {
    ctx->mmu.write32(0x80000030, arena_lo);
    ctx->mmu.write32(0x80000034, arena_hi);
  }
  if (!vdisc.valid()) {
    
    ctx->mmu.write32(0x80000038, arena_hi);
  }
  if (is_gc) {

    

    uint32_t gc_arena_hi = arena_hi;
    uint32_t fst_base = ctx->mmu.read32(0x80000038);
    if (fst_base >= 0x80000000 && fst_base < 0x81800000)
      gc_arena_hi = fst_base & ~31u;
    // bi2.bin: disc offset 0x440 (sys/bi2.bin in an extracted folder).
    std::vector<uint8_t> bi2_data(0x2000);
    if (vdisc.valid() && vdisc.read(0x440, 0x2000, bi2_data.data()) == 0x2000) {
      uint32_t bi2_addr = 0x81200000; 
      for (size_t i = 0; i < bi2_data.size(); ++i)
        ctx->mmu.write8(bi2_addr + (uint32_t)i, (uint8_t)bi2_data[i]);
      ctx->mmu.write32(0x800000F4, bi2_addr);
      std::cout << "[Loader] BI2 loaded at 0x" << std::hex << bi2_addr
                << std::dec << std::endl;
    }

    
    
    ctx->mmu.write32(0x80000030, arena_lo);
    ctx->mmu.write32(0x80000034, gc_arena_hi);
    std::cout << "[Loader] GC Arena = 0x" << std::hex << arena_lo << " - 0x"
              << gc_arena_hi << " (" << std::dec
              << (gc_arena_hi - arena_lo) / 1024 << " KiB)" << std::endl;
  }
  
  ctx->mmu.write32(0x800000F0, mem1_size);

  

  
  
  auto poke_global = [&](uint32_t addr, uint32_t val) {
    for (const auto &sec : exe.sections)
      if (!sec.is_bss && addr >= sec.address && addr < sec.address + sec.size)
        return; 
    ctx->mmu.write32(addr, val);
  };

  if (!is_gc) {

    poke_global(0x80003118, mem2_size);
    poke_global(0x8000311C, mem2_size);
    
    poke_global(0x80003124, 0x90000800);
    poke_global(0x80003128, 0x93e00000);
    poke_global(0x80003130, 0x93e00000);
    poke_global(0x80003134, 0x94000000);
    
    poke_global(0x80003158, 0x00000023);
  }

  

  

  {
    const std::string &gid = nwii::runtime::Config::get().game_id;
    char region = gid.size() >= 4 ? gid[3] : 'E';
    
    bool pal = (region == 'P' || region == 'D' || region == 'F' ||
                region == 'I' || region == 'S' || region == 'H' ||
                region == 'U' || region == 'X' || region == 'Y' ||
                region == 'Z');
    poke_global(0x800000CC, pal ? 1u : 0u);
  }

  ctx->gpr[1] = 0x816FFFF0;

  // Low memory 0xF8/0xFC hold the BUS and CPU clocks (Dolphin writes

  
  
  if (is_gc) {
    ctx->mmu.write32(0x800000F8, 162000000); 
    ctx->mmu.write32(0x800000FC, 486000000); 
    ctx->tb_freq = 40500000;                 
  } else {
    ctx->mmu.write32(0x800000F8, 243000000); 
    ctx->mmu.write32(0x800000FC, 729000000); 
    ctx->tb_freq = 60750000;                 
  }

  ctx->tb_start = std::chrono::steady_clock::now();

  nwii::runtime::init_ipc_client(*ctx);

  std::thread cpu_thread(cpu_thread_func, ctx.get());
  nwii::runtime::native_prof_start(cpu_thread); // NWII_NPROF=1

  if (headless) {
    
    uint64_t tick = 0;
    while (ctx->is_running) {
      std::this_thread::sleep_for(std::chrono::milliseconds(16));

      
      nwii::runtime::input::InputManager::get().update();

      extern void ProcessGXFifo();
      ProcessGXFifo();
      ctx->vblank_pending = true;
      if (s_env_sample && (++tick % 60) == 0) {
        std::cout << "[Sample] pc=0x" << std::hex << ctx->pc << " lr=0x"
                  << ctx->lr << " msr=0x" << ctx->msr << " r3=0x" << ctx->gpr[3]
                  << " cb=" << (ctx->in_callback ? 1 : 0) << " inst=" << std::dec
                  << ctx->inst_count << "\n";
        
        if (const char *env = std::getenv("NWII_PEEK")) {
          uint32_t pa = 0, pw = 8;
          if (std::sscanf(env, "%x,%u", &pa, &pw) >= 1 && pa) {
            std::cout << "[Peek] " << std::hex << pa << ":";
            for (uint32_t i = 0; i < pw && i < 32; i++)
              std::cout << " " << ctx->mmu.read32(pa + i * 4);
            std::cout << std::dec << "\n";
          }
        }

        
        if ((tick % 300) == 0 && ctx->gpr[13] >= 0x80000000u) {
          uint32_t r13 = ctx->gpr[13];
          std::cout << "  [RunQ] bitmap=0x" << std::hex
                    << ctx->mmu.read32(r13 - 0x21c8) << " resched=0x"
                    << ctx->mmu.read32(r13 - 0x21c4) << " cur=0x"
                    << ctx->mmu.read32(0x800000E4)

                    << " retrace=" << std::dec << ctx->mmu.read32(r13 - 0x2358)
                    << std::hex << " postCB=0x" << ctx->mmu.read32(r13 - 0x2344)
                    << std::dec << "\n";

          
          uint32_t vs = ctx->mmu.read32(r13 - 0x29c0);

          
          uint32_t ef = ctx->mmu.read32(r13 - 0x2AF0);
          std::cout << "  [FlipG] vs=0x" << std::hex << vs
                    << " vsA2=" << std::dec << (int)ctx->mmu.read8(vs + 0xa2)
                    << " pend29b6=" << (int)ctx->mmu.read8(r13 - 0x29b6)
                    << " b29b5=" << (int)ctx->mmu.read8(r13 - 0x29b5)
                    << " b4808=" << (int)ctx->mmu.read8(r13 - 0x4808)
                    << " ef=0x" << std::hex << ef
                    << " efB0=0x" << (ef ? ctx->mmu.read32(ef + 0xb0) : 0)
                    << " efB4=0x" << (ef ? ctx->mmu.read32(ef + 0xb4) : 0)
                    << " efB8=" << std::dec << (ef ? (int)ctx->mmu.read8(ef + 0xb8) : -1)
                    << " efBE=" << (ef ? (int)ctx->mmu.read8(ef + 0xbe) : -1)
                    << " ef9C=" << (ef ? (int)ctx->mmu.read32(ef + 0x9c) : -1)
                    << " ddflag=" << (int)ctx->mmu.read8(r13 - 0x2368)
                    << std::hex << " tok=0x" << ctx->mmu.read16(r13 - 0x29b8)
                    << " pe_sr=0x" << nwii::runtime::hw::g_pe_sr
                    << std::dec << "\n";

          
          std::cout << "  [VIwrap] hwp=0x" << std::hex
                    << ctx->mmu.read32(r13 - 0x4118) << " shp=0x"
                    << ctx->mmu.read32(r13 - 0x239c) << " sdkPre=0x"
                    << ctx->mmu.read32(r13 - 0x2348) << " wrapPre=0x"
                    << ctx->mmu.read32(r13 - 0x237c) << std::dec << "\n";
        }

        
        
        if ((tick % 300) == 0) {
          uint32_t cur = ctx->mmu.read32(0x800000E4);
          uint32_t th = ctx->mmu.read32(0x800000DC);
          int guard = 0;
          while (th >= 0x80000000u && th < 0x81800000u && guard++ < 16) {
          uint32_t q = 0x803540e0;
          std::cout << "Queue " << std::hex << q << ": msgArray=" << ctx->mmu.read32(q+16) << " count=" << ctx->mmu.read32(q+20) << " first=" << ctx->mmu.read32(q+24) << " used=" << ctx->mmu.read32(q+28) << "\n";

            uint16_t state = ctx->mmu.read16(th + 0x2C8);
            uint32_t prio = ctx->mmu.read32(th + 0x2D0);
            uint32_t wq = ctx->mmu.read32(th + 0x2DC);
            uint32_t srr0 = ctx->mmu.read32(th + 0x198);
            std::cout << "  [Thr] 0x" << std::hex << th << (th == cur ? "*" : " ")
                      << " st=" << std::dec << state << " pr=" << prio
                      << " wq=0x" << std::hex << wq << " srr0=0x" << srr0;
            if (wq >= 0x80000000u && wq < 0x81800000u) {
              std::cout << " wq[-2..5]:";
              for (int i = -2; i < 6; ++i)
                std::cout << " " << ctx->mmu.read32(wq + i * 4);
            }
            std::cout << std::dec << "\n";
            th = ctx->mmu.read32(th + 0x2FC);
          }
        }
      }
    }
  } else {
    
    std::vector<unsigned char> xfb_px;
    unsigned xfb_w = 0, xfb_h = 0;
    bool quit = false;

    while (!quit && ctx->is_running) {
      if (!headless) {
        SDL_Event e;
        while (SDL_PollEvent(&e)) {
          if (e.type == SDL_QUIT) quit = true;
          nwii::runtime::ui::handle_event(e); // F1 settings overlay
        }
      }

      // The game sees a neutral pad while the settings overlay is open.
      nwii::runtime::input::InputManager::get().set_suppressed(nwii::runtime::ui::is_open());
      nwii::runtime::input::InputManager::get().update();

      if (!headless) {
        glBindFramebuffer(GL_FRAMEBUFFER, nwii::runtime::efb::draw_fbo);
        glViewport(0, 0, nwii::runtime::efb::width, nwii::runtime::efb::height);
      }

      extern void GX_GetXfb(uint32_t*, unsigned*, unsigned*, unsigned*);
      uint32_t xfb_addr; unsigned xw, xh, xstride;
      GX_GetXfb(&xfb_addr, &xw, &xh, &xstride);
      if (nwii::runtime::hw::g_vi_top_field_base != 0) {
        xfb_addr = nwii::runtime::hw::g_vi_top_field_base;
      }

      if (xfb_addr && xw && xh && xw <= 720 && xh <= 576 && std::getenv("NWII_XFB") && !headless) {
        xfb_px.resize((size_t)xw * xh * 4);
        auto clamp8 = [](float v) -> unsigned char { return (unsigned char)(v < 0 ? 0 : v > 255 ? 255 : v); };
        for (unsigned y = 0; y < xh; ++y) {
          uint32_t row = (xfb_addr | 0x80000000u) + (uint32_t)y * xstride;
          for (unsigned x = 0; x < xw; x += 2) {
            uint8_t y0 = ctx->mmu.read8(row + x * 2 + 0);
            uint8_t cb = ctx->mmu.read8(row + x * 2 + 1);
            uint8_t y1 = ctx->mmu.read8(row + x * 2 + 2);
            uint8_t cr = ctx->mmu.read8(row + x * 2 + 3);
            float fcb = (float)cb - 128.0f, fcr = (float)cr - 128.0f;
            for (int k = 0; k < 2; ++k) {
              float yy = (float)(k ? y1 : y0);
              size_t p = ((size_t)y * xw + x + k) * 4;
              if (x + (unsigned)k >= xw) break;
              xfb_px[p + 0] = clamp8(yy + 1.371f * fcr);
              xfb_px[p + 1] = clamp8(yy - 0.336f * fcb - 0.698f * fcr);
              xfb_px[p + 2] = clamp8(yy + 1.732f * fcb);
              xfb_px[p + 3] = 255;
            }
          }
        }
        glBindTexture(GL_TEXTURE_2D, xfb_tex);
        if (xw != xfb_w || xh != xfb_h) {
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, xw, xh, 0, GL_RGBA, GL_UNSIGNED_BYTE, xfb_px.data());
          xfb_w = xw; xfb_h = xh;
        } else {
          glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, xw, xh, GL_RGBA, GL_UNSIGNED_BYTE, xfb_px.data());
        }
      }

      extern void ProcessGXFifo();
      ProcessGXFifo();

      

      // NWII_PCPROF=1: sample the guest pc/lr once per host frame and print
      // the hottest pairs every 5 s -- a poor man's profiler for "where is
      // the CPU thread spending its time".
      static const bool pcprof = std::getenv("NWII_PCPROF") != nullptr;
      if (pcprof) {
        static std::unordered_map<uint64_t, uint32_t> hist;
        static auto p0 = std::chrono::steady_clock::now();
        static uint64_t inst0 = 0;
        hist[((uint64_t)ctx->pc << 32) | ctx->lr]++;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - p0).count() >= 5) {
          p0 = now;
          std::vector<std::pair<uint32_t, uint64_t>> v;
          for (auto &kv : hist) v.push_back({kv.second, kv.first});
          std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.first > b.first; });
          std::cout << "[PCPROF] inst/5s=" << std::dec << (ctx->inst_count - inst0)
                    << " cur=0x" << std::hex << ctx->mmu.read32(0x800000E4);
          for (size_t i = 0; i < v.size() && i < 10; ++i)
            std::cout << " | " << std::dec << v[i].first << "x pc=0x" << std::hex
                      << (uint32_t)(v[i].second >> 32) << " lr=0x"
                      << (uint32_t)v[i].second;
          std::cout << std::dec << "\n";
          inst0 = ctx->inst_count;
          hist.clear();
        }
      }

      if (!headless) {
        static auto t0 = std::chrono::steady_clock::now();
        static int stat_n = 0;
        auto now = std::chrono::steady_clock::now();
        if (std::chrono::duration_cast<std::chrono::seconds>(now - t0).count() >=
            5) {
          t0 = now;
          extern uint64_t g_stat_frames, g_stat_draws;
          extern uint64_t g_stat_parse_us, g_stat_render_us;
          extern uint64_t nwii_stat_hash0(), nwii_stat_shaders();
          // efb_nonblack reads the whole EFB back (a full GPU sync plus ~15 MB
          // at 3x widescreen): a visible hitch every 5 s on slower GPUs, so it
          // is only measured with NWII_VERBOSE=1.
          size_t nonblack = 0;
          if (nwii::runtime::g_verbose) {
            namespace efb = nwii::runtime::efb;
            efb::resolve();
            std::vector<unsigned char> p((size_t)efb::width * efb::height * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::read_fbo());
            glReadPixels(0, 0, efb::width, efb::height, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            for (size_t i = 0; i < p.size(); i += 4)
              if (p[i] | p[i + 1] | p[i + 2])
                ++nonblack;
            nonblack = nonblack * 307200 / (p.size() / 4);
          }
          std::cout << "[STAT] " << ++stat_n * 5 << "s frames=" << std::dec
                    << g_stat_frames << " draws=" << g_stat_draws
                    << " parse_ms=" << g_stat_parse_us / 1000
                    << " render_ms=" << g_stat_render_us / 1000
                    << " efb_nonblack=" << nonblack << "/307200"
                    << " shaders=" << nwii_stat_shaders() << " hash0=0x"
                    << std::hex << nwii_stat_hash0();
          g_stat_parse_us = 0;
          g_stat_render_us = 0;

          std::cout << " E0..E7=";
          for (int i = 0xE0; i <= 0xE7; ++i)
            std::cout << nwii::runtime::gx::g_state.bp[i] << ",";

          
          std::cout << std::dec << " projType="
                    << nwii::runtime::gx::g_state.projType
                    << " projSet=" << (int)nwii::runtime::gx::g_state.projSet
                    << " proj=";
          for (int i = 0; i < 6; ++i)
            std::cout << nwii::runtime::gx::g_state.projection[i] << ",";

          
          uint32_t base = nwii::runtime::gx::g_state.texStages[0].base_addr;
          uint32_t toff = nwii::runtime::gx::g_state.texStages[0].tlut_offset;
          uint32_t tex_nz = 0, tlut_nz = 0;
          if (nwii::runtime::g_ctx_ptr) {
            for (uint32_t i = 0; i < 2048; ++i)
              if (nwii::runtime::g_ctx_ptr->mmu.read8(base + i))
                ++tex_nz;
          }
          for (uint32_t i = 0; i < 512; ++i) {
            uint32_t idx = toff + i;
            if (idx < sizeof(nwii::runtime::gx::g_state.tlutMem) &&
                nwii::runtime::gx::g_state.tlutMem[idx])
              ++tlut_nz;
          }
          std::cout << " tex0=" << std::hex << base << std::dec << "/"
                    << nwii::runtime::gx::g_state.texStages[0].width << "x"
                    << nwii::runtime::gx::g_state.texStages[0].height << "/f"
                    << (int)nwii::runtime::gx::g_state.texStages[0].format
                    << " texnz=" << tex_nz << "/2048 tlutnz=" << tlut_nz << "/512"
                    << std::endl;
        }
      }

      if (!headless) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        int draw_w = 0, draw_h = 0;
        SDL_GL_GetDrawableSize(window, &draw_w, &draw_h);
        glViewport(0, 0, draw_w, draw_h);
        glClearColor(0, 0, 0, 1);
        glClear(GL_COLOR_BUFFER_BIT);

        namespace efb = nwii::runtime::efb;
        // Present the XFB the VI scans out, not the EFB in progress.
        efb::select_display(nwii::runtime::hw::g_vi_top_field_base);
        glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::display_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        {
          // Present what the game displays (its last EFB->XFB copy, e.g.
          // 640x448) at its display aspect: square native pixels, 4/3 wider
          // in widescreen. Fit to the window, centred, bars where it differs.
          const efb::Rect src = efb::to_host((float)efb::xfb_x, (float)efb::xfb_y,
                                             (float)efb::xfb_w, (float)efb::xfb_h);
          // The game's own anamorphic widescreen also gets the 16:9 stretch.
          extern bool g_game_anamorphic_display();
          const float aspect = (float)efb::xfb_w / (float)efb::xfb_h *
                               (efb::widescreen || g_game_anamorphic_display() ? 4.0f / 3.0f : 1.0f);
          int dw = draw_w, dh = (int)(draw_w / aspect + 0.5f);
          if (dh > draw_h) { dh = draw_h; dw = (int)(draw_h * aspect + 0.5f); }
          const int dx = (draw_w - dw) / 2, dy = (draw_h - dh) / 2;
          glBlitFramebuffer(src.x, src.y, src.x + src.w, src.y + src.h, dx, dy,
                            dx + dw, dy + dh, GL_COLOR_BUFFER_BIT, GL_LINEAR);
        }

        // NWII_PURPLE_TRIGGER=1 (test): every 15 presents look for the solid
        // purple water glitch; when >2% of the frame is that colour, draw-log
        // the next game frame (once) and save a screenshot of this one.
        // NWII_STRIP_TRIGGER=<present> (test, from that present on): a nearly black frame with a few bright
        // pixels (the garbled text strip before the hub spawn) draw-logs the
        // next game frame, once.
        if (static const char *strip = std::getenv("NWII_STRIP_TRIGGER"); strip) {
          static int sn = 0;
          static const int strip_from = std::atoi(strip); // first present to check
          static bool sfired = false;
          extern uint64_t g_drawlog_frame_req, g_stat_frames;
          if (!sfired && ++sn % 3 == 0 && sn >= strip_from) {
            const int w = efb::width, h = efb::height;
            std::vector<unsigned char> px((size_t)w * h * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::display_fbo);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            size_t bright = 0, n = 0;
            for (size_t i = 0; i < px.size(); i += 4 * 5, ++n)
              if (px[i] + px[i + 1] + px[i + 2] > 120) ++bright;
            if (bright > 20 && bright * 100 < n) {
              sfired = true;
              g_drawlog_frame_req = g_stat_frames + 1;
              std::printf("[STRIP] %zu bright samples at frame %llu, logging next\n", bright,
                          (unsigned long long)g_stat_frames);
            }
          }
        }
        if (static const bool purple = std::getenv("NWII_PURPLE_TRIGGER") != nullptr; purple) {
          static int pn = 0;
          static bool fired = false;
          extern uint64_t g_drawlog_frame_req, g_stat_frames;
          if (!fired && ++pn % 15 == 0) {
            const int w = efb::width, h = efb::height;
            std::vector<unsigned char> px((size_t)w * h * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::display_fbo);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
            size_t hits = 0, n = 0;
            for (size_t i = 0; i < px.size(); i += 4 * 7, ++n)
              if (std::abs(px[i] - 76) < 8 && px[i + 1] < 28 && std::abs(px[i + 2] - 103) < 8) ++hits;
            if (hits * 50 > n) {
              fired = true;
              g_drawlog_frame_req = g_stat_frames + 1;
              std::printf("[PURPLE] %.1f%% at frame %llu, logging next\n",
                          100.0 * hits / n, (unsigned long long)g_stat_frames);
              std::vector<unsigned char> flip(px.size());
              for (int y = 0; y < h; ++y)
                std::memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
              for (size_t i = 3; i < flip.size(); i += 4) flip[i] = 255;
              if (SDL_Surface *s = SDL_CreateRGBSurfaceFrom(flip.data(), w, h, 32, w * 4, 0xFF,
                                                            0xFF00, 0xFF0000, 0xFF000000)) {
                SDL_SaveBMP(s, "purple_frame.bmp");
                SDL_FreeSurface(s);
              }
            }
          }
        }

        // NWII_MEMDUMP=<present>:<file>: write MEM1 (24 MB, big-endian as in
        // the guest) to a file once, for offline analysis.
        if (static const char *md = std::getenv("NWII_MEMDUMP"); md) {
          static unsigned at = (unsigned)std::atoi(md), n = 0;
          if (++n == at) {
            const char *path = std::strchr(md, ':');
            if (FILE *f = std::fopen(path ? path + 1 : "mem1.bin", "wb")) {
              std::fwrite(ctx->mmu.mem1.data(), 1, 24u << 20, f);
              std::fclose(f);
              // ARAM (backed by mem2 here) next to it, as <file>.aram.
              if (FILE *g = std::fopen((std::string(path ? path + 1 : "mem1.bin") + ".aram").c_str(), "wb")) {
                std::fwrite(ctx->mmu.mem2.data(), 1, 16u << 20, g);
                std::fclose(g);
              }
              std::printf("[MEMDUMP] wrote MEM1 at present %u\n", n);
            }
          }
        }

        // NWII_FIELDS=<hexaddr>,<bytes>,<every>: print guest words (and their
        // float reading) every n presents -- watching an object's fields.
        if (static const char *pk = std::getenv("NWII_FIELDS"); pk) {
          static uint32_t pk_addr = 0, pk_len = 0, pk_every = 30, pk_n = 0;
          if (!pk_len) {
            pk_addr = (uint32_t)std::strtoul(pk, nullptr, 16);
            const char *c = std::strchr(pk, ',');
            pk_len = c ? (uint32_t)std::strtoul(c + 1, nullptr, 0) : 64;
            if (c && (c = std::strchr(c + 1, ','))) pk_every = std::max(1, std::atoi(c + 1));
          }
          if (++pk_n % pk_every == 0) {
            std::printf("[FIELDS] %u", pk_n);
            for (uint32_t o = 0; o < pk_len; o += 4) {
              uint32_t w = ctx->mmu.read32(pk_addr + o);
              float f;
              std::memcpy(&f, &w, 4);
              if (w && std::fabs(f) > 1e-6f && std::fabs(f) < 1e7f)
                std::printf(" %X=%g", o, f);
              else
                std::printf(" %X=%08X", o, w);
            }
            std::printf("\n");
          }
        }

        // NWII_POKE16=<present>:<hexptr>:<off>:<val>: once, at that present,
        // write a halfword at *(ptr)+off (poke game state through a pointer).
        if (static const char *pk16 = std::getenv("NWII_POKE16"); pk16) {
          static unsigned at = 0, ptr = 0, off = 0, val = 0, n = 0;
          static bool parsed = std::sscanf(pk16, "%u:%x:%x:%u", &at, &ptr, &off, &val) == 4;
          if (parsed && ++n == at) {
            uint32_t base = ctx->mmu.read32(ptr);
            std::printf("[POKE16] *(%08X)=%08X +%X: %u -> %u\n", ptr, base, off,
                        ctx->mmu.read16(base + off), val);
            ctx->mmu.write16(base + off, (uint16_t)val);
          }
        }

        // NWII_CTRL=<file>: interactive test driver. Each line of <file> is
        // "press NAME ms" (NWII_INPUT_SCRIPT names), "interp on|off"
        // (interpreter mode, see g_interp_mode) or "shot"; the file is
        // consumed, and "shot" writes the displayed frame to <file>.bmp.
        bool ctrl_shot = false;
        static const char *ctrl = std::getenv("NWII_CTRL");
        if (ctrl) {
          static int ctrl_tick = 0;
          if (++ctrl_tick % 10 == 0) {
            if (FILE *cf = std::fopen(ctrl, "rb")) {
              char line[128];
              while (std::fgets(line, sizeof line, cf)) {
                char cmd[16] = {}, name[16] = {};
                int ms = 200;
                const int n = std::sscanf(line, "%15s %15s %d", cmd, name, &ms);
                if (n >= 2 && std::strcmp(cmd, "press") == 0)
                  nwii::runtime::input::inject_press(name, ms);
                else if (n >= 2 && std::strcmp(cmd, "interp") == 0) {
                  // "interp on [hexlimit]": interpret functions below limit.
                  unsigned limit = 0xFFFFFFFFu;
                  std::sscanf(line, "%*s %*s %x", &limit);
                  nwii::runtime::g_interp_limit = limit;
                  nwii::runtime::g_interp_mode = std::strcmp(name, "on") == 0;
                  std::printf("[CTRL] interpreter mode %s (below 0x%08X)\n",
                              nwii::runtime::g_interp_mode ? "on" : "off", limit);
                }
                else if (n >= 1 && std::strcmp(cmd, "drawlog") == 0) {
                  extern uint64_t g_drawlog_frame_req, g_stat_frames;
                  g_drawlog_frame_req = g_stat_frames + 1;
                  std::printf("[CTRL] draw log for frame %llu\n",
                              (unsigned long long)g_drawlog_frame_req);
                }
                else if (n >= 2 && std::strcmp(cmd, "skipcmode") == 0) {
                  // "skipcmode <hex>|off": drop draws with that PE_CMODE0.
                  extern std::atomic<int> g_skip_cmode;
                  g_skip_cmode = std::strcmp(name, "off") == 0 ? -1 : (int)std::strtoul(name, nullptr, 16);
                  std::printf("[CTRL] skipcmode %s\n", name);
                }
                else if (n >= 2 && std::strcmp(cmd, "probe") == 0) {
                  // "probe <x> <y>": the draw log's third pixel sample.
                  extern std::atomic<int> g_probe_x, g_probe_y;
                  g_probe_x = std::atoi(name);
                  g_probe_y = ms;
                  std::printf("[CTRL] probe %d,%d\n", g_probe_x.load(), g_probe_y.load());
                }
                else if (n >= 1 && std::strcmp(cmd, "shot") == 0)
                  ctrl_shot = true;
              }
              std::fclose(cf);
              std::remove(ctrl);
            }
          }
        }
        if (ctrl_shot) {
          const int w = efb::width, h = efb::height;
          std::vector<unsigned char> px((size_t)w * h * 4);
          glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::display_fbo);
          glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
          for (size_t i = 3; i < px.size(); i += 4) px[i] = 255;
          std::vector<unsigned char> flip(px.size());
          for (int y = 0; y < h; ++y)
            std::memcpy(&flip[(size_t)y * w * 4], &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
          if (SDL_Surface *s = SDL_CreateRGBSurfaceFrom(flip.data(), w, h, 32, w * 4, 0x000000FF,
                                                        0x0000FF00, 0x00FF0000, 0xFF000000)) {
            SDL_SaveBMP(s, (std::string(ctrl) + ".bmp").c_str());
            SDL_FreeSurface(s);
          }
          glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
        }

        if (const char *shot_pfx = std::getenv("NWII_SCREENSHOT")) {
          static int shot_frame = 0;
          // NWII_SHOT_EVERY=<n> / NWII_SHOT_FROM=<frame>: a single frame every
          // n frames from that frame on (motion bursts). Default: two
          // consecutive frames every 300, so a present that alternates
          // between a rendered and an empty EFB shows up.
          static const char *ev = std::getenv("NWII_SHOT_EVERY");
          static const char *fr = std::getenv("NWII_SHOT_FROM");
          static const int every = ev ? std::max(1, std::atoi(ev)) : 300;
          static const int from = fr ? std::atoi(fr) : 0;
          ++shot_frame;
          if (ev ? (shot_frame >= from && shot_frame % every == 0)
                 : (shot_frame % 300) <= 1) {
            const int w = efb::width, h = efb::height;
            std::vector<unsigned char> px((size_t)w * h * 4);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::display_fbo);
            glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
            if (std::getenv("NWII_SHOTLOG"))
              std::printf("[SHOT] %d display_fbo=%u xfb=%d,%d %dx%d vi=%08X\n", shot_frame,
                          efb::display_fbo, efb::xfb_x, efb::xfb_y, efb::xfb_w, efb::xfb_h,
                          nwii::runtime::hw::g_vi_top_field_base);
            // EFB alpha is meaningless in RGB8 formats (and never written
            // there); an alpha-0 BMP shows as black, so save it opaque.
            for (size_t i = 3; i < px.size(); i += 4)
              px[i] = 255;
            std::vector<unsigned char> flip(px.size());
            for (int y = 0; y < h; ++y)
              std::memcpy(&flip[(size_t)y * w * 4],
                          &px[(size_t)(h - 1 - y) * w * 4], (size_t)w * 4);
            SDL_Surface *s = SDL_CreateRGBSurfaceFrom(
                flip.data(), w, h, 32, w * 4, 0x000000FF, 0x0000FF00,
                0x00FF0000, 0xFF000000);
            if (s) {
              std::string p = std::string(shot_pfx) + "_" +
                              std::to_string(shot_frame) + ".bmp";
              SDL_SaveBMP(s, p.c_str());
              SDL_FreeSurface(s);
            }
            glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
          }
        }

        nwii::runtime::ui::frame();
        SDL_GL_SwapWindow(window);

        const auto req = nwii::runtime::ui::take_requests();
        if (req.rebuild_efb) nwii::runtime::efb::create();
        if (req.vsync)
          SDL_GL_SetSwapInterval(nwii::runtime::Config::get().enable_vsync ? 1 : 0);
        if (req.fullscreen)
          SDL_SetWindowFullscreen(window, nwii::runtime::Config::get().fullscreen
                                              ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
        if (req.quit) quit = true;
      }

      // (Host audio is fed by audio_thread.)
      
      ctx->vblank_pending = true;
      if (headless) std::this_thread::sleep_for(std::chrono::milliseconds(16));
      // Without VSync the swap no longer paces this loop; spinning flat out
      // starved the CPU thread (the game stalled during the logos). Cap the
      // host loop at ~250 Hz -- still well above any game frame rate.
      if (!headless && !nwii::runtime::Config::get().enable_vsync) {
        static auto next = std::chrono::steady_clock::now();
        next += std::chrono::microseconds(4000);
        const auto now = std::chrono::steady_clock::now();
        if (next > now)
          std::this_thread::sleep_until(next);
        else
          next = now;
      }
    }

    if (!headless) {
      audio_stop = true;
      if (audio_thread.joinable()) audio_thread.join();
      if (audio_dev > 0) SDL_CloseAudioDevice(audio_dev);
      nwii::runtime::ui::shutdown();
      SDL_GL_DeleteContext(gl_ctx);
      SDL_DestroyWindow(window);
      SDL_Quit();
    }
  }

  ctx->is_running = false;
  ctx->pc = 0; 
  if (cpu_thread.joinable())
    cpu_thread.join();

  nwii::runtime::shutdown();
  return 0;
}