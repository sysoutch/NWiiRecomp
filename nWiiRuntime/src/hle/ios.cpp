#include "runtime/platform/platform.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"
#include <algorithm>
#include <cstring>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <vector>
#include <thread>
#include "runtime/hw/hw.h"
#include "runtime/event_scheduler.h"
#include "runtime/os_thread.h"
#include "runtime/virtual_disc.h"

using namespace nwii::runtime;

namespace nwii::runtime {
    std::string read_guest_string(CPUContext &ctx, uint32_t addr, int max_len = 256);
    bool valid_callback(uint32_t cb);

    // See poll_callbacks(): TSC cycles per NWII_POLL_US microseconds
    // (default 20), measured once over ~5 ms. 0 (no TSC, or NWII_POLL_US=0)
    // keeps the gate open: every 16th back-edge does the full check.
    uint64_t g_poll_cycles = [] {
        const char *e = std::getenv("NWII_POLL_US");
        const uint64_t us = e ? std::strtoull(e, nullptr, 10) : 20;
        if (us == 0) return uint64_t{0};
        const auto t0 = std::chrono::steady_clock::now();
        const uint64_t c0 = nwii_cycles();
        while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(5)) {}
        const uint64_t c1 = nwii_cycles();
        const double el_us = std::chrono::duration<double, std::micro>(
                                 std::chrono::steady_clock::now() - t0).count();
        return el_us > 0 ? (uint64_t)((double)(c1 - c0) / el_us * (double)us) : uint64_t{0};
    }();
}

#include "runtime/ios_device.h"
#include "runtime/ios_kernel.h"
#include "runtime/devices.h"
#include <iostream>
#include <vector>

extern "C" void run_game(nwii::runtime::CPUContext &ctx);

static uint32_t phys_to_virt(uint32_t addr) {
  if (addr == 0)
    return 0;
  if (addr >= 0x80000000 && addr < 0x94000000)
    return addr;
  if (addr >= 0x00800000 && addr < 0x04000000)
    return addr | 0x90000000;
  if (addr < 0x00800000)
    return addr | 0x80000000;
  if (addr >= 0x10000000 && addr < 0x14000000)
    return addr | 0x90000000;
  return addr;
}

std::string nwii::runtime::read_guest_string(CPUContext &ctx, uint32_t addr,
                                     int max_len) {
  std::string path;
  if (addr == 0) {
    std::cout << "[HLE IOS] read_guest_string: null address! pc=0x" << std::hex
              << ctx.pc << " lr=0x" << ctx.lr << std::dec << std::endl;
    return path;
  }
  uint32_t vaddr = phys_to_virt(addr);
  if (vaddr >= 0x80000000 && vaddr < 0x94000000) {
    for (int i = 0; i < max_len; ++i) {
      char c = ctx.mmu.read8(vaddr + i);
      if (c == '\0')
        break;
      if (c >= 32 && c <= 126)
        path += c;
    }
    if (path.empty()) {
      std::cout << "[HLE IOS] read_guest_string: string at 0x" << std::hex
                << addr << " (vaddr 0x" << vaddr
                << ") is empty (first char is 0x" << (int)ctx.mmu.read8(vaddr)
                << ") pc=0x" << ctx.pc << " lr=0x" << ctx.lr << std::dec
                << std::endl;
    }
  } else {
    std::cout << "[HLE IOS] read_guest_string: invalid translated address 0x"
              << std::hex << vaddr << " (orig: 0x" << addr << ") pc=0x"
              << ctx.pc << " lr=0x" << ctx.lr << std::dec << std::endl;
  }
  return path;
}

static constexpr int32_t IPC_EIO = -8;


static constexpr uint32_t IPC_POOL_BASE = 0x93EE0000;
static constexpr uint32_t IPC_SLOT_SIZE = 128; 
static constexpr uint32_t IPC_POOL_SLOTS = 64;
static uint32_t ipc_pool_next = 0; 




static uint32_t ios_guest_heap_ptr = 0x93E40000;
static uint32_t ios_ipc_heap_ptr = 0x93E00000;

static uint32_t ios_guest_alloc(CPUContext &ctx, uint32_t size, uint32_t align,
                                int heap_id = 0) {
  if (align < 32)
    align = 32;
  
  uint32_t &heap =
      (heap_id == 0 && size <= 128) ? ios_ipc_heap_ptr : ios_guest_heap_ptr;
  heap = (heap + align - 1) & ~(align - 1);
  uint32_t ptr = heap;
  heap += (size + align - 1) & ~(align - 1);
  for (uint32_t i = 0; i < size; ++i)
    ctx.mmu.write8(ptr + i, 0);
  return ptr;
}

static uint32_t ipc_pool_alloc(CPUContext &ctx) {
  uint32_t slot = ipc_pool_next;
  ipc_pool_next = (ipc_pool_next + 1) % IPC_POOL_SLOTS;
  uint32_t ptr = IPC_POOL_BASE + slot * IPC_SLOT_SIZE;
  
  for (uint32_t i = 0; i < IPC_SLOT_SIZE; ++i)
    ctx.mmu.write8(ptr + i, 0);
  return ptr;
}


bool nwii::runtime::valid_callback(uint32_t cb) {
  return cb != 0 && cb != 0xFFFFFFFFu && cb >= 0x80000000u && cb < 0x82000000u;
}

static std::shared_ptr<std::ifstream> g_disc_file;
static bool g_disc_is_wbfs = false;
static uint32_t g_wbfs_block_size = 0;
static std::vector<uint16_t> g_wbfs_bat;

static bool init_wbfs(const std::filesystem::path &path) {
  auto file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (!file->is_open())
    return false;

  char magic[4];
  file->read(magic, 4);
  if (magic[0] != 'W' || magic[1] != 'B' || magic[2] != 'F' ||
      magic[3] != 'S') {
    return false;
  }

  uint8_t shift;
  file->read(reinterpret_cast<char *>(&shift), 1);
  g_wbfs_block_size = 1u << (shift + 17);

  file->seekg(0, std::ios::end);
  size_t file_size = file->tellg();
  file->seekg(0x100, std::ios::beg);

  size_t num_entries = (file_size - 0x100) / 2;
  if (num_entries > 65536)
    num_entries = 65536;

  g_wbfs_bat.resize(num_entries);
  for (size_t i = 0; i < num_entries; ++i) {
    uint16_t entry;
    file->read(reinterpret_cast<char *>(&entry), 2);
    g_wbfs_bat[i] = ((entry >> 8) & 0xFF) | ((entry << 8) & 0xFF00);
  }

  g_disc_file = file;
  g_disc_is_wbfs = true;
  std::cout << "[HLE IOS] Opened WBFS: " << path
            << " block_size=" << g_wbfs_block_size << std::endl;
  return true;
}

static void init_iso(const std::filesystem::path &path) {
  g_disc_file = std::make_shared<std::ifstream>(path, std::ios::binary);
  if (g_disc_file->is_open()) {
    g_disc_is_wbfs = false;
    std::cout << "[HLE IOS] Opened ISO: " << path << std::endl;
  }
}

static std::shared_ptr<std::ifstream> get_disc_file() {
  if (!g_disc_file) {
    auto &cfg = Config::get();
    std::filesystem::path game_dir(cfg.game_dir);
    std::filesystem::path p;

    p = game_dir / "disc.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir / "game.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir.parent_path() / "game.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }
    p = game_dir.parent_path() / "disc.iso";
    if (std::filesystem::exists(p)) {
      init_iso(p);
      return g_disc_file;
    }

    if (std::filesystem::exists(game_dir)) {
      for (const auto &entry : std::filesystem::directory_iterator(game_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".wbfs") {
          if (init_wbfs(entry.path()))
            return g_disc_file;
        }
      }
    }
    if (std::filesystem::exists(game_dir.parent_path())) {
      for (const auto &entry :
           std::filesystem::directory_iterator(game_dir.parent_path())) {
        if (entry.is_regular_file() && entry.path().extension() == ".wbfs") {
          if (init_wbfs(entry.path()))
            return g_disc_file;
        }
      }
    }
  }
  return g_disc_file;
}

static void read_disc_data(uint8_t *dst, uint64_t offset, uint32_t length) {
  
  auto &vd = VirtualDisc::get();
  if (!vd.valid())
    vd.init(Config::get().game_dir);
  if (vd.valid() && vd.read(offset, length, dst))
    return;

  auto file = get_disc_file();
  if (!file || !file->is_open()) {
    std::memset(dst, 0, length);
    return;
  }

  if (!g_disc_is_wbfs) {
    file->seekg(offset);
    file->read(reinterpret_cast<char *>(dst), length);
  } else {
    uint32_t done = 0;
    while (done < length) {
      uint64_t block_idx = (offset + done) / g_wbfs_block_size;
      uint32_t block_off = (offset + done) % g_wbfs_block_size;
      uint32_t chunk =
          std::min<uint32_t>(length - done, g_wbfs_block_size - block_off);

      if (block_idx < g_wbfs_bat.size() && g_wbfs_bat[block_idx] != 0) {
        uint64_t phys =
            (uint64_t)g_wbfs_bat[block_idx] * g_wbfs_block_size + block_off;
        file->seekg(phys);
        file->read(reinterpret_cast<char *>(dst + done), chunk);
      } else {
        std::memset(dst + done, 0, chunk);
      }
      done += chunk;
    }
  }
}

namespace nwii {
namespace runtime {

struct PendingCallback {
  uint32_t callback;
  uint32_t arg1;
  uint32_t arg2;
};

static std::vector<PendingCallback> g_pending_callbacks;



} 
} 

static void di_read_internal(nwii::runtime::MMU *mmu, uint32_t inbuf, uint32_t outbuf,
                             uint32_t callback, uint32_t userdata,
                             bool is_async, uint32_t out_size = 0) {
  
  uint32_t cmd = 0, transferSize = 0, addr = 0, length = 0;
  uint64_t offset = 0;

  uint32_t ipc_ptr = phys_to_virt(inbuf);

  if (ipc_ptr >= 0x80000000 && ipc_ptr < 0x94000000) {
    uint32_t raw_cmd = mmu->read32(ipc_ptr + 0x00);
    cmd = raw_cmd >> 24; 
    if (cmd == 1) {
      cmd = 0x12; 
    } else if (cmd == 0) {
      
      if (raw_cmd == 0x20)
        cmd = 0x20;
    }

    transferSize = mmu->read32(ipc_ptr + 0x04);
    addr = mmu->read32(ipc_ptr + 0x08);

    uint32_t val10 = mmu->read32(ipc_ptr + 0x10);
    uint32_t val14 = mmu->read32(ipc_ptr + 0x14);

    if (nwii::runtime::Config::get().platform ==
        nwii::runtime::Platform::GameCube) {
      offset = ((uint64_t)val14) << 2; 
    } else {
      
      offset = (((uint64_t)(val10 & 0xFFFF) << 32) | val14) << 2;
    }

    length = mmu->read32(ipc_ptr + 0x18);
  }
  
  if (addr == 0 && outbuf != 0) {
      addr = outbuf;
  }

  std::cout << "[HLE IOS] DI_Read" << (is_async ? "Async" : "") << ": inbuf=0x"
            << std::hex << inbuf << " cmd=0x" << cmd
            << " transferSize=" << std::dec << transferSize << " addr=0x"
            << std::hex << addr << " cb=0x" << callback << " offset=0x"
            << offset << " length=" << std::dec << length << " outbuf=0x" << std::hex << outbuf << std::endl;

  bool use_defaults =
      (cmd != 0x20 && cmd != 0x12 && cmd != 0x95 && cmd != 0x71 && cmd != 0x72 && cmd != 0x96 && cmd != 0xe3) || (offset > 0x1FFFFFFFFFFULL);

  if (use_defaults) {
    std::cout << "[HLE IOS] DI_Read: Using defaults (invalid structure)"
              << std::endl;
    cmd = 0x20;
    transferSize = 2;
    offset = 0;
    length = 0x440;
  }

  uint32_t buf_ptr = phys_to_virt(addr);
  uint32_t total_length = transferSize * 2048;
  if (length > 0 && length < total_length)
    total_length = length;
  if (total_length == 0 && length > 0)
    total_length = length;
  if (total_length == 0)
    total_length = 0x440;
  
  if (out_size > 0 && total_length > out_size)
    total_length = out_size;

  std::cout << "[HLE IOS] DI_Read final: offset=0x" << std::hex
            << (uint32_t)offset << " total_length=" << std::dec << total_length
            << " buf_ptr=0x" << std::hex << buf_ptr << std::dec << std::endl;

  auto file = get_disc_file();
  bool success = false;
  uint32_t bytes_read = 0;
  bool have_media = (file && file->is_open()) || VirtualDisc::get().valid();

  if (have_media && buf_ptr != 0 && total_length > 0) {
    std::vector<uint8_t> temp(total_length);
    read_disc_data(temp.data(), (uint32_t)offset, total_length);
    bytes_read = total_length;
    for (uint32_t i = 0; i < bytes_read; i++)
      mmu->write8(buf_ptr + i, temp[i]);
    std::cout << "[DI] Read " << bytes_read << " bytes" << std::endl;
    success = true;
  } else if (buf_ptr != 0) {
    if (offset == 0) {
      const std::string& gid = Config::get().game_id;
      for (uint32_t i = 0; i < total_length; i++) mmu->write8(buf_ptr + i, 0);
      for (int i = 0; i < (int)gid.size() && i < 4; i++)
        mmu->write8(buf_ptr + i, (uint8_t)gid[i]);
      mmu->write32(buf_ptr + 0x18, 0x5D1C9EA3);
      std::cout << "[DI] Faked disc header (" << gid << ")" << std::endl;
      success = true;
      bytes_read = total_length;
    } else {
      
      std::cout << "[DI] No disc file, zeroing out read buffer of size " << total_length << std::endl;
      for (uint32_t i = 0; i < total_length; i++) mmu->write8(buf_ptr + i, 0);
      success = true;
      bytes_read = total_length;
    }
  }
}

void di_read_internal_shared(nwii::runtime::MMU* mmu, uint32_t inbuf, uint32_t outbuf,
                              uint32_t callback, uint32_t userdata, bool is_async,
                              uint32_t out_size) {
    di_read_internal(mmu, inbuf, outbuf, callback, userdata, is_async, out_size);
}

extern "C" {
void IOS_Open(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_open(ctx);
}

void IOS_OpenAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_open_async(ctx);
}

void IOS_Close(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_close(ctx);
}

void IOS_Read(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_read(ctx);
}

void IOS_Write(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_write(ctx);
}

void IOS_Ioctl(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctl(ctx);
}

void IOS_Ioctlv(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctlv(ctx);
}

void iosAlloc(CPUContext &ctx) {
  uint32_t heap_id = ctx.gpr[3];
  uint32_t size = ctx.gpr[4];
  uint32_t align = ctx.gpr[5];
  uint32_t ptr = ios_guest_alloc(ctx, size, align, heap_id);
  std::cout << "[HLE IOS] iosAlloc: heap=" << heap_id << " size=" << size
            << " align=" << align << " -> ptr=0x" << std::hex << ptr << " LR=0x" << ctx.lr << std::dec
            << std::endl;
  ctx.gpr[3] = ptr;
  ctx.pc = ctx.lr;
}


void iosFree(CPUContext &ctx) {
  uint32_t heap_id = ctx.gpr[3];
  uint32_t ptr = ctx.gpr[4];
  std::cout << "[HLE IOS] iosFree: heap=" << heap_id << " ptr=0x" << std::hex
            << ptr << " LR=0x" << ctx.lr << std::dec << std::endl;
  ctx.gpr[3] = 0; 
  ctx.pc = ctx.lr;
}

void IOS_IoctlAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctl_async(ctx);
}

void IOS_IoctlvAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_ioctlv_async(ctx);
}

void IOS_Seek(CPUContext &ctx) {
  if (Config::get().platform == Platform::GameCube) {
    ctx.gpr[3] = -1;
    ctx.pc = ctx.lr;
    return;
  }
  int32_t fd = (int32_t)ctx.gpr[3];
  int32_t offset = (int32_t)ctx.gpr[4];
  int32_t whence = (int32_t)ctx.gpr[5];

  int32_t result = IOSKernel::get().seek(ctx, fd, offset, whence);
  ctx.gpr[3] = result;
  ctx.pc = ctx.lr;
}

void IOS_CloseAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_close_async(ctx);
}

void IOS_ReadAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_read_async(ctx);
}

void IOS_WriteAsync(CPUContext &ctx) {
    nwii::runtime::platform::IPlatform::get().ios_write_async(ctx);
}

void IOS_SeekAsync(CPUContext &ctx) {
  if (Config::get().platform == Platform::GameCube) {
    ctx.gpr[3] = -1;
    ctx.pc = ctx.lr;
    return;
  }
  int32_t fd = (int32_t)ctx.gpr[3];
  int32_t offset = (int32_t)ctx.gpr[4];
  int32_t whence = (int32_t)ctx.gpr[5];
  uint32_t callback = ctx.gpr[6];
  uint32_t userdata = ctx.gpr[7];

  int32_t result = IOSKernel::get().seek(ctx, fd, offset, whence);

  if (valid_callback(callback)) {
    ctx.queue_callback(callback, result, userdata);
  }

  ctx.gpr[3] = IPC_OK;
  ctx.pc = ctx.lr;
}

} 

namespace nwii {
namespace runtime {
extern MMU *g_mmu;
}
} 

extern "C" int32_t handle_ios_ipc(CPUContext& ctx, uint32_t request_addr) {
  if (!nwii::runtime::g_mmu)
    return 0;

  

  uint32_t virt_addr = request_addr;
  if (virt_addr < 0x01800000) {
    virt_addr |= 0x80000000;
  } else if (virt_addr >= 0x10000000 && virt_addr < 0x14000000) {
    virt_addr = (virt_addr & 0x03FFFFFF) | 0x90000000;
  }

  // IOS IPC request layout (Dolphin IOS::HLE):
  
  uint32_t cmd = nwii::runtime::g_mmu->read32(virt_addr);
  int32_t result = IPC_OK;

  if (cmd == 1) { 
    
    uint32_t path_addr = nwii::runtime::g_mmu->read32(virt_addr + 0x0C);
    uint32_t mode = nwii::runtime::g_mmu->read32(virt_addr + 0x10);
    
    if (path_addr < 0x01800000)
      path_addr |= 0x80000000;
    else if (path_addr >= 0x10000000 && path_addr < 0x14000000)
      path_addr = (path_addr & 0x03FFFFFF) | 0x90000000;

    std::string path;
    for (int i = 0; i < 64; ++i) {
      char c = (char)ctx.mmu.read8(path_addr + i);
      if (c == '\0') break;
      path += c;
    }
    result = IOSKernel::get().open(ctx, path, mode);
    std::cout << "[HW IPC] Open path='" << path << "' -> fd=" << result << "\n";
  } else if (cmd == 2) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    result = IOSKernel::get().close(ctx, fd);
  } else if (cmd == 3) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t buf = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t len = ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().read(ctx, fd, buf, len);
  } else if (cmd == 4) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t buf = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t len = ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().write(ctx, fd, buf, len);
  } else if (cmd == 5) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    int32_t offset = (int32_t)ctx.mmu.read32(virt_addr + 0x0C);
    int32_t whence = (int32_t)ctx.mmu.read32(virt_addr + 0x10);
    result = IOSKernel::get().seek(ctx, fd, offset, whence);
  } else if (cmd == 6) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t ioctl_cmd = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t in_buf = ctx.mmu.read32(virt_addr + 0x10);
    uint32_t in_size = ctx.mmu.read32(virt_addr + 0x14);
    uint32_t out_buf = ctx.mmu.read32(virt_addr + 0x18);
    uint32_t out_size = ctx.mmu.read32(virt_addr + 0x1C);

    std::cout << "[HW IPC] Ioctl fd=" << fd << " cmd=0x" << std::hex << ioctl_cmd << std::dec << "\n";

    IpcRequest req{};
    req.fd = fd;
    req.req_addr = virt_addr;
    req.ioctl_cmd = ioctl_cmd;
    req.in_buf = in_buf;
    req.in_size = in_size;
    req.out_buf = out_buf;
    req.out_size = out_size;
    result = IOSKernel::get().ioctl(ctx, req);
  } else if (cmd == 7) { 
    uint32_t fd = ctx.mmu.read32(virt_addr + 0x08);
    uint32_t ioctl_cmd = ctx.mmu.read32(virt_addr + 0x0C);
    uint32_t arg_in = ctx.mmu.read32(virt_addr + 0x10);
    uint32_t arg_out = ctx.mmu.read32(virt_addr + 0x14);
    uint32_t arg_array = ctx.mmu.read32(virt_addr + 0x18);

    IpcRequest req{};
    req.cmd = cmd;
    req.fd = fd;
    req.req_addr = virt_addr;
    req.ioctl_cmd = ioctl_cmd;
    req.arg_cnt_in = arg_in;
    req.arg_cnt_out = arg_out;

    std::cout << "[HW IPC] Parsed IOCTLV fd=" << fd << " cmd=" << std::hex << ioctl_cmd << std::dec 
              << " in=" << arg_in << " out=" << arg_out << "\n";

    for (uint32_t i = 0; i < arg_in + arg_out; i++) {
        IoctlvVector vec;
        vec.addr = ctx.mmu.read32(arg_array + i * 8);
        vec.len = ctx.mmu.read32(arg_array + i * 8 + 4);
        req.ioctlv_vecs.push_back(vec);
    }

    result = IOSKernel::get().ioctlv(ctx, req);
  }

  std::cout << "[HW IPC] cmd=" << cmd << " virt=0x" << std::hex << virt_addr
            << " -> result=" << (int32_t)result << std::dec << std::endl;

  if (result != -0x70000001) { 
      nwii::runtime::g_mmu->write32(virt_addr + 4, (uint32_t)(int32_t)result);
  }
  return result;
}

namespace nwii {
namespace runtime {

void init_ipc_client(CPUContext &ctx) {
  IOSKernel::get().init();
  nwii::runtime::devices::register_all();
}

bool handle_syscall(CPUContext &ctx) {
  uint32_t syscall_id = ctx.gpr[0];
  uint32_t old_pc = ctx.pc; 

  switch (syscall_id) {
  case 0x61:
    IOS_Open(ctx);
    break;
  case 0x62:
    IOS_Close(ctx);
    break;
  case 0x63:
    IOS_Read(ctx);
    break;
  case 0x64:
    IOS_Write(ctx);
    break;
  case 0x65:
    IOS_Seek(ctx);
    break;
  case 0x66:
    IOS_Ioctl(ctx);
    break;
  case 0x67:
    IOS_Ioctlv(ctx);
    break;
  case 0x68:
    IOS_OpenAsync(ctx);
    break;
  case 0x69:
    IOS_CloseAsync(ctx);
    break;
  case 0x6A:
    IOS_ReadAsync(ctx);
    break;
  case 0x6B:
    IOS_IoctlAsync(ctx);
    break;
  case 0x6C:
    IOS_IoctlvAsync(ctx);
    break;
  default:

    
    if (process_pending_callbacks(ctx))
      return true;
    break;
  }

  ctx.pc = old_pc; 
  return false;
}

} 
} 

#include "runtime/cpu_context.h"

// Cached: std::getenv takes a lock and scans the environment, and these
// checks sit on the per-branch interrupt-poll path.
static const bool s_env_sample = std::getenv("NWII_SAMPLE") != nullptr;

namespace nwii::runtime {



bool g_ipc_reply_pending = false;

int g_ipc_interrupt_delay = 0;


static constexpr uint32_t CALLBACK_STACK_TOP_DEFAULT = 0x816F0000;

static inline uint32_t get_callback_stack_top() {
    uint32_t v = Config::get().game_profile.callback_stack_top;
    return v ? v : CALLBACK_STACK_TOP_DEFAULT;
}

void hle_drive_thread_queue(CPUContext& ctx) {
    
}



static void hle_save_context_to_guest(CPUContext &ctx, uint32_t c) {
  if (s_env_sample) {
      static int n = 0;
      if (n++ < 40)
          std::cout << "[CtxSave] #" << n << " ctx=0x" << std::hex << c
                    << " srr0<=0x" << ctx.pc << " msr=0x" << ctx.msr
                    << " lr=0x" << ctx.lr << std::dec << "\n";
  }
  for (int i = 0; i < 32; i++)
    ctx.mmu.write32(c + i * 4, ctx.gpr[i]);

  

  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) | 0x0002);
  uint32_t cr_val = 0;
  for (int i = 0; i < 8; i++) {
    cr_val |= (ctx.cr[i].lt ? 8u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].gt ? 4u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].eq ? 2u : 0) << (28 - i * 4);
    cr_val |= (ctx.cr[i].so ? 1u : 0) << (28 - i * 4);
  }
  ctx.mmu.write32(c + 0x80, cr_val);
  ctx.mmu.write32(c + 0x84, ctx.lr);
  ctx.mmu.write32(c + 0x88, ctx.ctr);
  ctx.mmu.write32(c + 0x8C, ctx.xer);
  for (int i = 0; i < 32; i++) {
    uint64_t d;
    std::memcpy(&d, &ctx.fpr[i], 8);
    ctx.mmu.write64(c + 0x90 + i * 8, d);
  }
  ctx.mmu.write32(c + 0x194, ctx.fpscr);
  ctx.mmu.write32(c + 0x198, ctx.pc);  
  ctx.mmu.write32(c + 0x19C, ctx.msr); 
  for (int i = 0; i < 8; i++)
    ctx.mmu.write32(c + 0x1A4 + i * 4, ctx.gqr[i]);
  for (int i = 0; i < 32; i++) {
    uint64_t d;
    std::memcpy(&d, &ctx.ps1[i], 8);
    ctx.mmu.write64(c + 0x1C8 + i * 8, d);
  }
}


static bool hle_load_context_from_guest(CPUContext &ctx) {
  uint32_t c = ctx.mmu.read32(0x800000D4);
  if (c == 0)
    return false;
  for (int i = 0; i < 32; i++)
    ctx.gpr[i] = ctx.mmu.read32(c + i * 4);
  uint32_t cr_val = ctx.mmu.read32(c + 0x80);
  for (int i = 0; i < 8; i++) {
    uint32_t nib = (cr_val >> (28 - i * 4)) & 0xF;
    ctx.cr[i].lt = (nib & 8) != 0;
    ctx.cr[i].gt = (nib & 4) != 0;
    ctx.cr[i].eq = (nib & 2) != 0;
    ctx.cr[i].so = (nib & 1) != 0;
  }
  ctx.lr = ctx.mmu.read32(c + 0x84);
  ctx.ctr = ctx.mmu.read32(c + 0x88);
  ctx.xer = ctx.mmu.read32(c + 0x8C);
  for (int i = 0; i < 32; i++) {
    uint64_t d = ctx.mmu.read64(c + 0x90 + i * 8);
    std::memcpy(&ctx.fpr[i], &d, 8);
  }
  ctx.fpscr = ctx.mmu.read32(c + 0x194);
  for (int i = 0; i < 8; i++)
    ctx.gqr[i] = ctx.mmu.read32(c + 0x1A4 + i * 4);
  for (int i = 0; i < 32; i++) {
    uint64_t d = ctx.mmu.read64(c + 0x1C8 + i * 8);
    std::memcpy(&ctx.ps1[i], &d, 8);
  }
  ctx.pc = ctx.mmu.read32(c + 0x198);  
  ctx.msr = ctx.mmu.read32(c + 0x19C); 

  

  
  ctx.mmu.write16(c + 0x1A2, ctx.mmu.read16(c + 0x1A2) & ~0x0002);
  return true;
}

int g_trace_after_dec = 0;

// --- VI retrace source -----------------------------------------------------
// One VI interrupt per retrace, at vi_target_hz() (NWII_VI_HZ, default 60).
// Self-rescheduling one-shot instead of a fixed recurring event so GXDrawDone
// can pull the next retrace forward (vi_request_retrace, below).
static uint64_t s_vi_period = 0; // timebase ticks per retrace (0 = not started)
static uint64_t s_vi_last = 0;   // timebase of the last retrace
static uint64_t s_vi_due = 0;    // timebase the pending retrace fires at
// First retrace after the previous frame end: the one that released the
// game's VIWaitForRetrace. Retraces in the middle of a frame (nobody waits)
// must not delay the next one.
static uint64_t s_vi_frame_retrace = 0;
static bool s_vi_frame_open = false; // frame ended, its retrace not fired yet
static uint64_t s_vi_paced = 0;      // steady-pacing period last used (NWII_FRAMEHIST)
// Recent frame work times (from the retrace that released a frame to that
// frame's GXDrawDone), for the steady pacing below. Work, not the paced
// period, or the pacing would feed on itself and never speed up again.
static uint64_t s_vi_hist[32] = {};
static unsigned s_vi_hist_n = 0;
static nwii::runtime::EventScheduler::EventId s_vi_event = 0;

// [graphics] frame_rate (live from the settings overlay); NWII_VI_HZ overrides.
static int vi_target_hz() {
    static const int env_hz = [] {
        const char *e = std::getenv("NWII_VI_HZ");
        return e && std::atoi(e) > 0 ? std::atoi(e) : 0;
    }();
    return env_hz ? env_hz : nwii::runtime::Config::get().frame_rate;
}

static void vi_fire(CPUContext &c, uint64_t);

static void vi_schedule_at(uint64_t due) {
    auto &es = nwii::runtime::EventScheduler::get();
    if (s_vi_event) es.cancel(s_vi_event);
    const uint64_t now = es.now();
    s_vi_due = due;
    s_vi_event = es.schedule_after(due > now ? due - now : 0, vi_fire);
}

uint64_t g_vi_fired_total = 0; // retraces so far (NWII_VISTAT)

static void vi_fire(CPUContext &c, uint64_t) {
    s_vi_event = 0;
    s_vi_period = std::max<uint64_t>(1, c.tb_freq / vi_target_hz());
    ++g_vi_fired_total;
    s_vi_last = nwii::runtime::EventScheduler::get().now();
    if (s_vi_frame_open) {
        // NWII_FRAMEHIST=1 also: frame START intervals (retrace that releases
        // the game to retrace that releases the next frame), 1 ms buckets,
        // every 5 s -- the game reads its clock there, so this spread is
        // what moving objects step by.
        static const bool hist = std::getenv("NWII_FRAMEHIST") != nullptr;
        if (hist && s_vi_frame_retrace) {
            static uint32_t b[32] = {};
            static uint64_t t0 = s_vi_last;
            ++b[std::min<uint64_t>(31, (s_vi_last - s_vi_frame_retrace) * 1000 / c.tb_freq)];
            if (s_vi_last - t0 >= c.tb_freq * 5) {
                t0 = s_vi_last;
                std::printf("[STARTHIST] period %.2fms |", s_vi_paced * 1000.0 / c.tb_freq);
                for (int i = 0; i < 32; ++i)
                    if (b[i]) std::printf(" %dms:%u", i, b[i]);
                std::printf("\n");
                std::fill(std::begin(b), std::end(b), 0u);
            }
        }
        s_vi_frame_retrace = s_vi_last;
        s_vi_frame_open = false;
    }
    const bool deliver =
        (nwii::runtime::hw::pi_intmr & 0x100) && !c.in_callback &&
        !(nwii::runtime::hw::pi_intsr & 0x100) &&
        (c.dispatch_saved_ctx == 0 || c.mmu.read32(0x800000E4) == 0);
    if (deliver)
        nwii::runtime::hw::vi_trigger_interrupt();
    // NWII_VISTAT=1: VI events fired vs delivered, every 5 s.
    static const bool vistat = std::getenv("NWII_VISTAT") != nullptr;
    if (vistat) {
        static uint32_t fired = 0, delivered = 0, why_cb = 0, why_pending = 0, why_ctx = 0;
        static auto t0 = std::chrono::steady_clock::now();
        ++fired;
        if (deliver) ++delivered;
        else if (c.in_callback) ++why_cb;
        else if (nwii::runtime::hw::pi_intsr & 0x100) ++why_pending;
        else ++why_ctx;
        const auto now = std::chrono::steady_clock::now();
        if (now - t0 >= std::chrono::seconds(5)) {
            t0 = now;
            std::printf("[VISTAT] fired=%u delivered=%u skipped: in_callback=%u "
                        "still_pending=%u other=%u\n",
                        fired, delivered, why_cb, why_pending, why_ctx);
            fired = delivered = why_cb = why_pending = why_ctx = 0;
        }
    }
    // While steady pacing holds frames apart, the free-running retraces
    // follow the paced period too: an 8.3 ms one landing mid-frame (or before
    // the paced one) let the game start its next frame early.
    vi_schedule_at(s_vi_last + std::max(s_vi_period, s_vi_paced));
}

// Adaptive retrace (NWII_VI_ADAPTIVE=0 turns it off): called from the
// GXDrawDone seam at the end of each game frame, right before the game's
// VIFlush + VIWaitForRetrace. The next retrace then comes at
// max(now + 0.3 ms, previous frame's retrace + period) instead of on a fixed
// grid, so a
// frame costs max(its work, one period) rather than a whole number of
// periods (a 9 ms frame at 120 Hz took 16.7 ms; an 18 ms one at 60 took
// 33). Safe for Spyro: it simulates on OS time, not retrace counts. The
// 0.3 ms lets the game reach VIWaitForRetrace first, or it would wait for the
// retrace after.
void vi_request_retrace(CPUContext &ctx) {
    static const bool adaptive = [] {
        const char *e = std::getenv("NWII_VI_ADAPTIVE");
        return !(e && e[0] == '0');
    }();
    // NWII_VISTAT: per frame, retraces since the previous frame end and the
    // time from the last retrace to this frame end (the frame's work).
    static const bool vistat = std::getenv("NWII_VISTAT") != nullptr;
    if (vistat) {
        static uint64_t frames = 0, work_tb = 0, last_count = 0, hist[5] = {};
        extern uint64_t g_vi_fired_total;
        const uint64_t now = ctx.read_timebase();
        ++frames;
        work_tb += now - s_vi_last;
        ++hist[std::min<uint64_t>(4, g_vi_fired_total - last_count)];
        last_count = g_vi_fired_total;
        if (frames == 300) {
            std::printf("[VIFRAME] avg work since last retrace %.2f ms | retraces per "
                        "frame 0:%llu 1:%llu 2:%llu 3:%llu 4+:%llu\n",
                        (double)work_tb / frames * 1000.0 / ctx.tb_freq,
                        (unsigned long long)hist[0], (unsigned long long)hist[1],
                        (unsigned long long)hist[2], (unsigned long long)hist[3],
                        (unsigned long long)hist[4]);
            frames = work_tb = 0;
            std::fill(std::begin(hist), std::end(hist), 0);
        }
    }
    if (!adaptive || s_vi_period == 0) return;
    if (s_vi_frame_retrace && !s_vi_frame_open)
        s_vi_hist[s_vi_hist_n++ % 32] = ctx.read_timebase() - s_vi_frame_retrace;
    s_vi_frame_open = true;
    // Steady pacing (only above 60 Hz): frame times at the 120 setting swing
    // between ~8 and ~14 ms with the scene. The game moves each frame by the
    // PREVIOUS frame's measured time, so every swing shows up as a step that
    // doesn't match the frame it is displayed in: fast motion (charging)
    // shakes. The next frame starts no earlier than the 31st-fastest of the
    // last 32 frames' work (NWII_VI_STEADY=<0-31> picks the rank, 0 = off;
    // the 80th percentile, rank 25, still left charging visibly shaky) --
    // fewer fps where frames vary, evenly spaced.
    static const int steady_rank = [] {
        const char *e = std::getenv("NWII_VI_STEADY");
        return e ? std::clamp(std::atoi(e), 0, 31) : 30;
    }();
    uint64_t period = s_vi_period;
    if (steady_rank > 0 && vi_target_hz() > 60 && s_vi_hist_n >= 32) {
        uint64_t v[32];
        std::copy(std::begin(s_vi_hist), std::end(s_vi_hist), v);
        std::nth_element(v, v + steady_rank, v + 32);
        period = std::clamp<uint64_t>(v[steady_rank], s_vi_period, ctx.tb_freq / 30);
    }
    s_vi_paced = period;
    const uint64_t soon = ctx.read_timebase() + ctx.tb_freq * 3 / 10000;
    const uint64_t due = std::max(soon, s_vi_frame_retrace + period);
    // Paced: this retrace replaces a pending earlier one too.
    if (due < s_vi_due || period > s_vi_period) vi_schedule_at(due);
}

void GX_WGPIPE_Flush(); // gx.cpp: gather-pipe staging -> FIFO

bool process_pending_callbacks(CPUContext &ctx) {
  GX_WGPIPE_Flush();

  
  if (s_env_sample) {
      static uint32_t last_ee = 0, edges = 0;
      uint32_t ee = ctx.msr & 0x8000;
      if (ee && !last_ee && edges < 8)
          std::cout << "[EEup] #" << ++edges << " pc=0x" << std::hex << ctx.pc
                    << " lr=0x" << ctx.lr << std::dec << "\n";
      last_ee = ee;
  }

  
  if (g_trace_after_dec > 0) {
      static int skip = 20000; 
      if (skip > 0) { skip--; }
      else {
          g_trace_after_dec--;
          std::cout << "[T] 0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
                    << " r1=0x" << ctx.gpr[1] << " r3=0x" << ctx.gpr[3]
                    << " r29=0x" << ctx.gpr[29] << " sr0=0x" << ctx.srr0
                    << std::dec << "\n";
      }
  }

  {
    static uint32_t trace_pc = []() -> uint32_t {
      const char *e = std::getenv("NWII_LOOPTRACE");
      return e ? (uint32_t)std::strtoul(e, nullptr, 16) : 0;
    }();
    static uint64_t hits = 0;
    if (trace_pc && ctx.pc == trace_pc) {
      if ((hits++ % 200000) == 0)
        std::cout << "[Loop 0x" << std::hex << trace_pc << "] #" << std::dec
                  << hits << " r3=0x" << std::hex << ctx.gpr[3] << " r4=0x"
                  << ctx.gpr[4] << " r5=0x" << ctx.gpr[5] << " r30=0x"
                  << ctx.gpr[30] << " r31=0x" << ctx.gpr[31] << " lr=0x"
                  << ctx.lr << std::dec << "\n";
    }
  }

  {
    static bool sample = s_env_sample;
    static uint64_t n = 0;
    if (sample && (++n % 400000) == 0)
      std::cout << "[Sample] pc=0x" << std::hex << ctx.pc << " lr=0x" << ctx.lr
                << " r3=0x" << ctx.gpr[3] << std::dec << "\n";
  }

  

  
  if (ctx.pc == 0xFFFFFFFC && !ctx.in_callback) {

    

    
    
    static uint32_t g_resched_addr = []() -> uint32_t {
      const char *e = std::getenv("NWII_RESCHED");
      return e ? (uint32_t)std::strtoul(e, nullptr, 16)
               : nwii::runtime::Config::get().game_profile.resched_addr;
    }();
    if (ctx.ext_resched_pending && g_resched_addr) {
      ctx.ext_resched_pending = false;
      if (s_env_sample) {
        static int n = 0;
        if (n++ < 12) {
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          std::cout << "[Resched] cur=0x" << std::hex << cur
                    << " prio=" << std::dec << ctx.mmu.read32(cur + 0x2D0)
                    << " r1=0x" << std::hex << ctx.gpr[1] << std::dec << "\n";
        }
      }
      ctx.gpr[3] = 0;
      ctx.lr = 0xFFFFFFFC;
      ctx.pc = g_resched_addr;
      return true;
    }
    ctx.ext_resched_pending = false;
    if (ctx.dispatch_saved_ctx) {
      uint32_t current_thread = ctx.mmu.read32(0x800000E4);
      if (current_thread != 0 && current_thread != ctx.dispatch_saved_ctx) {
        ctx.mmu.write32(0x800000D4, current_thread);
      } else {
        ctx.mmu.write32(0x800000D4, ctx.dispatch_saved_ctx);
      }
      ctx.dispatch_saved_ctx = 0;
    }
    if (hle_load_context_from_guest(ctx)) {

      

      
      ctx.dec_written_tb = ctx.read_timebase();
      static uint32_t restore_count = 0;
      restore_count++;
      if (nwii::runtime::g_verbose && (ctx.pc == 0 || (restore_count % 64) == 1)) {
          std::cout << "[HLE PI] Context restore #" << std::dec << restore_count
                    << " pc=0x" << std::hex << ctx.pc << " r1=0x" << ctx.gpr[1]
                    << " msr=0x" << ctx.msr << std::dec << "\n";
      }
      return true;
    }
  }

  if (nwii::runtime::g_verbose && (ctx.inst_count % 20000000) == 0) {
      std::cout << "[Heartbeat] PC: 0x" << std::hex << ctx.pc << " LR: 0x" << ctx.lr
                << " intmr=0x" << nwii::runtime::hw::pi_intmr.load()
                << " intsr=0x" << nwii::runtime::hw::pi_intsr.load()
                << " msr=0x" << ctx.msr
                << " dec_h=0x" << ctx.mmu.read32(0x80003020) << "\n";
      {
          std::cout << "  [ExcTable]";
          for (int i = 0; i < 16; i++) {
              uint32_t h = ctx.mmu.read32(0x80003000 + i * 4);
              if (h) std::cout << " " << i << ":0x" << std::hex << h << std::dec;
          }
          std::cout << "\n";
      }

      
      
      auto dump_thread = [&](uint32_t th, const char *tag) {
          uint16_t state = ctx.mmu.read16(th + 0x2C8);
          uint32_t prio = ctx.mmu.read32(th + 0x2D0);
          uint32_t wqueue = ctx.mmu.read32(th + 0x2DC);
          uint32_t srr0 = ctx.mmu.read32(th + 0x198);
          uint32_t slr = ctx.mmu.read32(th + 0x84);
          std::cout << "  [" << tag << "] 0x" << std::hex << th << " state=" << state
                    << " prio=" << std::dec << prio << " waitq=0x" << std::hex
                    << wqueue << " srr0=0x" << srr0 << " lr=0x" << slr;
          
          uint32_t sp = ctx.mmu.read32(th + 4);
          for (int f = 0; f < 6 && sp != 0 && sp < 0x94000000; ++f) {
              uint32_t ret = ctx.mmu.read32(sp + 4);
              if (ret) std::cout << " <-0x" << std::hex << ret;
              sp = ctx.mmu.read32(sp);
          }
          std::cout << std::dec << "\n";
      };
      
      {
          static uint32_t peek_addr = 0, peek_words = 8;
          static bool peek_parsed = false;
          if (!peek_parsed) {
              peek_parsed = true;
              if (const char *env = std::getenv("NWII_PEEK")) {
                  peek_addr = (uint32_t)std::strtoul(env, nullptr, 16);
                  if (const char *c = std::strchr(env, ','))
                      peek_words = (uint32_t)std::strtoul(c + 1, nullptr, 10);
              }
          }
          if (peek_addr) {
              std::cout << "  [Peek 0x" << std::hex << peek_addr << "]";
              for (uint32_t i = 0; i < peek_words; i++)
                  std::cout << " " << ctx.mmu.read32(peek_addr + i * 4);
              std::cout << std::dec << "\n";
          }
      }
      uint32_t cur = ctx.mmu.read32(0x800000E4);
      if (cur) dump_thread(cur, "CurThread");
      uint32_t th = ctx.mmu.read32(0x800000DC);
      int guard = 0;
      while (th != 0 && guard++ < 16) {
          if (th != cur) dump_thread(th, "Thread");
          th = ctx.mmu.read32(th + 0x2FC);
      }
  }

  
  
  if (ctx.pc == 0) {
      static uint64_t idle_hits = 0;
      static int idle_dumps = 0;
      // Periodic (capped) while idle, so a stall later in the game shows
      // which queue every thread is parked on.
      if ((++idle_hits % 20000) == 0 && idle_dumps++ < 40) {
          std::cout << "[IDLE] no runnable thread. intmr=0x" << std::hex
                    << nwii::runtime::hw::pi_intmr << " intsr=0x"
                    << nwii::runtime::hw::pi_intsr << " dec_armed="
                    << std::dec << ctx.dec_irq_pending << "\n";
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          uint32_t th = ctx.mmu.read32(0x800000DC);
          int guard = 0;
          while (th != 0 && guard++ < 24) {
              uint16_t state = ctx.mmu.read16(th + 0x2C8);
              uint32_t prio = ctx.mmu.read32(th + 0x2D0);
              uint32_t wq = ctx.mmu.read32(th + 0x2DC);
              uint32_t srr0 = ctx.mmu.read32(th + 0x198);
              std::cout << "  [IdleThread] 0x" << std::hex << th
                        << (th == cur ? "*" : " ") << " state=" << std::dec
                        << state << " prio=" << prio << " waitq=0x" << std::hex
                        << wq << " srr0=0x" << srr0 << std::dec << "\n";
              th = ctx.mmu.read32(th + 0x2FC);
          }
      }
  }

  if (g_ipc_interrupt_delay > 0) {
      g_ipc_interrupt_delay--;
      if (g_ipc_interrupt_delay == 0) {
          nwii::runtime::hw::trigger_pi_interrupt(0x00004000);
          if (s_env_sample)
              std::cout << "[IPC] irq raised intmr=0x" << std::hex
                        << nwii::runtime::hw::pi_intmr << " msr=0x" << ctx.msr
                        << " pc=0x" << ctx.pc << std::dec << "\n";
      }
  }

  
  nwii::runtime::hw::di_tick();

  if (ctx.vblank_pending) {
      if (!ctx.in_callback && (ctx.msr & 0x8000)) {
          ctx.vblank_pending = false;
          // With a wall timebase the 60 Hz scheduler event below is the VI
          // source; raising it on every host present as well tripled the
          // retrace rate, so VIWaitForRetrace barely slept and lower-priority
          // threads (Spyro's streamer) never ran.
          if (!nwii::runtime::g_wall_timebase)
              nwii::runtime::hw::vi_trigger_interrupt();

          
          
          hle_drive_thread_queue(ctx);
      }
  }

  if (ctx.in_callback && ctx.pc == 0xFFFFFFFC) {
    
    ctx.in_callback = false;
  }

  if (ctx.in_callback) {
    if (s_env_sample && (nwii::runtime::hw::pi_intsr & 0x4000)) {
        static uint64_t n = 0;
        if ((n++ % 100000) == 0)
            std::cout << "[IPC] irq pending but in_callback pc=0x" << std::hex
                      << ctx.pc << " lr=0x" << ctx.lr << " depth="
                      << std::dec << ctx.callback_depth << "\n";
    }
    return false;
  }

  // Periodically raise VI (PI_INTSR bit 8 = 0x100, Dolphin INT_CAUSE_VI)

  

  

  

  

  
  
  if (s_vi_period == 0) {
      s_vi_period = ctx.tb_freq / vi_target_hz();
      if (s_vi_period == 0) s_vi_period = 500000;
      vi_schedule_at(EventScheduler::get().now() + s_vi_period);
  }
  // One wall-clock read shared by the scheduler and the decrementer.
  static uint32_t s_tb_tick = 0;
  static uint64_t s_tb_now = 0;
  // (Back-edges already call in only every 16th time, see poll_callbacks.)
  (void)s_tb_tick;
  s_tb_now = ctx.read_timebase();
  nwii::runtime::EventScheduler::get().advance(ctx, s_tb_now);

  
  if (ctx.dec_irq_pending && s_tb_now >= ctx.dec_written_tb &&
      (s_tb_now - ctx.dec_written_tb) >= ctx.dec_value &&
      (ctx.msr & 0x8000) && !ctx.in_callback) {
      ctx.dec_irq_pending = false; 
      if (s_env_sample) {
          static uint32_t df = 0;
          if (df++ < 12)
              std::cout << "[DECfire] #" << df << " pc=0x" << std::hex << ctx.pc
                        << " lr=0x" << ctx.lr << " inst=" << std::dec
                        << ctx.inst_count << "\n";
          extern int g_trace_after_dec;
          if (df == 1) g_trace_after_dec = 300;
      }

      

      
      {
          static uint32_t dec_same_pc_count = 0;
          static uint32_t dec_last_pc = 0;
          if (ctx.pc == dec_last_pc) {
              dec_same_pc_count++;
          } else {
              dec_same_pc_count = 0;
              dec_last_pc = ctx.pc;
          }

          if (dec_same_pc_count == 64) {
              dec_same_pc_count = 0;
              if (ctx.gpr[13] != 0) {
                  uint64_t now_tb = ctx.read_timebase();
                  uint32_t now_hi = (uint32_t)(now_tb >> 32);
                  uint32_t now_lo = (uint32_t)(now_tb & 0xFFFFFFFF);
                  uint32_t base = ctx.gpr[13];
                  for (int i = 0; i < 512; i++) {
                      uint32_t addr = base - (uint32_t)(i * 4);
                      if (addr < 0x80000000u || addr > 0x81FFFFFFu) continue;
                      uint32_t handler = ctx.mmu.read32(addr + 0x18);
                      if (handler < 0x80000000u || handler >= 0x82000000u) continue;
                      uint32_t fire_hi = ctx.mmu.read32(addr + 0x08);
                      uint32_t fire_lo = ctx.mmu.read32(addr + 0x0C);
                      if (fire_hi == 0 && fire_lo == 0) continue;
                      if (fire_hi < now_hi || (fire_hi == now_hi && fire_lo <= now_lo))
                          continue;
                      ctx.mmu.write32(addr + 0x08, now_hi);
                      ctx.mmu.write32(addr + 0x0C, now_lo);
                      static uint32_t forced_count = 0;
                      if ((forced_count++ % 32) == 0)
                          std::cout << "[HLE DEC] force-expired alarm 0x"
                                    << std::hex << addr << " handler=0x"
                                    << handler << std::dec << "\n";
                  }
              }
              return false;
          }
      }

      
      static uint32_t dcount = 0;
      if ((dcount++ % 500) == 0) {
          uint32_t cur = ctx.mmu.read32(0x800000E4);
          uint32_t th = ctx.mmu.read32(0x800000DC);
          std::cout << "[Sched] cur=0x" << std::hex << cur << std::dec << "\n";
          int guard = 0;
          while (th != 0 && guard++ < 24) {
              uint16_t state = ctx.mmu.read16(th + 0x2C8);
              uint32_t prio = ctx.mmu.read32(th + 0x2D0);
              uint32_t wq = ctx.mmu.read32(th + 0x2DC);
              uint32_t srr0 = ctx.mmu.read32(th + 0x198);
              std::cout << "  th=0x" << std::hex << th << (th==cur?"*":" ")
                        << " state=" << std::dec << state << " prio=" << prio
                        << " waitq=0x" << std::hex << wq << " srr0=0x" << srr0
                        << std::dec << "\n";

              

              if (wq >= 0x80000000u && wq < 0x81800000u &&
                  s_env_sample) {
                  std::cout << "    wq[-2..5]:" << std::hex;
                  for (int i = -2; i < 6; ++i)
                      std::cout << " " << ctx.mmu.read32(wq + i * 4);
                  std::cout << std::dec << "\n";
              }
              th = ctx.mmu.read32(th + 0x2FC);
          }
      }
      uint32_t dec_handler = ctx.mmu.read32(0x80003000 + 8 * 4);
      uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
      if (dec_handler != 0 && dec_handler != 0xFFFFFFFF && current_ctx != 0) {
          hle_save_context_to_guest(ctx, current_ctx);
          ctx.dispatch_saved_ctx = current_ctx;
          ctx.srr0 = ctx.pc;
          ctx.srr1 = ctx.msr;
          ctx.msr &= ~0x8000;
          ctx.gpr[3] = 8;           
          ctx.gpr[4] = current_ctx; 

          
          
          ctx.lr = 0xFFFFFFFC;
          ctx.pc = dec_handler;
          longjmp(ctx.exception_jmp_buf, 1);
      }
  }

  uint32_t active_ints = nwii::runtime::hw::pi_intsr & nwii::runtime::hw::pi_intmr;

  
  if (active_ints != 0 && (ctx.msr & 0x8000)) {

      

      
      
      bool sched_disabled = false;
      uint32_t r13 = ctx.gpr[13];
      uint32_t lock_off = nwii::runtime::Config::get().game_profile.sched_lock_offset;
      if (lock_off != 0 && r13 >= 0x80200000u && r13 < 0x81800000u) {
          uint32_t lock_addr = r13 - lock_off;
          if (lock_addr >= 0x80000000u && lock_addr < 0x82000000u) {
              uint32_t lock_val = ctx.mmu.read32(lock_addr);

              sched_disabled = (lock_val > 0 && lock_val < 256);
          }
      }
      bool handler_in_flight = (ctx.dispatch_saved_ctx != 0);

      

      
      bool use_new_path =
          (nwii::runtime::Config::get().game_profile.ext_interrupt_dispatch != 0);

      

      
      bool idle = (ctx.mmu.read32(0x800000E4) == 0);

      

      

      if (use_new_path && (handler_in_flight || (!idle && sched_disabled))) {
          
          if (s_env_sample) {
              static uint64_t defer_count = 0;
              if ((++defer_count % 500000) == 0)
                  std::cout << "[HLE PI] int deferred: sched_disabled="
                            << sched_disabled << " in_flight=" << handler_in_flight
                            << " pc=0x" << std::hex << ctx.pc << std::dec << "\n";
          }
      } else {

          
          static uint32_t s_dispatch_addr = 0xFFFFFFFF; 
          if (s_dispatch_addr == 0xFFFFFFFF) {
              
              uint32_t from_cfg = nwii::runtime::Config::get().game_profile.ext_interrupt_dispatch;
              if (from_cfg != 0) {
                  s_dispatch_addr = from_cfg;
                  std::cout << "[HLE PI] __OSDispatchInterrupt from config: 0x"
                            << std::hex << s_dispatch_addr << std::dec << "\n";
              } else {

                  
                  s_dispatch_addr = 0;
              }
          }

          if (s_dispatch_addr != 0) {

              

              

              

              

              ctx.srr0 = ctx.pc;
              ctx.srr1 = ctx.msr;

              
              
              uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
              if (current_ctx == 0) {
                  current_ctx = ctx.mmu.read32(0x800000D0);
              }
              if (current_ctx == 0) {
                  std::cout << "[HLE PI] FATAL: current_ctx is 0! pc=0x" << std::hex << ctx.pc << " intmr=0x" << nwii::runtime::hw::pi_intmr << " intsr=0x" << nwii::runtime::hw::pi_intsr << std::dec << "\n";
              }

              
              
              if (current_ctx != 0) {
                  hle_save_context_to_guest(ctx, current_ctx);
              }

              
              ctx.gpr[3] = 0x04;
              
              ctx.gpr[4] = current_ctx;

              
              ctx.msr &= ~0x8000u; 

              
              
              ctx.lr = 0xFFFFFFFC;
              ctx.pc = s_dispatch_addr;

              if (s_env_sample) {
                  static uint32_t dc = 0;
                  if ((dc++ % 1000) == 0)
                      std::cout << "[HLE PI] -> __OSDispatchInterrupt #" << std::dec
                                << dc << " pc=0x" << std::hex << ctx.pc
                                << " cur=0x" << ctx.mmu.read32(0x800000E4)
                                << std::dec << "\n";
              }

              longjmp(ctx.exception_jmp_buf, 1);

          } else {

              
              int os_intr = -1;
              uint32_t bit_to_clear = 0;
              
              if      (active_ints & 0x00004000) { os_intr = 27; bit_to_clear = 0x00004000; } 
              else if (active_ints & 0x00000100) { os_intr = 24; bit_to_clear = 0x00000100; } 
              else if (active_ints & 0x00000004) { os_intr = 21; bit_to_clear = 0x00000004; } 
              else if (active_ints & 0x00000008) { os_intr = 20; bit_to_clear = 0x00000008; } 
              else if (active_ints & 0x00000010) { os_intr = nwii::runtime::hw::exi_pending_os_interrupt(); bit_to_clear = 0x00000010; } 

              else if (active_ints & 0x00000040) { os_intr = nwii::runtime::hw::dsp_pending_os_interrupt(); bit_to_clear = 0x00000040; } 
              else if (active_ints & 0x00000020) { os_intr =  8; bit_to_clear = 0x00000020; } 
              else if (active_ints & 0x00000400) { os_intr = 19; bit_to_clear = 0x00000400; } 
              else if (active_ints & 0x00000200) { os_intr = 18; bit_to_clear = 0x00000200; } 
              else if (active_ints & 0x00000800) { os_intr = 17; bit_to_clear = 0x00000800; } 

              if (os_intr != -1) {
                  uint32_t handler = ctx.mmu.read32(0x80003040 + os_intr * 4);

                  if (handler != 0 && handler != 0xFFFFFFFF) {
                      if (s_env_sample && os_intr == 24) {
                          static int vn = 0;
                          if (vn++ < 5)
                              std::cout << "[VIdisp] #" << vn << " handler=0x"
                                        << std::hex << handler << " pc=0x"
                                        << ctx.pc << std::dec << "\n";
                      }
                      
                      static uint32_t vi_dispatch_count = 0;
                      if (nwii::runtime::g_verbose && os_intr != 24 && (vi_dispatch_count++ % 256) == 0) {
                          std::cout << "[HLE PI] Dispatching interrupt "
                                    << std::dec << os_intr << " to handler 0x"
                                    << std::hex << handler << " r1=0x"
                                    << ctx.gpr[1] << " pc=0x" << ctx.pc
                                    << std::dec << std::endl;
                      }
                      if (s_env_sample) {
                          static uint64_t census[32] = {0};
                          static uint64_t total = 0;
                          if (os_intr >= 0 && os_intr < 32) census[os_intr]++;
                          if ((++total % 2000) == 0) {
                              std::cout << "[IntCensus]";
                              for (int i = 0; i < 32; ++i)
                                  if (census[i])
                                      std::cout << " " << std::dec << i
                                                << ":" << census[i];
                              std::cout << "\n";
                          }
                      }
                      uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
                      if (current_ctx == 0) {
                          
                          if (s_env_sample) {
                              static int n = 0;
                              if (n++ < 4)
                                  std::cout << "[HLE PI] int " << std::dec
                                            << os_intr << " deferred: no current context\n";
                          }
                      } else {
                          
                          nwii::runtime::hw::pi_intsr &= ~bit_to_clear;
                          hle_save_context_to_guest(ctx, current_ctx);
                          ctx.dispatch_saved_ctx = current_ctx;
                          ctx.srr0 = ctx.pc;
                          ctx.srr1 = ctx.msr;
                          ctx.msr &= ~0x8000u; 
                          ctx.gpr[3] = os_intr;
                          ctx.gpr[4] = current_ctx;
                          ctx.lr = 0xFFFFFFFC;
                          ctx.pc = handler;
                          ctx.ext_resched_pending = true;
                          longjmp(ctx.exception_jmp_buf, 1);
                      }

                      ctx.msr &= ~0x8000u;
                      CPUContext::BackupState bk;
                      bk.gpr = ctx.gpr;
                      bk.fpr = ctx.fpr;
                      bk.ps1 = ctx.ps1;
                      bk.cr  = ctx.cr;
                      bk.lr  = ctx.lr;
                      bk.ctr = ctx.ctr;
                      bk.xer = ctx.xer;
                      bk.pc  = ctx.pc;
                      bk.srr0 = ctx.srr0;
                      bk.srr1 = ctx.srr1;
                      bk.msr = ctx.msr | 0x8000;
                      bk.fpscr = ctx.fpscr;
                      bk.gqr = ctx.gqr;
                      bk.sprg = ctx.sprg;
                      ctx.backup_stack.push(bk);

                      ctx.in_callback = true;
                      ctx.callback_depth++;
                      ctx.srr0 = ctx.pc;
                      ctx.srr1 = ctx.msr | 0x8000;
                      ctx.gpr[3] = os_intr;
                      ctx.gpr[4] = 0;
                      ctx.lr = 0xFFFFFFFC;
                      ctx.pc = handler;
                      longjmp(ctx.exception_jmp_buf, 1);
                  } else {
                      
                      static uint32_t last_null_pc = 0;
                      static int null_warn_count = 0;
                      if (ctx.pc != last_null_pc) {
                          null_warn_count = 0;
                          last_null_pc = ctx.pc;
                      }
                      if (null_warn_count++ < 3)
                          std::cout << "[HLE PI] Handler for interrupt "
                                    << std::dec << os_intr
                                    << " is NULL (OS reinit in progress, deferring)."
                                    << std::endl;
                  }
              }
          } 
      } 
  } else if (nwii::runtime::hw::pi_intsr != 0) {
      
  }

  if ((ctx.msr & 0x8000) == 0)
    return false;

  if (ctx.pending_cb_count.load(std::memory_order_relaxed) == 0)
    return false;

  CallbackInfo cb;
  {
    std::lock_guard<std::mutex> lock(ctx.cb_mutex);
    if (ctx.pending_callbacks.empty())
      return false;
    cb = ctx.pending_callbacks.front();
    ctx.pending_callbacks.pop();
    ctx.pending_cb_count.fetch_sub(1, std::memory_order_relaxed);
  }

  std::cout << "[HLE Callback] Dispatching cb=0x" << std::hex << cb.cb_addr
            << " arg1=0x" << cb.arg1 << " arg2=0x" << cb.arg2 << " from PC=0x"
            << ctx.pc << " LR=0x" << ctx.lr << " r1=0x" << ctx.gpr[1]
            << std::dec << std::endl;

  uint32_t current_ctx = ctx.mmu.read32(0x800000D4);
  if (current_ctx != 0) {

    hle_save_context_to_guest(ctx, current_ctx);
    ctx.dispatch_saved_ctx = current_ctx;

    ctx.srr0 = ctx.pc;
    ctx.srr1 = ctx.msr;
    ctx.msr &= ~0x8000; 

    ctx.gpr[3] = cb.arg1;
    ctx.gpr[4] = cb.arg2;

    ctx.lr = 0xFFFFFFFC;
    ctx.pc = cb.cb_addr;
    longjmp(ctx.exception_jmp_buf, 1);
  }

  CPUContext::BackupState bk;
  bk.gpr = ctx.gpr;
  bk.fpr = ctx.fpr;
  bk.ps1 = ctx.ps1;
  bk.cr  = ctx.cr;
  bk.lr  = ctx.lr;
  bk.ctr = ctx.ctr;
  bk.xer = ctx.xer;
  bk.pc  = ctx.pc;
  bk.srr0 = ctx.srr0;
  bk.srr1 = ctx.srr1;
  bk.msr = ctx.msr;
  bk.fpscr = ctx.fpscr;
  bk.gqr = ctx.gqr;
  bk.sprg = ctx.sprg;
  ctx.backup_stack.push(bk);

  ctx.in_callback = true;
  ctx.callback_depth++;

  ctx.gpr[3] = cb.arg1;
  ctx.gpr[4] = cb.arg2;

  uint32_t cb_stack_top = get_callback_stack_top() - (ctx.callback_depth - 1) * 4096;
  ctx.gpr[1] = cb_stack_top - 16;
  ctx.mmu.write32(ctx.gpr[1], 0);
  ctx.mmu.write32(ctx.gpr[1] + 4, 0xFFFFFFFC);

  ctx.lr = 0xFFFFFFFC;
  ctx.pc = cb.cb_addr;
  longjmp(ctx.exception_jmp_buf, 1);
}

} 
