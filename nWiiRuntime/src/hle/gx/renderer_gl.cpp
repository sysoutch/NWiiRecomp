#include "runtime/efb.h"
#include "runtime/config.h"
#include <algorithm>
#include <atomic>
#include "runtime/cpu_context.h"
#include "runtime/gx/fifo_parser.h"
#include "runtime/gx/renderer.h"
#include "runtime/gx/tev_shader_gen.h"
#include "runtime/gx_state.h"
#include "runtime/texture_cache_gl.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <glad/glad.h>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace nwii::runtime {
extern MMU *g_mmu;
extern CPUContext *g_ctx_ptr;
} // namespace nwii::runtime

void GX_GetClearColor(unsigned char rgba[4]);
extern uint64_t g_stat_frames;
// Frame set by the host (main.cpp NWII_PURPLE_TRIGGER) to draw-log on demand.
uint64_t g_drawlog_frame_req = ~0ull;
// PE_CMODE0 value whose draws are dropped (-1: none); see NWII_SKIP_CMODE.
std::atomic<int> g_skip_cmode{-1};
// Native screen pixel the draw log samples after every draw (-1: default).
std::atomic<int> g_probe_x{-1}, g_probe_y{-1};

namespace nwii::runtime::gx {

// NWII_DRAWLOG_FRAME=<n> or <first>-<last>: log every draw and EFB copy of
// those frames (the plain NWII_DRAWLOG only covers the first 40 draws).
static bool frame_log_active() {
  static const char *env = std::getenv("NWII_DRAWLOG_FRAME");
  static uint64_t first = 0, last = 0;
  static const bool parsed = [] {
    if (!env) return false;
    char *end = nullptr;
    first = last = std::strtoull(env, &end, 10);
    if (end && *end == '-') last = std::strtoull(end + 1, nullptr, 10);
    return true;
  }();
  return (parsed && g_stat_frames >= first && g_stat_frames <= last) ||
         g_stat_frames == g_drawlog_frame_req;
}

// NWII_FADELOG=<n>: every n frames, one line per draw with the colour-channel
// material registers -- enough to see whether a fade is animating.
static bool fade_log_active() {
  static const char *env = std::getenv("NWII_FADELOG");
  static const uint64_t every = env ? std::strtoull(env, nullptr, 10) : 0;
  return every && (g_stat_frames % every) == 0;
}

static bool gx_trace() {
  static bool t = std::getenv("NWII_GXTRACE") != nullptr;
  return t;
}

struct GLShader {
  GLuint id;
  GLint uTex[8];
  GLint uTexMtx[8];
  GLint uTevKColor;
  GLint uTevColor;
  GLint uProjMtx;
  GLint uMatColor, uAmbColor, uChanCtrl;
  GLint uLightCol, uLightPos, uLightDir, uLightCosAtt, uLightDistAtt;
};
static std::unordered_map<uint64_t, GLShader> s_shader_cache;

uint64_t g_stat_hash0 = 0;
uint64_t g_stat_shaders = 0;

struct BatchVertex {
  float x, y, z;
  float r, g, b, a;
  float tc[8][3]; // texgen outputs (s, t, q) for texcoords 0-7
  float nx, ny, nz;
};

static std::vector<BatchVertex> s_batch;
// Triangle indices into s_batch: each source vertex is stored once (strips
// and fans reuse vertices up to three times -- uploading and shading the
// copies tripled the vertex work on CPU and GPU).
static std::vector<uint32_t> s_indices;
static float s_proj[16] = {1,0,0,0, 0,1,0,0, 0,0,1,0, 0,0,0,1};
static GLuint s_vao = 0, s_vbo = 0, s_ibo = 0;
static GLShader s_active_shader = {0};

// --- Persistent shader cache -------------------------------------------
// New GX render states compile a new TEV program the first time they appear
// (up to ~130 ms hitches while moving through a level). Linked programs are
// saved as driver binaries in <config dir>/shader_cache/<state hash>.bin and
// loaded on later runs; a hash of the generated source in the header keeps
// stale binaries out after shader-generator changes. glad (GL 3.3) lacks the
// GL 4.1 entry points, so they are looked up at runtime.
// NWII_NO_SHADER_CACHE=1 disables it.
extern "C" void *SDL_GL_GetProcAddress(const char *proc);
namespace shader_cache {
using PFN_GetProgramBinary = void(APIENTRY *)(GLuint, GLsizei, GLsizei *, GLenum *, void *);
using PFN_ProgramBinary = void(APIENTRY *)(GLuint, GLenum, const void *, GLsizei);
using PFN_ProgramParameteri = void(APIENTRY *)(GLuint, GLenum, GLint);
constexpr GLenum kBinaryLength = 0x8741, kRetrievableHint = 0x8257;
constexpr uint32_t kMagic = 0x53484331; // "SHC1"
struct Header {
  uint32_t magic;
  uint32_t format;
  uint64_t src_hash;
  uint32_t length;
};

static PFN_GetProgramBinary get_binary;
static PFN_ProgramBinary load_binary;
static PFN_ProgramParameteri param;
static std::string dir;

static bool enabled() {
  static const bool on = [] {
    if (std::getenv("NWII_NO_SHADER_CACHE")) return false;
    get_binary = (PFN_GetProgramBinary)SDL_GL_GetProcAddress("glGetProgramBinary");
    load_binary = (PFN_ProgramBinary)SDL_GL_GetProcAddress("glProgramBinary");
    param = (PFN_ProgramParameteri)SDL_GL_GetProcAddress("glProgramParameteri");
    if (!get_binary || !load_binary) return false;
    std::filesystem::path base =
        std::filesystem::path(nwii::runtime::Config::get().config_path).parent_path();
    std::error_code ec;
    std::filesystem::create_directories(base / "shader_cache", ec);
    dir = (base / "shader_cache").string();
    return !ec;
  }();
  return on;
}

static uint64_t fnv64(const char *a, const char *b) {
  uint64_t h = 1469598103934665603ull;
  for (const char *s : {a, b})
    for (; *s; ++s) h = (h ^ (uint8_t)*s) * 1099511628211ull;
  return h;
}

static std::string path_for(uint64_t key) {
  char name[32];
  std::snprintf(name, sizeof name, "%016llx.bin", (unsigned long long)key);
  return dir + "/" + name;
}

// A linked program from the cache, or 0.
static GLuint load(uint64_t key, uint64_t src_hash) {
  FILE *f = std::fopen(path_for(key).c_str(), "rb");
  if (!f) return 0;
  Header h{};
  std::vector<char> data;
  bool ok = std::fread(&h, sizeof h, 1, f) == 1 && h.magic == kMagic &&
            h.src_hash == src_hash && h.length > 0 && h.length < (64u << 20);
  if (ok) {
    data.resize(h.length);
    ok = std::fread(data.data(), 1, h.length, f) == h.length;
  }
  std::fclose(f);
  if (!ok) return 0;
  GLuint p = glCreateProgram();
  load_binary(p, h.format, data.data(), (GLsizei)h.length);
  GLint linked = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &linked);
  if (!linked) { // driver update etc.: recompile
    glDeleteProgram(p);
    return 0;
  }
  return p;
}

static void save(uint64_t key, uint64_t src_hash, GLuint p) {
  GLint len = 0;
  glGetProgramiv(p, kBinaryLength, &len);
  if (len <= 0) return;
  std::vector<char> data((size_t)len);
  GLenum format = 0;
  GLsizei got = 0;
  get_binary(p, len, &got, &format, data.data());
  if (got <= 0) return;
  const std::string path = path_for(key), tmp = path + ".tmp";
  if (FILE *f = std::fopen(tmp.c_str(), "wb")) {
    Header h{kMagic, format, src_hash, (uint32_t)got};
    const bool ok = std::fwrite(&h, sizeof h, 1, f) == 1 &&
                    std::fwrite(data.data(), 1, (size_t)got, f) == (size_t)got;
    std::fclose(f);
    std::error_code ec;
    if (ok) std::filesystem::rename(tmp, path, ec);
    else std::filesystem::remove(tmp, ec);
  }
}
} // namespace shader_cache

static GLuint CompileShader(const char *vs, const char *fs);

// CompileShader through the persistent cache (key: GX shader state hash).
static GLuint CompileShaderCached(uint64_t key, const char *vs, const char *fs) {
  if (!shader_cache::enabled()) return CompileShader(vs, fs);
  static const bool log = std::getenv("NWII_HITCHLOG") != nullptr;
  const auto t0 = std::chrono::steady_clock::now();
  auto ms = [&] {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0)
        .count();
  };
  const uint64_t src_hash = shader_cache::fnv64(vs, fs);
  if (GLuint p = shader_cache::load(key, src_hash)) {
    if (log) std::printf("[SHADER] %016llx loaded %.1f ms\n", (unsigned long long)key, ms());
    return p;
  }
  GLuint p = CompileShader(vs, fs);
  GLint linked = 0;
  glGetProgramiv(p, GL_LINK_STATUS, &linked);
  if (linked) shader_cache::save(key, src_hash, p);
  if (log) std::printf("[SHADER] %016llx compiled %.1f ms\n", (unsigned long long)key, ms());
  return p;
}

static GLuint CompileShader(const char *vs, const char *fs) {
  GLuint v = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(v, 1, &vs, nullptr);
  glCompileShader(v);

  GLuint f = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(f, 1, &fs, nullptr);
  glCompileShader(f);

  GLuint p = glCreateProgram();
  if (shader_cache::param) shader_cache::param(p, shader_cache::kRetrievableHint, GL_TRUE);
  glAttachShader(p, v);
  glAttachShader(p, f);
  glLinkProgram(p);

  glDeleteShader(v);
  glDeleteShader(f);
  return p;
}

// Streams `bytes` from `src` into `buf` (bound to `target`), a ring of `cap`
// bytes that is orphaned only when it wraps; returns the byte offset written.
static size_t RingUpload(GLenum target, size_t &pos, size_t cap, const void *src, size_t bytes) {
  if (pos + bytes > cap) {
    glBufferData(target, cap, nullptr, GL_STREAM_DRAW);
    pos = 0;
  }
  void *dst = glMapBufferRange(target, pos, bytes,
                               GL_MAP_WRITE_BIT | GL_MAP_UNSYNCHRONIZED_BIT |
                                   GL_MAP_INVALIDATE_RANGE_BIT);
  if (dst) {
    std::memcpy(dst, src, bytes);
    glUnmapBuffer(target);
  } else {
    glBufferSubData(target, pos, bytes, src);
  }
  const size_t at = pos;
  pos += bytes;
  return at;
}

// A VAO for the BatchVertex layout over these buffers (the element buffer
// binding is recorded in the VAO).
static void SetupBatchVao(GLuint &vao, GLuint vbo, GLuint ibo) {
  glGenVertexArrays(1, &vao);
  glBindVertexArray(vao);
  glBindBuffer(GL_ARRAY_BUFFER, vbo);
  glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ibo);
  glEnableVertexAttribArray(0);
  glEnableVertexAttribArray(1);
  glEnableVertexAttribArray(2);
  glEnableVertexAttribArray(3);
  glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                        (void *)offsetof(BatchVertex, x));
  glVertexAttribPointer(1, 4, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                        (void *)offsetof(BatchVertex, r));
  glVertexAttribPointer(2, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                        (void *)offsetof(BatchVertex, tc));
  glVertexAttribPointer(3, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                        (void *)offsetof(BatchVertex, nx));
  // Texcoords 1-7 at locations 4-10 (see tev_shader_gen.cpp).
  for (int i = 1; i < 8; ++i) {
    glEnableVertexAttribArray(3 + i);
    glVertexAttribPointer(3 + i, 3, GL_FLOAT, GL_FALSE, sizeof(BatchVertex),
                          (void *)(offsetof(BatchVertex, tc) + i * 3 * sizeof(float)));
  }
}

// Persistent mapped rings (GL 4.4 / ARB_buffer_storage): the vertex and index
// buffers are mapped once, coherently, and batches are written straight into
// them -- no map/unmap pair per batch (two driver calls each, ~900 batches a
// frame). Each ring is split into kSegs segments; leaving a segment fences it,
// entering one waits for its fence, so nothing the GPU may still read is
// overwritten. Batches larger than a segment, or no GL 4.4: the streaming
// rings below. NWII_PERSISTENT_VBO=0: off.
struct PersistentRing {
  static constexpr int kSegs = 8;
  GLuint buf = 0;
  uint8_t *ptr = nullptr;
  size_t cap = 0, pos = 0;
  GLsync fence[kSegs] = {};
};
static PersistentRing s_pvbo, s_pibo;
static GLuint s_vao_persistent = 0;

static bool PersistentRingsInit() {
  const char *e = std::getenv("NWII_PERSISTENT_VBO");
  if (e && e[0] == '0') return false;
  GLint major = 0, minor = 0;
  glGetIntegerv(GL_MAJOR_VERSION, &major);
  glGetIntegerv(GL_MINOR_VERSION, &minor);
  bool ext = major > 4 || (major == 4 && minor >= 4);
  if (!ext) {
    GLint n = 0;
    glGetIntegerv(GL_NUM_EXTENSIONS, &n);
    for (GLint i = 0; i < n && !ext; ++i)
      ext = std::strcmp((const char *)glGetStringi(GL_EXTENSIONS, i), "GL_ARB_buffer_storage") == 0;
  }
  using PFN_BufferStorage = void(APIENTRY *)(GLenum, GLsizeiptr, const void *, GLbitfield);
  const auto buffer_storage = ext ? (PFN_BufferStorage)SDL_GL_GetProcAddress("glBufferStorage") : nullptr;
  if (!buffer_storage) return false;
  constexpr GLbitfield kPersistent = 0x0040, kCoherent = 0x0080; // GL_MAP_*_BIT
  const GLbitfield flags = GL_MAP_WRITE_BIT | kPersistent | kCoherent;
  auto make = [&](PersistentRing &r, GLenum target, size_t cap) {
    glGenBuffers(1, &r.buf);
    glBindBuffer(target, r.buf);
    buffer_storage(target, (GLsizeiptr)cap, nullptr, flags);
    r.ptr = (uint8_t *)glMapBufferRange(target, 0, (GLsizeiptr)cap, flags);
    r.cap = cap;
    return r.ptr != nullptr;
  };
  GLuint vao_before = 0;
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, (GLint *)&vao_before);
  glBindVertexArray(0);
  // 16 MB of vertices (2 MB segments), 4 MB of indices (512 KB segments).
  const bool ok = make(s_pvbo, GL_ARRAY_BUFFER, (size_t)(1u << 16) * sizeof(BatchVertex)) &&
                  make(s_pibo, GL_ELEMENT_ARRAY_BUFFER, (size_t)(1u << 20) * sizeof(uint32_t));
  if (ok) SetupBatchVao(s_vao_persistent, s_pvbo.buf, s_pibo.buf);
  glBindVertexArray(vao_before);
  std::printf("[GL] persistent mapped vertex buffers: %s\n", ok ? "on" : "unavailable");
  return ok;
}

// Room for `bytes` in the ring (one segment at most); SIZE_MAX if too big.
static size_t PersistentAlloc(PersistentRing &r, size_t bytes) {
  const size_t seg = r.cap / PersistentRing::kSegs;
  if (bytes > seg) return SIZE_MAX;
  const size_t cur = r.pos / seg;
  if (r.pos + bytes > (cur + 1) * seg || r.pos >= r.cap) {
    const size_t done = std::min(cur, (size_t)PersistentRing::kSegs - 1);
    if (r.fence[done]) glDeleteSync(r.fence[done]);
    r.fence[done] = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    const size_t next = (done + 1) % PersistentRing::kSegs;
    if (GLsync f = r.fence[next]) {
      while (glClientWaitSync(f, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000ull) == GL_TIMEOUT_EXPIRED) {}
      glDeleteSync(f);
      r.fence[next] = nullptr;
    }
    r.pos = next * seg;
  }
  const size_t at = r.pos;
  r.pos += bytes;
  return at;
}

static void FlushBatch() {
  if (s_indices.empty()) {
    s_batch.clear();
    return;
  }

  if (!s_vao) {
    glGenBuffers(1, &s_vbo);
    glGenBuffers(1, &s_ibo);
    SetupBatchVao(s_vao, s_vbo, s_ibo);
  }
  static const bool persistent = PersistentRingsInit();

  glBindVertexArray(s_vao);
  glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
  // Streaming rings: batches go to increasing offsets of one big vertex and
  // one index buffer (unsynchronised maps), orphaned only when they wrap. A
  // glBufferData per batch made the driver create and destroy a buffer
  // allocation for each of thousands of batches per frame (~15% of the
  // render thread in DestroyAllocation alone). NWII_VBO_RING=0: old path.
  static const bool ring = [] {
    const char *e = std::getenv("NWII_VBO_RING");
    return !(e && e[0] == '0');
  }();
  constexpr size_t kVboBytes = (size_t)(1u << 18) * sizeof(BatchVertex); // ~36 MB
  constexpr size_t kIboBytes = (size_t)(1u << 20) * sizeof(uint32_t);    // 4 MB
  static size_t s_vbo_pos = kVboBytes, s_ibo_pos = kIboBytes; // first use allocates
  const size_t n = s_batch.size(), ni = s_indices.size();
  const size_t vbytes = n * sizeof(BatchVertex), ibytes = ni * sizeof(uint32_t);
  GLint first = 0;
  size_t ioff = 0;
  size_t pv = SIZE_MAX, pi = SIZE_MAX;
  if (persistent) {
    pv = PersistentAlloc(s_pvbo, vbytes);
    if (pv != SIZE_MAX) pi = PersistentAlloc(s_pibo, ibytes);
  }
  if (pi != SIZE_MAX) {
    std::memcpy(s_pvbo.ptr + pv, s_batch.data(), vbytes);
    std::memcpy(s_pibo.ptr + pi, s_indices.data(), ibytes);
    glBindVertexArray(s_vao_persistent);
    first = (GLint)(pv / sizeof(BatchVertex));
    ioff = pi;
  } else if (!ring || vbytes > kVboBytes || ibytes > kIboBytes) {
    glBufferData(GL_ARRAY_BUFFER, vbytes, s_batch.data(), GL_STREAM_DRAW);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER, ibytes, s_indices.data(), GL_STREAM_DRAW);
    s_vbo_pos = kVboBytes; // no longer the rings: reallocate next time
    s_ibo_pos = kIboBytes;
  } else {
    first = (GLint)(RingUpload(GL_ARRAY_BUFFER, s_vbo_pos, kVboBytes, s_batch.data(), vbytes) /
                    sizeof(BatchVertex));
    ioff = RingUpload(GL_ELEMENT_ARRAY_BUFFER, s_ibo_pos, kIboBytes, s_indices.data(), ibytes);
  }

  static const bool s_gxtrace1 = std::getenv("NWII_GXTRACE") != nullptr;
  if (s_gxtrace1) {
    static int dn = 0;
    if (dn++ < 5)
      printf("[GXTRACE] flush #%d verts=%zu\n", dn, s_batch.size());
  }
  glDrawElementsBaseVertex(GL_TRIANGLES, (GLsizei)ni, GL_UNSIGNED_INT, (void *)ioff, first);
  if (frame_log_active()) {
    // Depth right after this batch at a HUD spot and the centre.
    namespace efb = nwii::runtime::efb;
    GLint drawfb = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &drawfb);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, drawfb);
    GLfloat d0 = -1, d1 = -1;
    glReadPixels((int)(0.2f * efb::width), efb::height - 1 - (int)(0.08f * efb::height), 1, 1,
                 GL_DEPTH_COMPONENT, GL_FLOAT, &d0);
    glReadPixels(efb::width / 2, efb::height / 2, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d1);
    printf("           [FLUSH] verts=%zu depth hud=%.6f centre=%.6f fb=%d\n", s_batch.size(), d0,
           d1, drawfb);
  }
  static const bool s_gxtrace2 = std::getenv("NWII_GXTRACE") != nullptr;
  if (s_gxtrace2) {
    static int en = 0;
    if (en++ < 8) {
      GLint prog = 0, linked = 0;
      glGetIntegerv(GL_CURRENT_PROGRAM, &prog);
      if (prog)
        glGetProgramiv(prog, GL_LINK_STATUS, &linked);
      GLenum err = glGetError();
      printf("[GXTRACE] draw: prog=%d linked=%d glErr=0x%x verts=%zu\n", prog,
             linked, err, s_batch.size());
      if (prog && !linked) {
        char log[512] = {0};
        GLsizei n = 0;
        glGetProgramInfoLog(prog, 511, &n, log);
        printf("[GXTRACE] link log: %s\n", log);
      }
    }
  }
  s_batch.clear();
  s_indices.clear();
}

bool g_game_anamorphic = false; // game renders 16:9 squeezed into 4:3 (main.cpp)
// Current projection (perspective? anamorphic?) and this frame's draws per kind.
static bool s_proj_persp = false, s_proj_anamorphic = false;
static uint64_t s_ana_draws = 0, s_std_draws = 0;
} // namespace nwii::runtime::gx
uint32_t g_hitch_compiles = 0, g_hitch_texdecodes = 0; // NWII_HITCHLOG (gx.cpp)
namespace nwii::runtime::gx {

// Widescreen state: what the current draw wants, and what the bound
// projection uniform was built with.
static float s_ws_compress = 1.0f; // x scale of the projection, 1 = none
static float s_ws_applied = 1.0f;

// GX viewport (XF 0x101A-0x101F: x/y/z scale, x/y/z origin + 342) as a
// native-unit rect; full 640x480 when the game never set one.
static void GetViewport(float &left, float &top, float &w, float &h) {
  float v[6];
  std::memcpy(v, &g_state.xf[0x01A], sizeof(v));
  w = v[0] * 2.0f;
  h = -v[1] * 2.0f;
  left = v[3] - 342.0f - v[0];
  top = v[4] - 342.0f + v[1];
  if (!(w > 0.0f && h > 0.0f)) { left = 0; top = 0; w = 640; h = 480; }
}

static void ApplyViewport() {
  float left, top, w, h;
  GetViewport(left, top, w, h);
  const auto r = nwii::runtime::efb::to_host(left, top, w, h);
  glViewport(r.x, r.y, r.w, r.h);
  // Depth range (GXSetViewport near/far): window z = zorigin + zscale *
  // z_ndc with z_ndc in [-1, 0], in 24-bit units, so near = (origin -
  // scale) / 2^24-1 and far = origin / 2^24-1. Games squeeze HUD models
  // into the near end so the world never covers them.
  float zs, zo;
  std::memcpy(&zs, &g_state.xf[0x01C], 4);
  std::memcpy(&zo, &g_state.xf[0x01F], 4);
  constexpr float kMax = 16777215.0f;
  float n = (zo - zs) / kMax, f = zo / kMax;
  if (!(zs > 0.0f) || !(zo > 0.0f)) { n = 0.0f; f = 1.0f; } // never set
  static int logged = 0;
  if ((n > 0.001f || f < 0.999f) && logged < 4) {
    ++logged;
    std::printf("[VPZ] depth range %.4f..%.4f (zscale=%g zorigin=%g)\n", n, f, zs, zo);
  }
  glDepthRange(std::clamp(n, 0.0f, 1.0f), std::clamp(f, 0.0f, 1.0f));
  // GX clamps depth to the viewport range instead of clipping at the far
  // plane (Dolphin uses depth clamp too): Spyro's full-screen depth-clear
  // quad before the HUD and its 2D overlays sit beyond far. NWII_DEPTHCLAMP=0
  // turns it off for comparison.
  static const bool clamp_on = [] {
    const char *e = std::getenv("NWII_DEPTHCLAMP");
    return !(e && e[0] == '0');
  }();
  if (clamp_on) glEnable(GL_DEPTH_CLAMP);
}

// Widescreen applies to draws in a full-width viewport (the main pass; a
// render-to-texture pass such as Spyro's 128x128 shadow viewport is left
// alone so its texture comes out as the game expects). 2D draws that span
// the whole width -- fades, full-screen overlays -- are not compressed, so
// they still cover the widened screen.
static float WidescreenWanted(const GXCommand &cmd) {
  const float wide = nwii::runtime::efb::wide;
  if (!nwii::runtime::efb::widescreen || !g_state.projSet)
    return 1.0f;
  float left, top, w, h;
  GetViewport(left, top, w, h);
  if (w < 600.0f)
    return 1.0f;
  if (g_state.projType == 0) {
    // The game's own "Widescreen 16:9" option renders anamorphically: its
    // perspective x scale is 0.663 of the 4:3 one (y/x ratio ~1.95 instead of
    // ~1.29). Compressing again gave a doubly widened view.
    // Menu and dialog panels are flat 3D UI (all at z = -220, screen-locked):
    // they belong with the 2D text, which is compressed, or the two do not
    // line up.
    float zmin = 1e9f, zmax = -1e9f;
    for (const auto &v : cmd.vertices) {
      zmin = std::min(zmin, v.pos[2]);
      zmax = std::max(zmax, v.pos[2]);
    }
    if (zmax - zmin < 0.01f && zmin > -300.0f && zmax < 0.0f) return 1.0f / wide;
    // The scene, with the game's option on: already 16:9 wide, so only what
    // lies beyond 16:9.
    const float sx = g_state.projection[0], sy = g_state.projection[2];
    if (sx > 0.0f && sy / sx > 1.5f) return (4.0f / 3.0f) / wide;
    // Option off: the same picture, its 0.663 at 16:9 rather than a flat 3/4.
    return nwii::runtime::efb::kSceneWiden / wide;
  }
  const float a = g_state.projection[0], b = g_state.projection[1];
  float mn = 1e9f, mx = -1e9f;
  for (const auto &v : cmd.vertices) {
    const float x = a * v.pos[0] + b;
    mn = std::min(mn, x);
    mx = std::max(mx, x);
  }
  return mn <= -0.95f && mx >= 0.95f ? 1.0f : 1.0f / wide;
}

static void ApplyProjection(GLint uProj) {
  if (uProj < 0)
    return;
  const float *sp = g_state.projection;
  float p[16] = {0};

  if (!g_state.projSet) {
    p[0] = 1;
    p[5] = 1;
    p[10] = 1;
    p[15] = 1;
  } else if (g_state.projType == 0) {
    p[0] = sp[0];
    p[4] = 0;
    p[8] = sp[1];
    p[12] = 0;
    p[1] = 0;
    p[5] = sp[2];
    p[9] = sp[3];
    p[13] = 0;
    p[2] = 0;
    p[6] = 0;
    p[10] = 2.0f * sp[4] - 1.0f;
    p[14] = 2.0f * sp[5];
    p[3] = 0;
    p[7] = 0;
    p[11] = -1.0f;
    p[15] = 0;
  } else {
    p[0] = sp[0];
    p[4] = 0;
    p[8] = 0;
    p[12] = sp[1];
    p[1] = 0;
    p[5] = sp[2];
    p[9] = 0;
    p[13] = sp[3];
    p[2] = 0;
    p[6] = 0;
    // GX clip z is in [-1, 0]; GL wants [-1, 1] (as the perspective case).
    p[10] = 2.0f * sp[4];
    p[14] = 2.0f * sp[5] + 1.0f;
    p[3] = 0;
    p[7] = 0;
    p[11] = 0;
    p[15] = 1.0f;
  }
  // Widescreen: the EFB is 4/3 wider, so compress clip-space x by 3/4. For
  // 3D this widens the field of view; for 2D it keeps HUD elements at their
  // 4:3 shape, centred. (Which draws get it: see WidescreenWanted.)
  if (s_ws_compress != 1.0f && g_state.projSet) {
    p[0] *= s_ws_compress;
    p[8] *= s_ws_compress;
    p[12] *= s_ws_compress;
  }
  s_ws_applied = s_ws_compress;
  static const bool s_vtxtrace_p = std::getenv("NWII_VTXTRACE") != nullptr;
  if (s_vtxtrace_p) {
    static int pn = 0;
    if (pn++ < 2)
      printf("[PROJ] type=%u set=%d raw=[%.5f %.2f %.5f %.2f %.2f %.2f]\n",
             g_state.projType, (int)g_state.projSet, sp[0], sp[1], sp[2], sp[3],
             sp[4], sp[5]);
  }
  std::memcpy(s_proj, p, sizeof(s_proj));
  glUniformMatrix4fv(uProj, 1, GL_FALSE, p);
}

static bool CurrentIsPerspective() {
  static const bool flatz = std::getenv("NWII_FLATZ") != nullptr;
  if (flatz)
    return false;
  return g_state.projSet && g_state.projType == 0;
}

// GX Z mode (BP 0x40 = PE_ZMODE): bit 0 compare enable, bits 1-3 function,
// bit 4 update (depth write) enable. Translucent effects (mist, glows,
// sparkles) are drawn with updates off; writing their whole quad into the
// depth buffer anyway made later water/fog layers vanish inside each quad --
// the "square particles". With compare disabled the hardware doesn't update
// Z either.
static void ApplyZMode() {
  // NWII_ZMODE=0: old behaviour (always test LEQUAL and write) for comparison.
  static const bool zmode = [] {
    const char *e = std::getenv("NWII_ZMODE");
    return !(e && e[0] == '0');
  }();
  // 2D draws follow the game's Z mode too: Spyro clears depth before the
  // HUD with a full-screen ortho quad (compare ALWAYS, write on, colour
  // off); skipping it let walls cut through the HUD models.
  // NWII_ZMODE=2: old behaviour for 2D (no depth test, write on).
  static const bool old2d = [] {
    const char *e = std::getenv("NWII_ZMODE");
    return e && e[0] == '2';
  }();
  if (old2d && !CurrentIsPerspective()) {
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    return;
  }
  if (!zmode) {
    glEnable(GL_DEPTH_TEST);
    glDepthFunc(GL_LEQUAL);
    glDepthMask(GL_TRUE);
    return;
  }
  const uint32_t zm = g_state.bp[0x40];
  static const GLenum funcs[8] = {GL_NEVER, GL_LESS, GL_EQUAL, GL_LEQUAL,
                                  GL_GREATER, GL_NOTEQUAL, GL_GEQUAL, GL_ALWAYS};
  // Compare off still honours the update bit (Spyro lays down terrain/sky
  // depth with 0x18: no test, write on). GL_DEPTH_TEST off would also stop
  // writes, so "no compare" is ALWAYS instead.
  glEnable(GL_DEPTH_TEST);
  glDepthFunc((zm & 1) ? funcs[(zm >> 1) & 7] : GL_ALWAYS);
  glDepthMask((zm >> 4) & 1 ? GL_TRUE : GL_FALSE);
}

// Apply the GX blend state (BP 0x41 = PE_CMODE0). Without it every draw is
// opaque, so alpha fades (the WARNING "press" blink), transparent UI and the
// dialogue window never composite. The src/dst factor tables follow the
// documented GX->GL factor order (src uses DST_COLOR at index 2/3, dst uses
// SRC_COLOR).
static void ApplyBlend() {
  uint32_t cm = g_state.bp[0x41];
  bool enable = cm & 1;
  bool subtract = (cm >> 11) & 1;
  int dstF = (cm >> 5) & 7;
  int srcF = (cm >> 8) & 7;
  // Colour/alpha update (bits 3/4) are write masks. Ignoring them turns an
  // alpha-only pass into an opaque quad: Spyro's logo scene writes its fade
  // into destination alpha with a full-screen quad and colour writes off,
  // which painted the whole frame grey. Alpha only exists in RGBA6_Z24.
  const bool efb_has_alpha = (g_state.bp[0x43] & 7) == 1;
  const bool color_update = (cm >> 3) & 1;
  const bool alpha_update = ((cm >> 4) & 1) && efb_has_alpha;
  glColorMask(color_update, color_update, color_update, alpha_update);
  if (!enable) { glDisable(GL_BLEND); return; }
  // Without EFB alpha, destination alpha reads as 1 on hardware; the GL
  // alpha channel is stale there, so resolve those factors to constants.
  if (!efb_has_alpha) {
    if (srcF == 6) srcF = 1; else if (srcF == 7) srcF = 0;
    if (dstF == 6) dstF = 1; else if (dstF == 7) dstF = 0;
  }
  static const GLenum srcMap[8] = {
      GL_ZERO, GL_ONE, GL_DST_COLOR, GL_ONE_MINUS_DST_COLOR,
      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
  static const GLenum dstMap[8] = {
      GL_ZERO, GL_ONE, GL_SRC_COLOR, GL_ONE_MINUS_SRC_COLOR,
      GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA, GL_DST_ALPHA, GL_ONE_MINUS_DST_ALPHA};
  glEnable(GL_BLEND);
  if (subtract) {
    glBlendEquation(GL_FUNC_REVERSE_SUBTRACT);
    glBlendFunc(GL_ONE, GL_ONE);
  } else {
    glBlendEquation(GL_FUNC_ADD);
    glBlendFunc(srcMap[srcF], dstMap[dstF]);
  }
}

// Backface culling from BP 0x00 (GEN_MODE), bits 14-15: 0 none, 1 cull back,
// 2 cull front, 3 cull all.
//
// The subtlety is which way round GL is told about it. Our projection negates
// Y, so a triangle that is counter-clockwise on the console arrives clockwise
// here. Rather than fight that per draw, front-facing is declared clockwise and
// the cull target left at GL's default back face; culling the front is then a
// matter of declaring counter-clockwise instead. Switching glCullFace while
// leaving the default GL_CCW winding removes the opposite half of the scene --
// that is what took out foliage and text on the first attempt.
//
// NWII_NOCULL=1 disables culling entirely.
static void ApplyCullMode() {
  static const bool no_cull = std::getenv("NWII_NOCULL") != nullptr;
  const uint8_t m = g_state.cullMode;
  if (no_cull || m == 0) {
    glDisable(GL_CULL_FACE);
    return;
  }
  glEnable(GL_CULL_FACE);
  glCullFace(m == 3 ? GL_FRONT_AND_BACK : GL_BACK);
  glFrontFace(m == 2 ? GL_CCW : GL_CW);
}

// Texgen configuration of the current draw (XF 0x1040+i, 0x1012, 0x1050+i),
// decoded once per draw by PrepareTexGens instead of once per vertex.
struct TexGenPlan {
  int n;          // texgens in use (<= 8)
  bool dual;      // XF 0x1012 bit 0: post-transform stage on
  struct Gen {
    int type, src, from;
    bool stq, abc1, post_norm;
    const float *post; // post matrix, or null when out of range
  } g[8];
  int8_t nonzero[64]; // per matrix row index: -1 unknown, 0/1
};
static TexGenPlan s_tgp;

static void PrepareTexGens() {
  s_tgp.n = std::min<int>(g_state.numTexGens, 8);
  s_tgp.dual = g_state.xf[0x012] & 1;
  for (int i = 0; i < s_tgp.n; ++i) {
    const uint32_t tg = g_state.xf[0x040 + i];
    auto &g = s_tgp.g[i];
    g.type = (tg >> 4) & 7;
    g.src = (tg >> 7) & 0x1F;
    g.from = (tg >> 12) & 7;
    g.stq = (tg >> 1) & 1;
    g.abc1 = (tg >> 2) & 1;
    const uint32_t pt = g_state.xf[0x050 + i];
    g.post_norm = pt & 0x100;
    g.post = (pt & 0x3F) * 4 + 12 <= 256 ? &g_state.postMatrices[(pt & 0x3F) * 4] : nullptr;
  }
  std::memset(s_tgp.nonzero, -1, sizeof(s_tgp.nonzero));
}

static void ComputeTexGens(const VertexData &vtx, const float col[4], float tc[8][3]) {
  // GX texgens (XF 0x1040+i, one per texcoord the TEV stages read): each
  // takes an input row -- model-space position or normal, a vertex colour,
  // or one of the vertex UV sets -- through its own matrix (any XF matrix
  // index, 60 = identity) to (s, t[, q]). The shader divides by q per pixel.
  // Effects like flame, smoke and sparkles use texcoords 1+ with normal or
  // position sources; sampling them all from UV set 0 gave square sprites.
  // Uses the per-draw plan from PrepareTexGens.
  {
    const int n = s_tgp.n;
    const float u0 = vtx.has_tex[0] ? vtx.tex[0][0] : 0.0f;
    const float v0 = vtx.has_tex[0] ? vtx.tex[0][1] : 0.0f;
    for (int i = 0; i < 8; ++i) {
      tc[i][0] = u0;
      tc[i][1] = v0;
      tc[i][2] = 1.0f;
    }
    for (int i = 0; i < n; ++i) {
      const auto &g = s_tgp.g[i];
      const int type = g.type, src = g.src;
      const bool stq = g.stq, abc1 = g.abc1;
      if (type == 1) { // emboss: offset copy of another texcoord (approximated)
        if (g.from < i) std::memcpy(tc[i], tc[g.from], sizeof(tc[i]));
        continue;
      }
      float in[3] = {0, 0, 1};
      bool uv_src = false;
      if (type >= 2) { // colour-to-texcoord: s, t = colour0 R, G
        in[0] = col[0]; in[1] = col[1];
      } else if (src == 0) {
        std::memcpy(in, vtx.raw_pos, sizeof(in));
      } else if (src == 1 || src == 3 || src == 4) { // normal (binormals ~ normal)
        std::memcpy(in, vtx.raw_norm, sizeof(in));
      } else if (src == 2) {
        in[0] = col[0]; in[1] = col[1]; in[2] = col[2];
      } else if (src >= 5 && src <= 12) {
        const int k = src - 5;
        in[0] = vtx.has_tex[k] ? vtx.tex[k][0] : 0.0f;
        in[1] = vtx.has_tex[k] ? vtx.tex[k][1] : 0.0f;
        uv_src = true;
      }
      if (uv_src || !abc1) in[2] = 1.0f; // AB11 form (UV inputs are always AB11)
      float s = in[0], t = in[1], q = 1.0f;
      const int ti = vtx.texMtxIdx[i];
      if (type == 0 && ti != 60 && ti * 4 + 12 <= 256) {
        const float *m = &g_state.posMatrices[ti * 4];
        // UV sets keep the old guards: only real texture matrices (30+), and
        // an all-zero matrix (never loaded) passes the UVs through.
        int8_t &nz = s_tgp.nonzero[ti];
        if (nz < 0) nz = (m[0] || m[1] || m[2] || m[3] || m[4] || m[5] || m[6] || m[7]) ? 1 : 0;
        if (nz && (!uv_src || ti >= 30)) {
          s = m[0] * in[0] + m[1] * in[1] + m[2] * in[2] + m[3];
          t = m[4] * in[0] + m[5] * in[1] + m[6] * in[2] + m[7];
          if (stq) q = m[8] * in[0] + m[9] * in[1] + m[10] * in[2] + m[11];
        }
      }
      // Dual texgen (XF 0x1012 bit 0): optionally normalise, then the
      // per-coordinate post matrix (XF 0x1050+i, bank 0x500). Env maps use
      // it to map normals from [-1, 1] into texture space; without it a
      // face-on normal sampled the corner texel (dark gold HUD digits).
      if (type == 0 && s_tgp.dual) {
        float v[3] = {s, t, q};
        if (g.post_norm) {
          const float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
          if (len > 0.0f) { v[0] /= len; v[1] /= len; v[2] /= len; }
        }
        if (const float *p = g.post) {
          s = p[0] * v[0] + p[1] * v[1] + p[2] * v[2] + p[3];
          t = p[4] * v[0] + p[5] * v[1] + p[6] * v[2] + p[7];
          q = p[8] * v[0] + p[9] * v[1] + p[10] * v[2] + p[11];
        }
        if (!stq) q = 1.0f; // only STQ texgens are projected
      }
      // q == 0 on hardware halves and clamps s,t (Dolphin's observation).
      if (type == 0 && q == 0.0f) {
        s = std::clamp(s * 0.5f, -1.0f, 1.0f);
        t = std::clamp(t * 0.5f, -1.0f, 1.0f);
        q = 1.0f;
      }
      tc[i][0] = s;
      tc[i][1] = t;
      tc[i][2] = q;
    }
  }
}

// Transforms one GX vertex into its batch form (colour, normal, texgens,
// position). Done once per source vertex: strips and fans reuse each vertex
// in up to three triangles, and redoing this per use was the top of the
// render thread's profile.
static void BuildVertex(const VertexData &vtx, BatchVertex &bv) {
  bv = BatchVertex{};
  if (vtx.has_color) {
    bv.r = vtx.color[0];
    bv.g = vtx.color[1];
    bv.b = vtx.color[2];
    bv.a = vtx.color[3];
  } else {
    bv.r = bv.g = bv.b = bv.a = 1.f;
  }
  if (vtx.has_norm && vtx.pre_xf) {
    bv.nx = vtx.norm[0]; bv.ny = vtx.norm[1]; bv.nz = vtx.norm[2];
  } else if (vtx.has_norm) {
    // Normal matrix for a position matrix index: 3 * (idx & 31), three rows
    // of three (XF normal matrix bank layout).
    int nb = 3 * (vtx.posMtxIdx & 31);
    const float *nm = g_state.normalMatrices;
    if (nb + 9 <= 96) {
      bv.nx = vtx.norm[0] * nm[nb + 0] + vtx.norm[1] * nm[nb + 1] +
              vtx.norm[2] * nm[nb + 2];
      bv.ny = vtx.norm[0] * nm[nb + 3] + vtx.norm[1] * nm[nb + 4] +
              vtx.norm[2] * nm[nb + 5];
      bv.nz = vtx.norm[0] * nm[nb + 6] + vtx.norm[1] * nm[nb + 7] +
              vtx.norm[2] * nm[nb + 8];
    } else {
      bv.nx = vtx.norm[0]; bv.ny = vtx.norm[1]; bv.nz = vtx.norm[2];
    }
  }
  ComputeTexGens(vtx, &bv.r, bv.tc);

  float x = vtx.pos[0], y = vtx.pos[1], z = vtx.pos[2];
  int mtx_base = vtx.posMtxIdx * 4;
  if (vtx.pre_xf) {
    // Already transformed at decode with the per-draw matrix snapshot.
    bv.x = x;
    bv.y = y;
    bv.z = z;
  } else if (vtx.has_pos && mtx_base + 12 <= 256) {
    const float *mm = g_state.posMatrices;
    bv.x = x * mm[mtx_base + 0] + y * mm[mtx_base + 1] + z * mm[mtx_base + 2] +
           mm[mtx_base + 3];
    bv.y = x * mm[mtx_base + 4] + y * mm[mtx_base + 5] + z * mm[mtx_base + 6] +
           mm[mtx_base + 7];
    bv.z = x * mm[mtx_base + 8] + y * mm[mtx_base + 9] + z * mm[mtx_base + 10] +
           mm[mtx_base + 11];
  } else {
    bv.x = x;
    bv.y = y;
    bv.z = z;
  }

  // 2D draws keep their z: the ortho projection maps it like GX and depth
  // clamp keeps out-of-range values. Flattening it to 0 put Spyro's
  // depth-clear quad (z = far) at the near plane, hiding the HUD.
  // NWII_ZMODE=2 restores the old flat-z, no-depth 2D path.
  static const bool old2d_z = [] {
    const char *e = std::getenv("NWII_ZMODE");
    return e && e[0] == '2';
  }();
  if (old2d_z && !CurrentIsPerspective())
    bv.z = 0.0f;
  static const bool s_vtxtrace_v = std::getenv("NWII_VTXTRACE") != nullptr;
  if (s_vtxtrace_v) {
    static int vn = 0;
    if (vn++ < 14)
      printf("[VTX] raw=(%.1f,%.1f,%.1f) view=(%.1f,%.1f,%.1f) uv=(%.3f,%.3f) "
             "tex0=%ux%u fmt=%u\n",
             vtx.pos[0], vtx.pos[1], vtx.pos[2], bv.x, bv.y, bv.z, bv.tc[0][0], bv.tc[0][1],
             g_state.texStages[0].width, g_state.texStages[0].height,
             g_state.texStages[0].format);
  }
}

static bool s_batch_open = false;
// Draws since the last clearing EFB->XFB copy (see the XFB copy path).
static uint64_t s_draws_since_clear = 1;
static uint8_t s_batch_prim = 0;

static int s_compiles_this_frame = 0;
// Skipping draws whose shader wasn't compiled yet (to spread compiles over
// frames) dropped e.g. the fade overlay for a frame -- single bright frames
// that flickered through every fade-in after loading. Compile on demand
// instead (a one-time hitch per new shader); NWII_SHADER_BUDGET=<n> restores
// the old per-frame cap for comparison.
static const int kMaxCompilesPerFrame = [] {
  const char *e = std::getenv("NWII_SHADER_BUDGET");
  return e ? std::max(1, std::atoi(e)) : 1 << 30;
}();
// Set by SetupDrawState when the draw's shader isn't ready: DrawPrimitive then
// skips emitting its vertices instead of drawing them with the wrong shader.
static bool s_skip_draw = false;

static void SetupDrawState(const GXCommand &cmd);
static void EmitPrimitive(const GXCommand &cmd);

static void DrawPrimitive(const GXCommand &cmd) {
  const auto &v = cmd.vertices;
  const size_t n = v.size();
  if (n == 0)
    return;

  static const bool s_drawlog = std::getenv("NWII_DRAWLOG") != nullptr;
  if (s_drawlog) {
    static int dn = 0;
    if (dn < 40) {

      printf("[DRAW %02d] prim=0x%02X verts=%zu hash=%llx tex0=%dx%d fmt=%d "
             "map=%d "
             "| bp00=%06X F3=%06X C0=%06X C1=%06X C2=%06X C3=%06X 28=%06X "
             "cp50=%08X\n",
             dn, cmd.prim_type, n,
             (unsigned long long)g_state.GetShaderHash(cmd.prim_type),
             g_state.texStages[0].width, g_state.texStages[0].height,
             g_state.texStages[0].format, (int)g_state.tevStages[0].texMap,
             g_state.bp[0x00], g_state.bp[0xF3], g_state.bp[0xC0],
             g_state.bp[0xC1], g_state.bp[0xC2], g_state.bp[0xC3],
             g_state.bp[0x28], g_state.cp[0x50]);

      uint32_t th = 2166136261u, nz = 0;
      uint32_t off = g_state.texStages[0].tlut_offset;
      for (uint32_t i = 0; i < 512 && off + i < sizeof(g_state.tlutMem); ++i) {
        uint8_t b = g_state.tlutMem[off + i];
        th = (th ^ b) * 16777619u;
        if (b)
          ++nz;
      }

      uint32_t dh = 2166136261u, dnz = 0;
      uint32_t base = g_state.texStages[0].base_addr;
      if (g_ctx_ptr) {
        for (uint32_t i = 0; i < 2048; ++i) {
          uint8_t b = g_ctx_ptr->mmu.read8(base + i);
          dh = (dh ^ b) * 16777619u;
          if (b)
            ++dnz;
        }
      }
      printf("          tlut off=%u hash=%08X nonzero=%u/512 src=%08X | "
             "texdata base=%08X hash=%08X nonzero=%u/2048\n",
             off, th, nz, g_state.tlutSrcAddr, base, dh, dnz);
      ++dn;
    }
  }

  if (fade_log_active())
    printf("[FADE] f=%llu prim=%02X n=%zu z=%.0f tex0=%08X mat0=%08X mat1=%08X "
           "amb0=%08X tevreg E0=%06X E2=%06X cmode0=%06X\n",
           (unsigned long long)g_stat_frames, cmd.prim_type, n, v[0].pos[2],
           g_state.texStages[0].base_addr, (uint32_t)g_state.xf[0x00C],
           (uint32_t)g_state.xf[0x00D], (uint32_t)g_state.xf[0x00A],
           g_state.bp[0xE0], g_state.bp[0xE2], g_state.bp[0x41]);

  if (frame_log_active()) {
    float vpf[6];
    std::memcpy(vpf, &g_state.xf[0x01A], sizeof(vpf));
    printf("[FVP] w=%.1f h=%.1f left=%.1f top=%.1f\n", vpf[0] * 2, -vpf[1] * 2,
           vpf[3] - 342 - vpf[0], vpf[4] - 342 + vpf[1]);
  }
  if (frame_log_active()) {
    static int fn = 0;
    const auto &a = v[0];
    float mn[3] = {a.pos[0], a.pos[1], a.pos[2]}, mx[3] = {mn[0], mn[1], mn[2]};
    for (const auto &q : v)
      for (int k = 0; k < 3; ++k) {
        mn[k] = std::min(mn[k], q.pos[k]);
        mx[k] = std::max(mx[k], q.pos[k]);
      }
    printf("[FDRAW %03d] prim=0x%02X n=%zu prexf=%d pos[%.1f..%.1f, %.1f..%.1f, "
           "%.1f..%.1f] col=%d:%.2f,%.2f,%.2f,%.2f tex=%d:%.2f,%.2f | tex0 "
           "%08X %dx%d f%d | gen=%06X cmode0=%06X cmode1=%06X z=%06X "
           "alpha=%06X tev0c=%06X tev0a=%06X kreg=%06X proj=%d\n",
           fn++, cmd.prim_type, n, (int)a.pre_xf, mn[0], mx[0], mn[1], mx[1],
           mn[2], mx[2], (int)a.has_color, a.color[0], a.color[1], a.color[2],
           a.color[3], (int)a.has_tex[0], a.tex[0][0], a.tex[0][1],
           g_state.texStages[0].base_addr, g_state.texStages[0].width,
           g_state.texStages[0].height, g_state.texStages[0].format,
           g_state.bp[0x00], g_state.bp[0x41], g_state.bp[0x42],
           g_state.bp[0x40], g_state.bp[0xF3], g_state.bp[0xC0],
           g_state.bp[0xC1], g_state.bp[0xE0], (int)g_state.projType);
    printf("           projz p4=%g p5=%g (p0=%g p2=%g)\n", g_state.projection[4],
           g_state.projection[5], g_state.projection[0], g_state.projection[2]);
    uint32_t nz = 0, base = g_state.texStages[0].base_addr;
    if (g_ctx_ptr && base)
      for (uint32_t i = 0; i < 4096; ++i)
        nz += g_ctx_ptr->mmu.read8(0x80000000u | (base + i)) != 0;
    printf("           texdata nonzero=%u/4096 tevstages=%u texgens=%u mode0=%06X "
           "E2=%06X E3=%06X E4=%06X E5=%06X pe43=%06X\n",
           nz, ((g_state.bp[0x00] >> 10) & 15) + 1, g_state.bp[0x00] & 15, g_state.bp[0x80],
           g_state.bp[0xE2], g_state.bp[0xE3], g_state.bp[0xE4],
           g_state.bp[0xE5], g_state.bp[0x43]);
    printf("           xf amb=%08X,%08X mat=%08X,%08X chan=%08X,%08X,%08X,%08X "
           "numchans=%u order0=%06X\n",
           (uint32_t)g_state.xf[0x00A], (uint32_t)g_state.xf[0x00B],
           (uint32_t)g_state.xf[0x00C], (uint32_t)g_state.xf[0x00D],
           (uint32_t)g_state.xf[0x00E], (uint32_t)g_state.xf[0x00F],
           (uint32_t)g_state.xf[0x010], (uint32_t)g_state.xf[0x011],
           (uint32_t)g_state.xf[0x009], g_state.bp[0x28]);
    if (g_ctx_ptr && base) {
      printf("           texbytes:");
      for (uint32_t i = 0; i < 48; ++i)
        printf("%s%02X", (i % 8) ? "" : " ",
               g_ctx_ptr->mmu.read8(0x80000000u | (base + i)));
      printf("\n");
    }
    {
      const int ti = a.texMtxIdx[0];
      const float *m = (ti * 4 + 12 <= 256) ? &g_state.posMatrices[ti * 4] : nullptr;
      printf("           texgen: tex2 %08X %dx%d f%d | v0.tex %.3f,%.3f v1.tex %.3f,%.3f v2.tex %.3f,%.3f | "
             "texMtxIdx=%d m=[%.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f]\n",
             g_state.texStages[2].base_addr, g_state.texStages[2].width,
             g_state.texStages[2].height, g_state.texStages[2].format,
             v[0].tex[0][0], v[0].tex[0][1], n > 1 ? v[1].tex[0][0] : 0.f,
             n > 1 ? v[1].tex[0][1] : 0.f, n > 2 ? v[2].tex[0][0] : 0.f,
             n > 2 ? v[2].tex[0][1] : 0.f, ti, m ? m[0] : 0, m ? m[1] : 0,
             m ? m[2] : 0, m ? m[3] : 0, m ? m[4] : 0, m ? m[5] : 0, m ? m[6] : 0,
             m ? m[7] : 0, m ? m[8] : 0, m ? m[9] : 0, m ? m[10] : 0, m ? m[11] : 0);
    }
    printf("           tev2c=%06X tev2a=%06X tev3c=%06X tev3a=%06X order1=%06X ksel=%06X,%06X,%06X kc0=%06X,%06X\n",
           g_state.bp[0xC4], g_state.bp[0xC5], g_state.bp[0xC6], g_state.bp[0xC7],
           g_state.bp[0x29], g_state.bp[0xF6], g_state.bp[0xF7], g_state.bp[0xF8],
           g_state.bp[0xE0], g_state.bp[0xE1]);
    printf("           tev1c=%06X tev1a=%06X tex1 %08X %dx%d f%d tex1uv=%d:%.2f,%.2f "
           "texgen0=%08X texgen1=%08X\n",
           g_state.bp[0xC2], g_state.bp[0xC3], g_state.texStages[1].base_addr,
           g_state.texStages[1].width, g_state.texStages[1].height,
           g_state.texStages[1].format, (int)a.has_tex[1], a.tex[1][0],
           a.tex[1][1], (uint32_t)g_state.xf[0x040], (uint32_t)g_state.xf[0x041]);
    {
      float col[4] = {a.has_color ? a.color[0] : 1.f, a.has_color ? a.color[1] : 1.f,
                      a.has_color ? a.color[2] : 1.f, a.has_color ? a.color[3] : 1.f};
      float tc[8][3];
      PrepareTexGens();
      ComputeTexGens(a, col, tc);
      printf("           texgens n=%d tref=%06X,%06X,%06X,%06X:", (int)g_state.numTexGens,
             g_state.bp[0x28], g_state.bp[0x29], g_state.bp[0x2A], g_state.bp[0x2B]);
      for (int i = 0; i < (int)g_state.numTexGens && i < 8; ++i)
        printf(" [%d] tg=%05X mtx=%d tc=%.3f,%.3f,%.3f", i, (uint32_t)g_state.xf[0x040 + i],
               (int)a.texMtxIdx[i], tc[i][0], tc[i][1], tc[i][2]);
      printf(" | raw tex1=%d:%.2f,%.2f\n", (int)a.has_tex[1], a.tex[1][0], a.tex[1][1]);
      // Dual texgen: enable bit, each texgen's post matrix (index, normalise,
      // rows) -- a zero matrix here maps every vertex to its offset.
      printf("           dual=%u", (unsigned)(g_state.xf[0x012] & 1));
      for (int i = 0; i < (int)g_state.numTexGens && i < 8; ++i) {
        const uint32_t pt = g_state.xf[0x050 + i];
        printf(" | post[%d]=%03X", i, pt);
        if ((pt & 0x3F) * 4 + 12 <= 256) {
          const float *p = &g_state.postMatrices[(pt & 0x3F) * 4];
          for (int k = 0; k < 12; ++k) printf("%s%.3f", k % 4 ? " " : (k ? " / " : " "), p[k]);
        }
      }
      printf("\n");
    }
    printf("           norm=%d:%.2f,%.2f,%.2f vcd=%08X,%08X", (int)a.has_norm,
           a.norm[0], a.norm[1], a.norm[2], g_state.cp[0x50], g_state.cp[0x60]);
    for (int li = 0; li < 4; ++li) {
      const float *L = g_state.lights[li];
      uint32_t c;
      std::memcpy(&c, &L[3], 4);
      printf(" | L%d col=%08X pos=%.0f,%.0f,%.0f dir=%.2f,%.2f,%.2f", li, c,
             L[10], L[11], L[12], L[13], L[14], L[15]);
    }
    printf("\n");
  }

  const float ws = WidescreenWanted(cmd);
  if (s_batch_open && ws != s_ws_applied) {
    FlushBatch();
    s_batch_open = false; // re-run setup so the projection is rebuilt
  }
  s_ws_compress = ws;

  if (!s_batch_open || s_batch_prim != (uint8_t)cmd.prim_type) {
    FlushBatch();
    SetupDrawState(cmd);
    s_batch_open = true;
    s_batch_prim = (uint8_t)cmd.prim_type;
  }
  if (frame_log_active()) {
    GLint linked = 0;
    if (s_active_shader.id)
      glGetProgramiv(s_active_shader.id, GL_LINK_STATUS, &linked);
    printf("           prog=%u linked=%d skip=%d glErr=0x%x\n", s_active_shader.id,
           linked, (int)s_skip_draw, glGetError());
    if (linked) {
      GLint cur = 0;
      glGetIntegerv(GL_CURRENT_PROGRAM, &cur);
      float tm[16] = {0}, mc[4] = {0}, lc[4] = {0};
      if (s_active_shader.uTexMtx[0] >= 0)
        glGetUniformfv(s_active_shader.id, s_active_shader.uTexMtx[0], tm);
      if (s_active_shader.uMatColor >= 0)
        glGetUniformfv(s_active_shader.id, s_active_shader.uMatColor, mc);
      if (s_active_shader.uLightCol >= 0)
        glGetUniformfv(s_active_shader.id, s_active_shader.uLightCol, lc);
      printf("           cur=%d loc texmtx0=%d mat=%d mvp=%d | texmtx diag=%.2f,%.2f "
             "mat=%.2f,%.2f,%.2f lightcol0=%.2f batch=%zu\n",
             cur, s_active_shader.uTexMtx[0], s_active_shader.uMatColor,
             s_active_shader.uProjMtx, tm[0], tm[5], mc[0], mc[1], mc[2], lc[0],
             s_batch.size());
    }
    if (s_active_shader.id && !linked) {
      char log[2048] = {0};
      GLsizei n = 0;
      glGetProgramInfoLog(s_active_shader.id, sizeof(log) - 1, &n, log);
      printf("           linklog: %s\n", log);
    }
  }
  if (s_skip_draw)
    return; // shader for this state not ready yet
  // NWII_SKIP_CMODE=<hex> (or NWII_CTRL "skipcmode <hex>|off" at run time):
  // drop draws with that PE_CMODE0 (bisecting effects).
  static const bool skip_init = [] {
    if (const char *e = std::getenv("NWII_SKIP_CMODE"))
      g_skip_cmode = (int)std::strtoul(e, nullptr, 16);
    return true;
  }();
  (void)skip_init;
  const int skip_cmode = g_skip_cmode.load(std::memory_order_relaxed);
  if (skip_cmode >= 0 && (int)g_state.bp[0x41] == skip_cmode)
    return;
  EmitPrimitive(cmd);
  if (frame_log_active()) {
    // Flush now and sample the EFB: did this draw write anything?
    FlushBatch();
    // NWII_DIFF_CMODE=<hex>: for draws with that PE_CMODE0, report how many
    // EFB pixels changed and their bounding box (finding thin streaks).
    static const int diff_cmode = [] {
      const char *e = std::getenv("NWII_DIFF_CMODE");
      return e ? (int)std::strtoul(e, nullptr, 16) : -1;
    }();
    if (diff_cmode >= 0) {
      namespace efb = nwii::runtime::efb;
      static std::vector<unsigned char> prev;
      std::vector<unsigned char> cur((size_t)efb::width * efb::height * 4);
      efb::resolve();
      glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::read_fbo());
      glReadPixels(0, 0, efb::width, efb::height, GL_RGBA, GL_UNSIGNED_BYTE, cur.data());
      glBindFramebuffer(GL_FRAMEBUFFER, efb::draw_fbo);
      if ((int)g_state.bp[0x41] == diff_cmode && prev.size() == cur.size()) {
        int n = 0, x0 = 1 << 30, y0 = 1 << 30, x1 = -1, y1 = -1;
        for (int y = 0; y < efb::height; ++y)
          for (int x = 0; x < efb::width; ++x) {
            const size_t i = ((size_t)y * efb::width + x) * 4;
            if (std::abs(cur[i] - prev[i]) + std::abs(cur[i + 1] - prev[i + 1]) +
                    std::abs(cur[i + 2] - prev[i + 2]) > 6) {
              ++n; x0 = std::min(x0, x); x1 = std::max(x1, x);
              y0 = std::min(y0, y); y1 = std::max(y1, y);
            }
          }
        if (n) printf("[DIFF] changed=%d bbox=%d,%d..%d,%d\n", n, x0, y0, x1, y1);
      }
      prev.swap(cur);
    }
    GLint fbo = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    unsigned char px[3][4] = {};
    int pts[3][2] = {{320, 240}, {100, 100}, {540, 380}};
    // NWII_CTRL "probe <x> <y>": the third sample is that native screen pixel
    // (top-left origin) -- which draw paints a given spot.
    if (g_probe_x >= 0) {
      pts[2][0] = g_probe_x;
      pts[2][1] = (int)((nwii::runtime::efb::height - 1) / nwii::runtime::efb::sy) - g_probe_y;
    }
    nwii::runtime::efb::resolve();
    glBindFramebuffer(GL_READ_FRAMEBUFFER, nwii::runtime::efb::read_fbo());
    for (int i = 0; i < 3; ++i)
      glReadPixels((int)(pts[i][0] * nwii::runtime::efb::sx),
                   (int)(pts[i][1] * nwii::runtime::efb::sy), 1, 1, GL_RGBA,
                   GL_UNSIGNED_BYTE, px[i]);
    glBindFramebuffer(GL_FRAMEBUFFER, nwii::runtime::efb::draw_fbo);
    GLint vp[4];
    glGetIntegerv(GL_VIEWPORT, vp);
    // NWII_TEXDUMP: also dump the textures bound to units 0/1 (raw RGBA).
    static const bool texdump = std::getenv("NWII_TEXDUMP") != nullptr;
    for (int unit = 0; texdump && unit < 2; ++unit) {
      const auto &st = g_state.texStages[unit];
      GLint tex = 0, w = 0, h = 0;
      glActiveTexture(GL_TEXTURE0 + unit);
      glGetIntegerv(GL_TEXTURE_BINDING_2D, &tex);
      if (tex) {
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &w);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &h);
      }
      if (w > 0 && h > 0 && w <= 1024 && h <= 1024) {
        std::vector<unsigned char> buf((size_t)w * h * 4);
        glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, buf.data());
        char name[96];
        std::snprintf(name, sizeof(name), "texdump_%08X_%dx%d_f%d.rgba",
                      st.base_addr, w, h, st.format);
        if (FILE *f = std::fopen(name, "wb")) {
          std::fwrite(buf.data(), 1, buf.size(), f);
          std::fclose(f);
        }
      }
    }
    {
      GLint dfunc = 0; GLboolean dmask = 0; GLfloat drange[2] = {0, 0};
      glGetIntegerv(GL_DEPTH_FUNC, &dfunc);
      glGetBooleanv(GL_DEPTH_WRITEMASK, &dmask);
      glGetFloatv(GL_DEPTH_RANGE, drange);
      printf("           depth: test=%d func=0x%X write=%d range=%.3f..%.3f clamp=%d batch_open=%d\n",
             (int)glIsEnabled(GL_DEPTH_TEST), dfunc, (int)dmask, drange[0], drange[1],
             (int)glIsEnabled(GL_DEPTH_CLAMP), (int)s_batch_open);
      if (s_active_shader.uProjMtx >= 0) {
        GLfloat m[16];
        glGetUniformfv(s_active_shader.id, s_active_shader.uProjMtx, m);
        printf("           mvp z-row: %g %g %g %g | w-row: %g %g %g %g\n", m[2], m[6], m[10],
               m[14], m[3], m[7], m[11], m[15]);
      }
    }
    printf("           after: fbo=%d mask=%d%d%d%d vp=%d,%d,%d,%d px=%d,%d,%d | "
           "%d,%d,%d | %d,%d,%d\n",
           fbo, mask[0], mask[1], mask[2], mask[3], vp[0], vp[1], vp[2], vp[3],
           px[0][0], px[0][1], px[0][2], px[1][0], px[1][1], px[1][2], px[2][0],
           px[2][1], px[2][2]);
  }
}

static void SetupDrawState(const GXCommand &cmd) {
  static const bool use_shader = (std::getenv("NWII_NOSHADER") == nullptr);
  if (!use_shader)
    return;

  uint64_t hash = g_state.GetShaderHash(cmd.prim_type);

  if (!g_stat_hash0)
    g_stat_hash0 = hash;
  g_stat_shaders = (uint64_t)s_shader_cache.size();
  s_skip_draw = false;
  auto it = s_shader_cache.find(hash);
  if (it != s_shader_cache.end()) {
    s_active_shader = it->second;
  } else if (s_compiles_this_frame >= kMaxCompilesPerFrame) {
    // Shader not compiled yet and out of budget this frame: skip the draw
    // rather than render this geometry with whatever shader is bound —
    // wrong TEV/texture/uniforms is the "каша". The cache fills over the
    // next few frames, so the geometry appears once its shader is ready.
    s_skip_draw = true;
    return;
  } else {
    ++s_compiles_this_frame;
    ++g_hitch_compiles;
    auto src = GenerateTEVShader(g_state, cmd.prim_type);
    static const bool s_shdump1 = std::getenv("NWII_SHADERDUMP") != nullptr;
    if (s_shdump1) {
      static int sn = 0;
      if (sn++ < 2)
        printf("[SHADER %d] ==== FS ====\n%s\n============\n", sn,
               src.fragment_source.c_str());
    }
    GLShader sh = {};
    sh.uProjMtx = -1;
    for (int i = 0; i < 8; i++) {
      sh.uTex[i] = -1;
      sh.uTexMtx[i] = -1;
    }
    sh.uTevColor = sh.uTevKColor = -1;
    sh.id = CompileShaderCached(hash, src.vertex_source.c_str(),
                                src.fragment_source.c_str());
    static const bool s_shdump2 = std::getenv("NWII_SHADERDUMP") != nullptr;
    if (s_shdump2) {
      static int vn2 = 0;
      if (vn2++ < 1) {
        printf("==== VS ====\n%s\n", src.vertex_source.c_str());
        GLint ok = 0;
        glGetProgramiv(sh.id, GL_LINK_STATUS, &ok);
        printf("link=%d\n", ok);
        char log[1024] = {0};
        GLsizei n = 0;
        glGetProgramInfoLog(sh.id, 1023, &n, log);
        if (n)
          printf("proglog: %s\n", log);
        GLint na = 0;
        glGetProgramiv(sh.id, GL_ACTIVE_UNIFORMS, &na);
        printf("active uniforms=%d:", na);
        for (GLint i = 0; i < na; i++) {
          char nm[64];
          GLsizei len;
          GLint sz;
          GLenum ty;
          glGetActiveUniform(sh.id, i, 63, &len, &sz, &ty, nm);
          printf(" %s", nm);
        }
        printf("\n");
      }
    }
    for (int i = 0; i < 8; i++) {
      sh.uTex[i] =
          glGetUniformLocation(sh.id, ("uTex" + std::to_string(i)).c_str());
      sh.uTexMtx[i] = glGetUniformLocation(
          sh.id, ("uTexMtx[" + std::to_string(i) + "]").c_str());
    }
    sh.uTevKColor = glGetUniformLocation(sh.id, "uTevKColor[0]");
    if (sh.uTevKColor < 0)
      sh.uTevKColor = glGetUniformLocation(sh.id, "uTevKColor");
    sh.uTevColor = glGetUniformLocation(sh.id, "uTevColor[0]");
    if (sh.uTevColor < 0)
      sh.uTevColor = glGetUniformLocation(sh.id, "uTevColor");
    static const bool s_gxtrace3 = std::getenv("NWII_GXTRACE") != nullptr;
    if (s_gxtrace3) {
      static int un = 0;
      if (un++ < 3)
        printf("[GXTRACE] uniforms: mvp=%d tevColor=%d tevKColor=%d\n",
               sh.uProjMtx, sh.uTevColor, sh.uTevKColor);
    }
    sh.uProjMtx = glGetUniformLocation(sh.id, "mvp");
    sh.uMatColor = glGetUniformLocation(sh.id, "uMatColor[0]");
    sh.uAmbColor = glGetUniformLocation(sh.id, "uAmbColor[0]");
    sh.uChanCtrl = glGetUniformLocation(sh.id, "uChanCtrl[0]");
    sh.uLightCol = glGetUniformLocation(sh.id, "uLightCol[0]");
    sh.uLightPos = glGetUniformLocation(sh.id, "uLightPos[0]");
    sh.uLightDir = glGetUniformLocation(sh.id, "uLightDir[0]");
    sh.uLightCosAtt = glGetUniformLocation(sh.id, "uLightCosAtt[0]");
    sh.uLightDistAtt = glGetUniformLocation(sh.id, "uLightDistAtt[0]");

    s_shader_cache[hash] = sh;
    s_active_shader = sh;
    if (gx_trace())
      printf("[GXTRACE] Compiled new shader! Hash: %llx\n",
             (unsigned long long)hash);
  }

  glUseProgram(s_active_shader.id);
  ApplyProjection(s_active_shader.uProjMtx);
  ApplyViewport();
  ApplyZMode();
  ApplyBlend();
  ApplyCullMode();

  for (int i = 0; i < 8; ++i) {
    if (g_state.texStages[i].base_addr != 0 && s_active_shader.uTex[i] >= 0) {
      glUniform1i(s_active_shader.uTex[i], i);
    }
  }

  // XF channel state changes every frame (material fades, light motion), so
  // it is uploaded per draw run rather than baked into the shader.
  {
    auto unpack = [](uint32_t c, float *o) {
      o[0] = ((c >> 24) & 0xFF) / 255.f;
      o[1] = ((c >> 16) & 0xFF) / 255.f;
      o[2] = ((c >> 8) & 0xFF) / 255.f;
      o[3] = (c & 0xFF) / 255.f;
    };
    float mat[8], amb[8];
    unpack(g_state.xf[0x00C], mat);
    unpack(g_state.xf[0x00D], mat + 4);
    unpack(g_state.xf[0x00A], amb);
    unpack(g_state.xf[0x00B], amb + 4);
    if (s_active_shader.uMatColor >= 0)
      glUniform4fv(s_active_shader.uMatColor, 2, mat);
    if (s_active_shader.uAmbColor >= 0)
      glUniform4fv(s_active_shader.uAmbColor, 2, amb);
    if (s_active_shader.uChanCtrl >= 0) {
      GLint cc[4] = {(GLint)g_state.xf[0x00E], (GLint)g_state.xf[0x00F],
                     (GLint)g_state.xf[0x010], (GLint)g_state.xf[0x011]};
      glUniform1iv(s_active_shader.uChanCtrl, 4, cc);
    }
    float lcol[32], lpos[32], ldir[32], lcos[32], ldist[32];
    for (int i = 0; i < 8; i++) {
      const float *L = g_state.lights[i];
      uint32_t c;
      std::memcpy(&c, &L[3], 4);
      unpack(c, lcol + i * 4);
      lcos[i * 4 + 0] = L[4]; lcos[i * 4 + 1] = L[5]; lcos[i * 4 + 2] = L[6];
      lcos[i * 4 + 3] = 0.f;
      ldist[i * 4 + 0] = L[7]; ldist[i * 4 + 1] = L[8]; ldist[i * 4 + 2] = L[9];
      ldist[i * 4 + 3] = 0.f;
      lpos[i * 4 + 0] = L[10]; lpos[i * 4 + 1] = L[11]; lpos[i * 4 + 2] = L[12];
      lpos[i * 4 + 3] = 0.f;
      ldir[i * 4 + 0] = L[13]; ldir[i * 4 + 1] = L[14]; ldir[i * 4 + 2] = L[15];
      ldir[i * 4 + 3] = 0.f;
    }
    if (s_active_shader.uLightCol >= 0)
      glUniform4fv(s_active_shader.uLightCol, 8, lcol);
    if (s_active_shader.uLightPos >= 0)
      glUniform4fv(s_active_shader.uLightPos, 8, lpos);
    if (s_active_shader.uLightDir >= 0)
      glUniform4fv(s_active_shader.uLightDir, 8, ldir);
    if (s_active_shader.uLightCosAtt >= 0)
      glUniform4fv(s_active_shader.uLightCosAtt, 8, lcos);
    if (s_active_shader.uLightDistAtt >= 0)
      glUniform4fv(s_active_shader.uLightDistAtt, 8, ldist);
  }

  float kcolor[16] = {0};
  float color[16] = {0};

  for (int i = 0; i < 8; i++) {
    if (s_active_shader.uTexMtx[i] >= 0) {
      float mat[16] = {0};
      mat[0] = mat[5] = mat[10] = mat[15] = 1.0f;
      glUniformMatrix4fv(s_active_shader.uTexMtx[i], 1, GL_FALSE, mat);
    }
  }

  auto decode11 = [](uint32_t val, int shift) -> float {
    int v = (val >> shift) & 0x7FF;
    if (v & 0x400)
      v |= ~0x7FF;
    return (float)v / 255.0f;
  };

  for (int i = 0; i < 4; i++) {
    uint32_t ra = g_state.bp[0xE0 + i * 2];
    uint32_t bg = g_state.bp[0xE1 + i * 2];
    if (((ra >> 23) & 1) == 0) {
      color[i * 4 + 0] = decode11(ra, 0);
      color[i * 4 + 3] = decode11(ra, 12);
    } else {
      kcolor[i * 4 + 0] = decode11(ra, 0);
      kcolor[i * 4 + 3] = decode11(ra, 12);
    }
    if (((bg >> 23) & 1) == 0) {
      color[i * 4 + 2] = decode11(bg, 0);
      color[i * 4 + 1] = decode11(bg, 12);
    } else {
      kcolor[i * 4 + 2] = decode11(bg, 0);
      kcolor[i * 4 + 1] = decode11(bg, 12);
    }
  }

  static const bool s_tevtrace = std::getenv("NWII_TEVTRACE") != nullptr;
  if (s_tevtrace) {
    static int tn = 0;
    if (tn++ < 4)
      printf("[TEV] bp E0=%06X E1=%06X E2=%06X E3=%06X | "
             "color0=(%.2f,%.2f,%.2f,%.2f) color1=(%.2f,%.2f,%.2f,%.2f)\n",
             g_state.bp[0xE0], g_state.bp[0xE1], g_state.bp[0xE2],
             g_state.bp[0xE3], color[0], color[1], color[2], color[3], color[4],
             color[5], color[6], color[7]);
  }
  if (s_active_shader.uTevKColor >= 0)
    glUniform4fv(s_active_shader.uTevKColor, 4, kcolor);
  if (s_active_shader.uTevColor >= 0)
    glUniform4fv(s_active_shader.uTevColor, 4, color);
}

static void EmitPrimitive(const GXCommand &cmd) {
  const auto &v = cmd.vertices;
  const size_t n = v.size();
  static std::vector<BatchVertex> built;
  built.resize(n);
  PrepareTexGens();
  for (size_t i = 0; i < n; ++i)
    BuildVertex(v[i], built[i]);
  const uint32_t base = (uint32_t)s_batch.size();
  s_batch.insert(s_batch.end(), built.begin(), built.end());
  auto emit = [base](size_t i) { s_indices.push_back(base + (uint32_t)i); };

  switch (cmd.prim_type) {
  case 0x80:
    for (size_t i = 0; i + 3 < n; i += 4) {
      emit(i);
      emit(i + 1);
      emit(i + 2);
      emit(i + 2);
      emit(i + 3);
      emit(i);
    }
    break;
  case 0x90:
    for (size_t i = 0; i + 2 < n; i += 3) {
      emit(i);
      emit(i + 1);
      emit(i + 2);
    }
    break;
  case 0x98:
    for (size_t i = 2; i < n; i++) {
      if (i & 1) {
        emit(i - 1);
        emit(i - 2);
        emit(i);
      } else {
        emit(i - 2);
        emit(i - 1);
        emit(i);
      }
    }
    break;
  case 0xA0:
    for (size_t i = 2; i < n; i++) {
      emit(0);
      emit(i - 1);
      emit(i);
    }
    break;
  }
}

// GX sampler state for texture map `map` (TX_SETMODE0: BP 0x80-0x83 for maps
// 0-3, 0xA0-0xA3 for 4-7): wrap S/T in bits 0-1/2-3 (clamp, repeat,
// mirror), mag filter bit 4, min filter bits 5-7 (bit 2 of that = linear).
// Applied to the bound texture per draw, since the same texture can be
// sampled with different modes. The cache used to force GL_NEAREST, which
// turned magnified glow/particle textures into hard blocks. No mip levels
// are uploaded, so mip variants fall back to their base filter.
// EFB-copy textures whose mip chain is out of date. Building it after every
// copy cost a full-size downsample chain per copy and frame (at 3x, the copies
// are up to 1920x1440) although the game almost always samples them unmipped.
static std::unordered_set<GLuint> s_efb_mips_stale;

// Widescreen: a camera-facing quad whose texture coordinates run a little past
// the texture (the logo and copyright screens, -0.24..1.24 on a repeating
// texture) showed the repeats in the extra width, just off-screen at 4:3. Such
// a quad gets a clamped s coordinate; tiling surfaces are not camera-facing.
static bool s_clamp_s = false;
static bool WantClampS(const GXCommand &cmd) {
  if (!nwii::runtime::efb::widescreen || !g_state.projSet || g_state.projType != 0 ||
      cmd.vertices.size() != 4)
    return false;
  float zmin = 1e9f, zmax = -1e9f, umin = 1e9f, umax = -1e9f;
  for (const auto &v : cmd.vertices) {
    zmin = std::min(zmin, v.pos[2]);
    zmax = std::max(zmax, v.pos[2]);
    umin = std::min(umin, v.tex[0][0]);
    umax = std::max(umax, v.tex[0][0]);
  }
  return zmax - zmin < 0.01f && (umin < 0.0f || umax > 1.0f) && umin > -1.0f && umax < 2.0f;
}

static void ApplySamplerMode(int map) {
  const uint32_t m0 = g_state.bp[(map < 4 ? 0x80 : 0xA0) + (map & 3)];
  static const GLint wrap[4] = {GL_CLAMP_TO_EDGE, GL_REPEAT, GL_MIRRORED_REPEAT,
                                GL_REPEAT};
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, s_clamp_s ? GL_CLAMP_TO_EDGE : wrap[m0 & 3]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap[(m0 >> 2) & 3]);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER,
                  (m0 & 0x10) ? GL_LINEAR : GL_NEAREST);
  // Linear minification gets trilinear mipmapping (the cache builds a mip
  // chain) plus anisotropic filtering: at higher internal resolutions
  // distant textures otherwise shimmer. Nearest stays nearest.
  // EFB copies (render targets: Spyro's projected shadow, reflections) follow
  // the game's filter exactly: with generated mips, the clamped shadow map's
  // edge texels picked up the blurred silhouette at grazing angles and smeared
  // it as a thin streak ahead of Spyro while he charged.
  bool linear = m0 & 0x80;
  const uint32_t base = g_state.texStages[map].base_addr;
  if (base) {
    if (const GLuint efb_tex = nwii::runtime::hle::TextureCache::get().find_efb_texture(base)) {
      const int gx_min = (m0 >> 5) & 7; // 0/4 = no mips; 1,2,5,6 = mipmapped
      const bool mips = gx_min == 1 || gx_min == 2 || gx_min == 5 || gx_min == 6;
      if (!mips) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, linear ? GL_LINEAR : GL_NEAREST);
        glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */, 1.0f);
        glGetError(); // anisotropy may be unsupported
        return;
      }
      if (s_efb_mips_stale.erase(efb_tex)) glGenerateMipmap(GL_TEXTURE_2D);
    }
  }
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER,
                  linear ? GL_LINEAR_MIPMAP_LINEAR : GL_NEAREST);
  static const float aniso = [] {
    float max_aniso = 1.0f;
    glGetFloatv(0x84FF /* GL_MAX_TEXTURE_MAX_ANISOTROPY */, &max_aniso);
    glGetError(); // unsupported query: stay at 1
    return std::min((float)nwii::runtime::Config::get().anisotropy, std::max(1.0f, max_aniso));
  }();
  if (aniso > 1.0f)
    glTexParameterf(GL_TEXTURE_2D, 0x84FE /* GL_TEXTURE_MAX_ANISOTROPY */,
                    linear ? aniso : 1.0f);
}

// The conversion ApplyEfbCopyFormat's CPU path does (2x2-native box filter,
// shadow border reset, intensity, 4-bit quantisation), as GPU passes with the
// same integer arithmetic. The CPU path reads the copy back with glReadPixels,
// which waits for the GPU to finish everything queued so far -- Spyro's shadow
// is such a copy every frame, so the render thread and the GPU took turns
// instead of overlapping (worst on slow GPUs). Takes the raw copy from the
// bound read framebuffer and the texture bound to GL_TEXTURE_2D, leaves the
// result in that texture (ow x oh) and restores the GL state it touches.
// NWII_EFBCONV_CPU=1 (or NWII_SHADOWSTAT, which needs the pixels) = CPU path.
static bool EfbConvertGpu(uint32_t w, uint32_t h, bool half, bool four_bit,
                          bool to_intensity, bool keep_alpha, bool efb_alpha) {
  static const bool off = std::getenv("NWII_EFBCONV_CPU") || std::getenv("NWII_SHADOWSTAT");
  static GLuint prog = 0, vao = 0, fbo = 0, src = 0, mid = 0, bg = 0;
  static GLint uMode, uSrc, uBg, uBlock, uSrcSize, uBorder, uFourBit, uIntensity, uKeepAlpha,
      uEfbAlpha;
  static bool failed = false;
  if (off || failed) return false;
  if (!prog) {
    static const char *vs = R"(#version 330 core
void main() {
  vec2 p = vec2((gl_VertexID << 1) & 2, gl_VertexID & 2);
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
})";
    // Modes: 0 box filter (uBlock source texels per output texel, partial
    // blocks at the far edges as on the CPU), 1 the most common border texel
    // (majority vote, exact plurality when there is no majority), 2 final:
    // border reset, intensity, 4-bit. Values are handled as 0-255 integers.
    static const char *fs = R"(#version 330 core
uniform sampler2D uSrc;
uniform sampler2D uBg;
uniform int uMode;
uniform ivec2 uBlock;
uniform ivec2 uSrcSize;
uniform int uBorder, uFourBit, uIntensity, uKeepAlpha, uEfbAlpha;
out vec4 o;
ivec4 fetch(ivec2 p) { return ivec4(texelFetch(uSrc, p, 0) * 255.0 + 0.5); }
ivec2 border_texel(int i) {
  if (i < 2 * uSrcSize.x) return ivec2(i >> 1, (i & 1) == 0 ? 0 : uSrcSize.y - 1);
  i -= 2 * uSrcSize.x;
  return ivec2((i & 1) == 0 ? 0 : uSrcSize.x - 1, 1 + (i >> 1));
}
void main() {
  ivec2 p = ivec2(gl_FragCoord.xy);
  if (uMode == 0) {
    ivec4 sum = ivec4(0);
    int n = 0;
    ivec2 b = p * uBlock;
    for (int y = b.y; y < min(uSrcSize.y, b.y + uBlock.y); ++y)
      for (int x = b.x; x < min(uSrcSize.x, b.x + uBlock.x); ++x) {
        sum += fetch(ivec2(x, y));
        ++n;
      }
    o = vec4(n > 0 ? sum / n : ivec4(0)) / 255.0;
    return;
  }
  if (uMode == 1) {
    int n = 2 * uSrcSize.x + 2 * (uSrcSize.y - 2);
    ivec4 cand = ivec4(0);
    int c = 0;
    for (int i = 0; i < n; ++i) {
      ivec4 v = fetch(border_texel(i));
      if (c == 0) { cand = v; c = 1; }
      else if (v == cand) ++c;
      else --c;
    }
    int cnt = 0;
    for (int i = 0; i < n; ++i)
      if (fetch(border_texel(i)) == cand) ++cnt;
    if (2 * cnt <= n) {
      int best = 0;
      for (int i = 0; i < n; ++i) {
        ivec4 v = fetch(border_texel(i));
        int k = 0;
        for (int j = 0; j < n; ++j)
          if (fetch(border_texel(j)) == v) ++k;
        if (k > best) { best = k; cand = v; }
      }
    }
    o = vec4(cand) / 255.0;
    return;
  }
  ivec4 v = fetch(p);
  if (uBorder != 0 &&
      (p.x == 0 || p.y == 0 || p.x == uSrcSize.x - 1 || p.y == uSrcSize.y - 1))
    v = ivec4(texelFetch(uBg, ivec2(0), 0) * 255.0 + 0.5);
  if (uIntensity != 0) {
    float yv = 16.0 + 0.257 * float(v.r) + 0.504 * float(v.g) + 0.098 * float(v.b);
    int l = int(min(yv, 255.0));
    v = ivec4(l, l, l, uKeepAlpha != 0 ? (uEfbAlpha != 0 ? v.a : 255) : l);
  }
  if (uFourBit != 0) v = (v >> 4) * 17;
  o = vec4(v) / 255.0;
})";
    prog = CompileShader(vs, fs);
    GLint linked = 0;
    glGetProgramiv(prog, GL_LINK_STATUS, &linked);
    if (!linked) {
      std::printf("[EFBCONV] shader failed to link, using the CPU path\n");
      glDeleteProgram(prog);
      prog = 0;
      failed = true;
      return false;
    }
    uMode = glGetUniformLocation(prog, "uMode");
    uSrc = glGetUniformLocation(prog, "uSrc");
    uBg = glGetUniformLocation(prog, "uBg");
    uBlock = glGetUniformLocation(prog, "uBlock");
    uSrcSize = glGetUniformLocation(prog, "uSrcSize");
    uBorder = glGetUniformLocation(prog, "uBorder");
    uFourBit = glGetUniformLocation(prog, "uFourBit");
    uIntensity = glGetUniformLocation(prog, "uIntensity");
    uKeepAlpha = glGetUniformLocation(prog, "uKeepAlpha");
    uEfbAlpha = glGetUniformLocation(prog, "uEfbAlpha");
    glGenVertexArrays(1, &vao);
    glGenFramebuffers(1, &fbo);
    GLuint t[3];
    glGenTextures(3, t);
    src = t[0], mid = t[1], bg = t[2];
  }

  // Saved state (the renderer re-applies most of it per batch anyway).
  GLint old_prog, old_vao, old_read, old_draw, old_active, old_tex0, old_tex1, vp[4];
  GLboolean cmask[4];
  glGetIntegerv(GL_CURRENT_PROGRAM, &old_prog);
  glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &old_vao);
  glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &old_read);
  glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &old_draw);
  glGetIntegerv(GL_ACTIVE_TEXTURE, &old_active);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex0); // the copy's texture
  const GLuint dst = (GLuint)old_tex0;
  glGetIntegerv(GL_VIEWPORT, vp);
  glGetBooleanv(GL_COLOR_WRITEMASK, cmask);
  const GLenum caps[] = {GL_BLEND, GL_DEPTH_TEST, GL_CULL_FACE, GL_SCISSOR_TEST, GL_STENCIL_TEST};
  GLboolean was_on[5];
  for (int i = 0; i < 5; ++i) {
    was_on[i] = glIsEnabled(caps[i]);
    glDisable(caps[i]);
  }
  glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
  glActiveTexture(GL_TEXTURE1);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex1);
  glActiveTexture(GL_TEXTURE0);
  glGetIntegerv(GL_TEXTURE_BINDING_2D, &old_tex0);

  auto alloc = [](GLuint tex, GLsizei tw, GLsizei th) {
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint cw = 0, ch = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &cw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &ch);
    if (cw != tw || ch != th) {
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, tw, th, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    }
  };
  auto pass = [&](int mode, GLuint in, GLsizei in_w, GLsizei in_h, GLuint out, GLsizei out_w,
                  GLsizei out_h) {
    glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, out, 0);
    glViewport(0, 0, out_w, out_h);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, in);
    glUniform1i(uMode, mode);
    glUniform2i(uSrcSize, in_w, in_h);
    glDrawArrays(GL_TRIANGLES, 0, 3);
  };

  // The raw copy -> src (the destination texture is resized below).
  alloc(src, w, h);
  glCopyTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 0, 0, w, h);

  glUseProgram(prog);
  glBindVertexArray(vao);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, fbo);
  glUniform1i(uSrc, 0);
  glUniform1i(uBg, 1);
  glUniform1i(uFourBit, four_bit);
  glUniform1i(uIntensity, to_intensity);
  glUniform1i(uKeepAlpha, keep_alpha);
  glUniform1i(uEfbAlpha, efb_alpha);

  GLuint cur = src;
  uint32_t ow = w, oh = h;
  if (half) {
    // One output texel per 2x2 native pixels = (2*sx) x (2*sy) host pixels.
    const uint32_t bx = std::max(1, (int)std::lround(2.0f * nwii::runtime::efb::sx));
    const uint32_t by = std::max(1, (int)std::lround(2.0f * nwii::runtime::efb::sy));
    ow = std::max(1u, w / bx);
    oh = std::max(1u, h / by);
    alloc(mid, ow, oh);
    glUniform2i(uBlock, bx, by);
    pass(0, src, w, h, mid, ow, oh);
    cur = mid;
  }
  static const bool fix_border = [] {
    const char *e = std::getenv("NWII_SHADOW_BORDER");
    return !(e && e[0] == '0');
  }();
  const bool border = four_bit && fix_border && ow >= 4 && oh >= 4;
  glUniform1i(uBorder, border);
  if (border) {
    alloc(bg, 1, 1);
    pass(1, cur, ow, oh, bg, 1, 1);
    glActiveTexture(GL_TEXTURE1);
    glBindTexture(GL_TEXTURE_2D, bg);
    glActiveTexture(GL_TEXTURE0);
  }
  glBindTexture(GL_TEXTURE_2D, dst);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
  pass(2, cur, ow, oh, dst, ow, oh);
  glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, 0, 0);

  glUseProgram(old_prog);
  glBindVertexArray(old_vao);
  glBindFramebuffer(GL_READ_FRAMEBUFFER, old_read);
  glBindFramebuffer(GL_DRAW_FRAMEBUFFER, old_draw);
  glViewport(vp[0], vp[1], vp[2], vp[3]);
  glColorMask(cmask[0], cmask[1], cmask[2], cmask[3]);
  for (int i = 0; i < 5; ++i)
    if (was_on[i]) glEnable(caps[i]);
  glActiveTexture(GL_TEXTURE1);
  glBindTexture(GL_TEXTURE_2D, old_tex1);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, old_tex0);
  glActiveTexture(old_active);
  glBindTexture(GL_TEXTURE_2D, dst); // the caller continues on the copy's texture
  return true;
}

// EFB copy to texture (BP 0x52): reshape the raw RGBA copy bound to
// GL_TEXTURE_2D into what the target format reads back as when the game
// samples it. The raw format sits in bits 3-6 (real = raw/2 + (raw&1)*8),
// bit 15 selects YUV intensity. Channel selections are a texture swizzle;
// the intensity variants need luma and are converted (EfbConvertGpu). Without
// EFB alpha (RGB8 pixel formats) alpha reads as 1. Spyro's ground shadow is
// an R4 copy used as dst*(1-a); copied raw, its stale alpha blacked out the
// whole floor.
static void ApplyEfbCopyFormat(uint32_t val, uint32_t x, uint32_t y,
                               uint32_t w, uint32_t h) {
  const uint32_t raw = (val >> 3) & 0xF;
  const uint32_t fmt = raw / 2 + (raw & 1) * 8;
  const bool intensity = (val >> 15) & 1;
  const bool efb_alpha = (g_state.bp[0x43] & 7) == 1;
  const GLint A = efb_alpha ? GL_ALPHA : GL_ONE;

  // fmt: 0 R4, 1 R8, 2 RA4, 3 RA8, 4 RGB565, 5 RGB5A3, 6 RGBA8, 7 A8,
  //      8 R8, 9 G8, 10 B8, 11 RG8, 12 GB8.
  //
  // Half-scale copies (bit 9: 2x2 box filter) and 4-bit formats are rebuilt
  // at the console's resolution and precision (EfbConvertGpu; CPU fallback).
  // Spyro's projected shadow is an R4 half-scale copy: kept at full
  // (upscaled) resolution and 8 bits, the 1-2 pixel tail survived and was
  // cast as a long thin streak while he charged; on hardware it averages away.
  const bool half = (val >> 9) & 1;
  const bool four_bit = fmt == 0 || fmt == 2;
  const bool to_intensity = intensity && (fmt <= 3 || fmt == 8);
  if (half || four_bit || to_intensity) {
    // NWII_EFBCONV_CHECK=1: convert each copy both ways, upload the CPU
    // result and report texels where the GPU result differs.
    static const bool check = std::getenv("NWII_EFBCONV_CHECK") != nullptr;
    std::vector<unsigned char> px, gpu;
    if (check) {
      px.resize((size_t)w * h * 4);
      glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
    }
    bool done = EfbConvertGpu(w, h, half, four_bit, to_intensity, fmt == 2 || fmt == 3, efb_alpha);
    GLint gw = 0, gh = 0;
    if (done && check) {
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &gw);
      glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &gh);
      gpu.resize((size_t)gw * gh * 4);
      glPixelStorei(GL_PACK_ALIGNMENT, 1);
      glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, gpu.data());
      glPixelStorei(GL_PACK_ALIGNMENT, 4);
      done = false;
    }
    if (!done) {
      if (!check) {
        px.resize((size_t)w * h * 4);
        glReadPixels(x, y, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      }
      uint32_t ow = w, oh = h;
      if (half) {
        // One output texel per 2x2 native pixels = (2*sx) x (2*sy) host pixels.
        const uint32_t bx = std::max(1, (int)std::lround(2.0f * nwii::runtime::efb::sx));
        const uint32_t by = std::max(1, (int)std::lround(2.0f * nwii::runtime::efb::sy));
        ow = std::max(1u, w / bx);
        oh = std::max(1u, h / by);
        std::vector<unsigned char> small((size_t)ow * oh * 4);
        for (uint32_t oy = 0; oy < oh; ++oy)
          for (uint32_t ox = 0; ox < ow; ++ox) {
            uint32_t sum[4] = {0, 0, 0, 0}, n = 0;
            for (uint32_t yy = oy * by; yy < std::min(h, oy * by + by); ++yy)
              for (uint32_t xx = ox * bx; xx < std::min(w, ox * bx + bx); ++xx, ++n)
                for (int c = 0; c < 4; ++c) sum[c] += px[((size_t)yy * w + xx) * 4 + c];
            for (int c = 0; c < 4; ++c)
              small[((size_t)oy * ow + ox) * 4 + c] = (unsigned char)(n ? sum[c] / n : 0);
          }
        px.swap(small);
      }
      // Projected-shadow copies (4-bit, sampled with clamp): any non-background
      // texel on the border is repeated to infinity along the projection --
      // the long thin lines from Spyro's shadow to the horizon, when his
      // stretched silhouette (charging, wings) touches the 128x128 shadow
      // viewport's edge. Reset the 1-texel border to the background (its most
      // common value). NWII_SHADOW_BORDER=0 keeps the border.
      static const bool fix_border = [] {
        const char *e = std::getenv("NWII_SHADOW_BORDER");
        return !(e && e[0] == '0');
      }();
      if (four_bit && fix_border && ow >= 4 && oh >= 4) {
        std::unordered_map<uint32_t, uint32_t> votes;
        auto texel = [&](uint32_t x2, uint32_t y2) -> uint32_t & {
          return *reinterpret_cast<uint32_t *>(&px[((size_t)y2 * ow + x2) * 4]);
        };
        for (uint32_t x2 = 0; x2 < ow; ++x2) { ++votes[texel(x2, 0)]; ++votes[texel(x2, oh - 1)]; }
        for (uint32_t y2 = 1; y2 + 1 < oh; ++y2) { ++votes[texel(0, y2)]; ++votes[texel(ow - 1, y2)]; }
        uint32_t bg = 0, best = 0;
        for (const auto &[v, c] : votes)
          if (c > best) { best = c; bg = v; }
        uint32_t fixed = 0;
        auto set = [&](uint32_t x2, uint32_t y2) {
          uint32_t &t = texel(x2, y2);
          if (t != bg) { t = bg; ++fixed; }
        };
        for (uint32_t x2 = 0; x2 < ow; ++x2) { set(x2, 0); set(x2, oh - 1); }
        for (uint32_t y2 = 1; y2 + 1 < oh; ++y2) { set(0, y2); set(ow - 1, y2); }
        static const bool stat = std::getenv("NWII_SHADOWSTAT") != nullptr;
        if (stat) {
          // Bounding box of non-background texels, every 30th copy or when the
          // border had to be reset.
          static uint32_t n = 0;
          uint32_t x0 = ow, y0 = oh, x1 = 0, y1 = 0;
          for (uint32_t y2 = 0; y2 < oh; ++y2)
            for (uint32_t x2 = 0; x2 < ow; ++x2)
              if (texel(x2, y2) != bg) {
                x0 = std::min(x0, x2); y0 = std::min(y0, y2);
                x1 = std::max(x1, x2); y1 = std::max(y1, y2);
              }
          if (fixed || (++n % 30) == 0)
            std::printf("[SHADOW] %ux%u copy fmt=%u bg=%08X bbox=%u,%u-%u,%u reset=%u\n", ow, oh,
                        fmt, bg, x0, y0, x1, y1, fixed);
        }
      }
      const bool keep_alpha = (fmt == 2 || fmt == 3);
      for (size_t i = 0; i < px.size(); i += 4) {
        if (to_intensity) {
          const float yv = 16.f + 0.257f * px[i] + 0.504f * px[i + 1] +
                           0.098f * px[i + 2];
          const unsigned char l = (unsigned char)(yv > 255.f ? 255.f : yv);
          const unsigned char a = keep_alpha ? (efb_alpha ? px[i + 3] : 255) : l;
          px[i] = px[i + 1] = px[i + 2] = l;
          px[i + 3] = a;
        }
        if (four_bit)
          for (int c = 0; c < 4; ++c) px[i + c] = (unsigned char)((px[i + c] >> 4) * 17);
      }
      if (check && !gpu.empty()) {
        static uint32_t copies = 0, bad_copies = 0;
        size_t diff = 0;
        if ((uint32_t)gw != ow || (uint32_t)gh != oh)
          diff = px.size() / 4;
        else
          for (size_t i = 0; i < px.size(); i += 4)
            if (std::memcmp(&px[i], &gpu[i], 4) != 0) ++diff;
        ++copies;
        if (diff) ++bad_copies;
        if (diff || copies % 300 == 0)
          std::printf("[EFBCONV] %ux%u -> %ux%u (gpu %dx%d) fmt=%u half=%d int=%d: %zu texels "
                      "differ; %u of %u copies differed\n",
                      w, h, ow, oh, gw, gh, fmt, half, to_intensity, diff, bad_copies, copies);
      }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, ow, oh, 0, GL_RGBA, GL_UNSIGNED_BYTE,
                   px.data());
    }
    if (to_intensity) {
      const GLint identity[4] = {GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA};
      glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, identity);
      return;
    }
    // Otherwise fall through to the channel swizzle for the format.
  }

  GLint sw[4] = {GL_RED, GL_GREEN, GL_BLUE, A};
  auto mono = [&](GLint c, GLint a) { sw[0] = sw[1] = sw[2] = c; sw[3] = a; };
  switch (fmt) {
  case 0: case 1: case 8: mono(GL_RED, GL_RED); break;
  case 2: case 3: mono(GL_RED, A); break;
  case 7: mono(A, A); break;
  case 9: mono(GL_GREEN, GL_GREEN); break;
  case 10: mono(GL_BLUE, GL_BLUE); break;
  case 11: mono(GL_RED, GL_GREEN); break;
  case 12: mono(GL_GREEN, GL_BLUE); break;
  default: break; // RGB565 / RGB5A3 / RGBA8: as copied
  }
  glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, sw);
}

class RendererGL : public IRenderer {
public:
  RendererGL() = default;
  ~RendererGL() override = default;

  void Initialize(void *window) override { m_window = window; }

  void Render(const std::vector<GXCommand> &commands) override {
    // A command batch is NOT a frame: with the CPU running ahead, one game
    // frame often spans two batches. Clearing here (depth always, colour when
    // a copy-with-clear had been parsed) wiped half-drawn frames -- flicker
    // and broken occlusion. Copies now clear in stream order, exactly where
    // the game's copy command is. NWII_OLDCLEAR=1 restores the old clears.
    static const bool old_clear = std::getenv("NWII_OLDCLEAR") != nullptr;
    if (old_clear) {
      glDepthMask(GL_TRUE);
      glClear(GL_DEPTH_BUFFER_BIT);
    }
    s_compiles_this_frame = 0; // reset the per-frame shader compile budget
    if (old_clear && g_state.pe_clear_pending) {
      g_state.pe_clear_pending = false;
      unsigned char cc[4];
      ::GX_GetClearColor(cc);
      glClearColor(cc[0] / 255.f, cc[1] / 255.f, cc[2] / 255.f, cc[3] / 255.f);
      glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    }

    // Only commands that change GPU-side state end the open batch. Matrix
    // loads (XF 0x000-0x5FF: position/texture, normal and post matrices) are
    // applied on the CPU when vertices are built, and CP writes (vertex
    // formats, arrays) only matter to the parser -- flushing on them turned
    // nearly every GX draw into its own GL draw (~16k per hub frame).
    // NWII_BATCH_ALL=1: flush on every non-draw command (old behaviour).
    static const bool batch_all = std::getenv("NWII_BATCH_ALL") != nullptr;
    // Spyro re-sends light 0 (XF 0x600), the material colour (0x100C) and
    // the matrix index (0x1018) for every object, usually unchanged, and
    // syncs with PE tokens (BP 0x45/0x47/0x48): ~3000 flushes per frame.
    // Unchanged XF values and the sync tokens keep the batch open.
    auto breaks_batch = [](const GXCommand &c) {
      if (batch_all) return true;
      if (c.type == GXCommandType::CPRegister) return false;
      if (c.type == GXCommandType::BPRegister) {
        const uint32_t r = c.reg & 0xFF;
        if (r == 0x45 || r == 0x47 || r == 0x48) return false; // PE sync tokens
        // TMEM placement (TX_SETIMAGE1/2: BP 0x8C-0x93, 0xAC-0xB3): nothing
        // in the GL renderer reads it -- textures are bound by address and
        // format -- yet it changes with every texture load (~70% of level 2's
        // flushes).
        if ((r >= 0x8C && r <= 0x93) || (r >= 0xAC && r <= 0xB3)) return false;
        // Pure state registers (no side effect beyond their value): an
        // unchanged write keeps the batch. Copies (0x52), TLUT/TMEM loads,
        // texture cache invalidation etc. always end it.
        const bool pure = r == 0x00 || r == 0x20 || r == 0x21 || r == 0x59 ||
                          (r >= 0x28 && r <= 0x2F) || (r >= 0x40 && r <= 0x43) ||
                          (r >= 0x80 && r <= 0x9B) || (r >= 0xA0 && r <= 0xBB) ||
                          (r >= 0xC0 && r <= 0xE7) || (r >= 0xEE && r <= 0xFD);
        return !(pure && g_state.bp[r] == c.val);
      }
      if (c.type == GXCommandType::XFRegister) {
        for (size_t i = 0; i < c.payload.size(); ++i) {
          const int a = (int)c.reg + (int)i;
          if (a < 0x600) continue;                  // matrices: used on the CPU
          if (a == 0x1018 || a == 0x1019) continue; // matrix indices: parser only
          uint32_t nv, cur;
          std::memcpy(&nv, &c.payload[i], 4);
          if (a <= 0x67F)
            std::memcpy(&cur, &g_state.lights[(a - 0x600) / 16][(a - 0x600) % 16], 4);
          else if (a >= 0x1000 && a < 0x1100)
            cur = g_state.xf[a - 0x1000];
          else
            return true;
          if (cur != nv) return true;
        }
        return false;
      }
      return true;
    };
    for (const auto &cmd : commands) {

      if (cmd.type != GXCommandType::DrawPrimitive && s_batch_open && breaks_batch(cmd)) {
        // NWII_BATCHSTAT=1: which commands end batches, per 300 Render calls.
        static const bool bstat = std::getenv("NWII_BATCHSTAT") != nullptr;
        if (bstat) {
          static std::unordered_map<uint32_t, uint32_t> why;
          static uint32_t calls = 0, flushes = 0;
          static const void *last_list = nullptr;
          ++flushes;
          ++why[((uint32_t)cmd.type << 16) | (cmd.reg & 0xFFFF)];
          if (last_list != &commands) {
            last_list = &commands;
            if (++calls == 300) {
              std::vector<std::pair<uint32_t, uint32_t>> v(why.begin(), why.end());
              std::sort(v.begin(), v.end(), [](auto &a, auto &b) { return a.second > b.second; });
              std::printf("[BATCH] %u flushes / 300 renders, top causes (type:reg):", flushes);
              for (size_t i = 0; i < v.size() && i < 10; ++i)
                std::printf(" %u:%03X=%u", v[i].first >> 16, v[i].first & 0xFFFF, v[i].second);
              std::printf("\n");
              why.clear();
              calls = flushes = 0;
            }
          }
        }
        FlushBatch();
        s_batch_open = false;
      }
      if (cmd.type == GXCommandType::XFRegister) {
        int addr = cmd.reg;
        bool touched_proj = false;
        for (size_t i = 0; i < cmd.payload.size(); ++i) {
          int current_addr = addr + (int)i;
          if (current_addr >= 0x1000 && current_addr < 0x1000 + 256) {
            std::memcpy(&g_state.xf[current_addr - 0x1000], &cmd.payload[i], 4);
          }
          if (current_addr >= 0x0000 && current_addr <= 0x00FF) {
            g_state.posMatrices[current_addr] = cmd.payload[i];
          } else if (current_addr >= 0x0400 && current_addr <= 0x045F) {
            g_state.normalMatrices[current_addr - 0x0400] = cmd.payload[i];
          } else if (current_addr >= 0x0500 && current_addr <= 0x05FF) {
            g_state.postMatrices[current_addr - 0x0500] = cmd.payload[i];
          } else if (current_addr >= 0x0600 && current_addr <= 0x067F) {
            int li = (current_addr - 0x0600) / 16;
            g_state.lights[li][(current_addr - 0x0600) % 16] = cmd.payload[i];
          } else if (current_addr >= 0x1020 && current_addr <= 0x1026) {
            g_state.projection[current_addr - 0x1020] = cmd.payload[i];
            if (current_addr == 0x1026) {
              std::memcpy(&g_state.projType, &cmd.payload[i], 4);
            }
            touched_proj = true;
          }
        }
        if (touched_proj) {
          g_state.projSet = true;
          // The game's anamorphic "Widescreen 16:9" mode (see WidescreenWanted):
          // the presenter stretches the picture to 16:9 even without our
          // widescreen option, like a widescreen TV would.
          // Only a vote: render-to-texture passes (atlas pages, shadows) load
          // their own projections, and taking the last one loaded flipped
          // the presented aspect for single frames (4:3 menus flickered).
          s_proj_persp = g_state.projType == 0 && g_state.projection[0] > 0.0f;
          if (s_proj_persp)
            s_proj_anamorphic = g_state.projection[2] / g_state.projection[0] > 1.5f;
          ApplyProjection(s_active_shader.uProjMtx);
        }
      } else if (cmd.type == GXCommandType::BPRegister) {
        ApplyBPRegister((uint8_t)cmd.reg, cmd.val);
        if (cmd.reg == 0x40) {
          if (frame_log_active()) printf("[FZMODE] %06X\n", cmd.val);
          ApplyZMode();
        }
        // EFB copy WITHOUT bit14 = copy to a texture in guest RAM. Grab the
        // just-rendered pixels into a GL texture keyed by the destination
        // address; texture lookups on that address sample it directly
        // (render-to-texture: title backgrounds, portraits, effects).
        if (cmd.reg == 0x52 && frame_log_active())
          printf("[FCOPY] val=%06X %s dest=%08X src=%u,%u size=%ux%u clear=%d\n",
                 cmd.val, (cmd.val & 0x4000) ? "XFB" : "TEX",
                 g_state.efbCopyDest, g_state.efbSrcX, g_state.efbSrcY,
                 g_state.efbW, g_state.efbH, (cmd.val & 0x800) ? 1 : 0);
        if (cmd.reg == 0x52 && !(cmd.val & 0x4000)) {
          uint32_t dest = g_state.efbCopyDest;
          uint32_t w = g_state.efbW, h = g_state.efbH;
          if (dest && w > 0 && h > 0 && w <= 1024 && h <= 1024) {
            GLuint tex =
                nwii::runtime::hle::TextureCache::get().find_efb_texture(dest);
            if (!tex)
              glGenTextures(1, &tex);
            glBindTexture(GL_TEXTURE_2D, tex);
            // Copy the source rectangle (native units, top-left origin) at
            // the EFB's host resolution, so render-to-texture effects get
            // the higher resolution too. It comes from the resolved target
            // and is flipped on the way: texture row 0 is the top row of the
            // source, as on hardware (GL's EFB is bottom-up).
            namespace efb = nwii::runtime::efb;
            const efb::Rect r = efb::to_host((float)g_state.efbSrcX,
                                             (float)g_state.efbSrcY, (float)w, (float)h);
            static GLuint copy_fbo = 0;
            if (!copy_fbo)
              glGenFramebuffers(1, &copy_fbo);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, r.w, r.h, 0, GL_RGBA,
                         GL_UNSIGNED_BYTE, nullptr);
            efb::resolve();
            glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::read_fbo());
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, copy_fbo);
            glFramebufferTexture2D(GL_DRAW_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                                   GL_TEXTURE_2D, tex, 0);
            glBlitFramebuffer(r.x, r.y, r.x + r.w, r.y + r.h, 0, r.h, r.w, 0,
                              GL_COLOR_BUFFER_BIT, GL_NEAREST);
            glBindFramebuffer(GL_READ_FRAMEBUFFER, copy_fbo);
            ApplyEfbCopyFormat(cmd.val, 0, 0, r.w, r.h);
            glBindFramebuffer(GL_FRAMEBUFFER, efb::draw_fbo);
            glBindTexture(GL_TEXTURE_2D, tex);
            // Mips only when a draw samples it mipmapped (ApplySamplerMode).
            s_efb_mips_stale.insert(tex);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            // Guest bytes the copy would occupy (at least 4 bpp; half-scale
            // copies are smaller, which only shortens the checked range).
            nwii::runtime::hle::TextureCache::get().register_efb_texture(dest,
                                                                         tex, w * h / 2);
            if (cmd.val & 0x800) {
              // The copy's clear only touches the copied rectangle.
              unsigned char cc[4];
              ::GX_GetClearColor(cc);
              glClearColor(cc[0] / 255.f, cc[1] / 255.f, cc[2] / 255.f,
                           cc[3] / 255.f);
              glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
              glDepthMask(GL_TRUE);
              glEnable(GL_SCISSOR_TEST);
              glScissor(r.x, r.y, r.w, r.h);
              glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
              glDisable(GL_SCISSOR_TEST);
              g_state.pe_clear_pending = false;
            }
          }
        }
        if (cmd.reg == 0x52 && (cmd.val & 0x4000)) {
          // EFB -> XFB: the rectangle the game displays this frame. Snapshot
          // the finished frame into the display buffer the window presents,
          // so a half-drawn next frame in the EFB never reaches the screen.
          namespace efb = nwii::runtime::efb;
          if (s_batch_open) {
            FlushBatch();
            s_batch_open = false;
          }
          if (g_state.efbW > 0 && g_state.efbH > 0) {
            efb::xfb_x = g_state.efbSrcX;
            efb::xfb_y = g_state.efbSrcY;
            efb::xfb_w = g_state.efbW;
            efb::xfb_h = g_state.efbH;
          }
          // A copy of an EFB that saw no draw since the last clearing copy is
          // just the clear colour: games use it to clear the EFB (and the back
          // XFB) before drawing. On hardware the real copy overwrites it
          // before VI scans that buffer; presenting it here flickered black
          // once frames got faster. NWII_XFB_ALL=1: snapshot every copy.
          static const bool xfb_all = std::getenv("NWII_XFB_ALL") != nullptr;
          static const bool xfb_log = std::getenv("NWII_XFBLOG") != nullptr;
          if (xfb_log)
            printf("[XFBDRAWS] dest=%08X draws_since_clear=%llu clear=%d\n", g_state.efbCopyDest,
                   (unsigned long long)s_draws_since_clear, (cmd.val & 0x800) ? 1 : 0);
          if (xfb_all || s_draws_since_clear > 0) {
            efb::snapshot(g_state.efbCopyDest);
            // Anamorphic display: the projection most of this frame's 3D
            // draws used, changing only after 10 frames in a row agree.
            if (s_ana_draws + s_std_draws > 0) {
              const bool want = s_ana_draws > s_std_draws;
              static int streak = 0;
              streak = want != g_game_anamorphic ? streak + 1 : 0;
              if (streak >= 10) {
                g_game_anamorphic = want;
                streak = 0;
                if (xfb_log) printf("[ANAMORPHIC] %d\n", want ? 1 : 0);
              }
            }
            s_ana_draws = s_std_draws = 0;
          }
          if (cmd.val & 0x800) s_draws_since_clear = 0;
          static const bool old_clear = std::getenv("NWII_OLDCLEAR") != nullptr;
          if ((cmd.val & 0x800) && !old_clear) {
            // The copy's clear, in stream order (copied rectangle only).
            const efb::Rect r = efb::to_host((float)g_state.efbSrcX, (float)g_state.efbSrcY,
                                             (float)g_state.efbW, (float)g_state.efbH);
            unsigned char cc[4];
            ::GX_GetClearColor(cc);
            glBindFramebuffer(GL_FRAMEBUFFER, efb::draw_fbo);
            glClearColor(cc[0] / 255.f, cc[1] / 255.f, cc[2] / 255.f, cc[3] / 255.f);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            glDepthMask(GL_TRUE);
            glEnable(GL_SCISSOR_TEST);
            glScissor(r.x, r.y, r.w, r.h);
            glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
            glDisable(GL_SCISSOR_TEST);
            g_state.pe_clear_pending = false;
          }
        }
      } else if (cmd.type == GXCommandType::DrawPrimitive) {
        ++s_draws_since_clear;
        if (s_proj_persp) ++(s_proj_anamorphic ? s_ana_draws : s_std_draws);
        const bool clamp_s = WantClampS(cmd);
        if (clamp_s != s_clamp_s) {
          if (s_batch_open) {
            FlushBatch();
            s_batch_open = false;
          }
          s_clamp_s = clamp_s;
        }
        if (!s_batch_open && g_ctx_ptr) {
          for (int i = 0; i < 8; ++i) {
            const auto &stage = g_state.texStages[i];
            if (stage.base_addr != 0 && stage.width > 0 && stage.height > 0) {
              // Select the unit first: a cache miss creates the texture with
              // glBindTexture on the active unit, which would otherwise be
              // unit i-1 and clobber the texture just bound there (the font
              // quads before the hub spawn sampled a stale stage-1 texture).
              glActiveTexture(GL_TEXTURE0 + i);
              GLuint tex = nwii::runtime::hle::TextureCache::get().get_texture(
                  *g_ctx_ptr, stage);
              glBindTexture(GL_TEXTURE_2D, tex);
              ApplySamplerMode(i);
            }
          }
        }
        DrawPrimitive(cmd);
      }
    }
    if (s_batch_open) {
      FlushBatch();
      s_batch_open = false;
    }
    // The GX write mask is per draw; don't let it leak into presentation.
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    if (frame_log_active()) {
      namespace efb = nwii::runtime::efb;
      efb::resolve();
      glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::read_fbo());
      std::vector<unsigned char> px((size_t)efb::width * efb::height * 4);
      glReadPixels(0, 0, efb::width, efb::height, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
      glBindFramebuffer(GL_FRAMEBUFFER, efb::draw_fbo);
      size_t nz = 0;
      for (size_t i = 0; i < px.size(); i += 4)
        nz += (px[i] | px[i + 1] | px[i + 2]) != 0;
      printf("[FEND] efb nonblack after Render: %zu/307200 cmds=%zu\n", nz,
             commands.size());
      // Depth at a few points (fractions of the EFB, top-left origin).
      glBindFramebuffer(GL_READ_FRAMEBUFFER, efb::draw_fbo);
      const float pts[4][2] = {{0.2f, 0.08f}, {0.5f, 0.08f}, {0.5f, 0.5f}, {0.9f, 0.5f}};
      for (const auto &pt : pts) {
        GLfloat d = -1.0f;
        const int x = (int)(pt[0] * efb::width), y = efb::height - 1 - (int)(pt[1] * efb::height);
        glReadPixels(x, y, 1, 1, GL_DEPTH_COMPONENT, GL_FLOAT, &d);
        printf("[FEND] depth at %.2f,%.2f = %.6f\n", pt[0], pt[1], d);
      }
      glBindFramebuffer(GL_FRAMEBUFFER, efb::draw_fbo);
    }
  }

  void Present() override {}

private:
  void *m_window = nullptr;
};

std::unique_ptr<IRenderer> CreateRendererGL() {
  return std::make_unique<RendererGL>();
}

} // namespace nwii::runtime::gx

uint64_t nwii_stat_hash0() { return nwii::runtime::gx::g_stat_hash0; }
uint64_t nwii_stat_shaders() { return nwii::runtime::gx::g_stat_shaders; }

namespace nwii::runtime::gx {}

// Presenter query (main.cpp): the game is in its anamorphic widescreen mode.
bool g_game_anamorphic_display() { return nwii::runtime::gx::g_game_anamorphic; }
