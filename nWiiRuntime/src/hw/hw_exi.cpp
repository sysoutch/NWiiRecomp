#include <cstdlib>
#include "runtime/hw/hw.h"
#include "runtime/config.h"
#include "runtime/cpu_context.h"

#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <vector>

namespace nwii::runtime {
    extern MMU* g_mmu;
}

namespace nwii::runtime::hw {

// GC EXI (Dolphin ExpansionInterface): three channels of five registers at
// 0xCC006800 + ch*0x14, each with up to three chip-selected devices. Modelled:
//
//  * channel 0 / device 1 -- the IPL: boot ROM (only the fonts, for
//    OSInitFont; Spyro panics in bitfont.cpp without them, taken from
//    Dolphin's Sys/GC replacements next to the game) and SRAM;
//  * channel 0 / device 0 -- memory card slot A, a 59-block card backed by
//    memcard_A.raw next to the game, so the SDK's own CARD library can
//    format, save and load.
//
// Everything else reports nothing attached (the bus floats high).

static constexpr uint32_t CSR_EXIINTMASK = 0x001, CSR_EXIINT = 0x002;
static constexpr uint32_t CSR_TCINTMASK = 0x004, CSR_TCINT = 0x008;
static constexpr uint32_t CSR_EXTINTMASK = 0x400, CSR_EXTINT = 0x800;
static constexpr uint32_t CSR_EXT = 0x1000;
static constexpr uint32_t CSR_INT_BITS = CSR_EXIINT | CSR_TCINT | CSR_EXTINT;
static constexpr uint32_t CR_TSTART = 0x1, CR_DMA = 0x2;
static constexpr uint32_t PI_INT_EXI = 0x10;

struct ExiChannel {
    uint32_t csr = 0, mar = 0, len = 0, cr = 0, data = 0;
    int selected = -1; // device index from the CS bits, -1 = none
};
static ExiChannel exi[3];

static void update_interrupt();

static std::filesystem::path find_next_to_game(const char *name) {
    namespace fs = std::filesystem;
    const fs::path game(Config::get().game_dir);
    const fs::path cfg_dir = fs::path(Config::get().config_path).parent_path();
    for (const fs::path &dir : {cfg_dir, game, game.parent_path(), fs::current_path()})
        if (fs::exists(dir / name))
            return dir / name;
    return {};
}

// A device sees one select, a stream of bytes (each in-byte answered with an
// out-byte, as on the real serial bus) and a deselect.
struct ExiDevice {
    virtual ~ExiDevice() = default;
    virtual void select() {}
    virtual void deselect() {}
    virtual uint8_t transfer(uint8_t in) = 0;
};

// --- IPL: ROM fonts, RTC, SRAM -------------------------------------------

class IplDevice : public ExiDevice {
public:
    static constexpr uint32_t ROM_SIZE = 0x200000;

    IplDevice() {
        // SRAM: zero except the flash-ID checksums. The CARD library checks
        // sramEx.flashIDCheckSum[slot] == ~sum(flashID[slot]); with all-zero
        // flash IDs that is 0xFF, and anything else fails every mount.
        sram_[0x3A] = 0xFF;
        sram_[0x3B] = 0xFF;
    }

    void select() override { pos_ = 0; cmd_ = 0; }

    uint8_t transfer(uint8_t in) override {
        if (pos_ < 4) { // the first four bytes are the address command
            cmd_ = (cmd_ << 8) | in;
            ++pos_;
            return 0;
        }
        const uint32_t cursor = pos_++ - 4;
        const bool write = cmd_ & 0x80000000u;
        const uint32_t a = cmd_ & 0x7FFFFFFFu;
        if ((a & 0x60000000) == 0) { // boot ROM
            ensure_rom();
            const uint32_t off = (a >> 6) + cursor;
            return off < ROM_SIZE ? rom_[off] : 0;
        }
        if (a >= 0x20000100 && a < 0x20000100 + (64u << 6)) { // SRAM
            const uint32_t off = ((a - 0x20000100) >> 6) + cursor;
            if (off >= sizeof(sram_)) return 0;
            if (write) sram_[off] = in;
            return sram_[off];
        }
        return 0; // RTC (0x20000000), UART, ...: zero
    }

private:
    void ensure_rom() {
        if (!rom_.empty())
            return;
        rom_.assign(ROM_SIZE, 0);
        load_font("font_japanese.bin", 0x1AFF00);
        load_font("font_western.bin", 0x1FCF00);
    }
    void load_font(const char *name, uint32_t rom_off) {
        const auto p = find_next_to_game(name);
        std::ifstream f(p, std::ios::binary);
        if (p.empty() || !f) {
            std::cout << "[HW EXI] " << name << " not found next to the game; "
                      << "OSInitFont will fail (copy it from Dolphin's Sys/GC)\n";
            return;
        }
        std::vector<char> buf((std::istreambuf_iterator<char>(f)), {});
        const size_t n = std::min<size_t>(buf.size(), ROM_SIZE - rom_off);
        std::memcpy(rom_.data() + rom_off, buf.data(), n);
        std::cout << "[HW EXI] IPL font " << p.string() << " (" << n
                  << " bytes) at ROM 0x" << std::hex << rom_off << std::dec << "\n";
    }

    std::vector<uint8_t> rom_;
    uint8_t sram_[64] = {};
    uint32_t pos_ = 0, cmd_ = 0;
};

// --- Memory card -----------------------------------------------------------

// Nintendo flash card protocol (Dolphin EXI_DeviceMemoryCard): command byte,
// address bytes, then data. Reads stream from an address that wraps inside a
// 512-byte page; programs buffer up to 128 bytes and commit on deselect;
// erases clear an 8 KiB sector to 0xFF. Completion raises the device
// interrupt when the SDK enabled it. Status starts "unlocked" so the CARD
// library skips the DSP-based unlock handshake.
class MemoryCardDevice : public ExiDevice {
public:
    static constexpr uint32_t SIZE_MBIT = 4;              // 59 usable blocks
    static constexpr uint32_t SIZE = SIZE_MBIT << 17;     // 512 KiB
    static constexpr uint8_t ST_UNLOCKED = 0x40, ST_ERASEERR = 0x10,
                             ST_PROGERR = 0x08, ST_SLEEP = 0x20, ST_READY = 0x01;

    explicit MemoryCardDevice(int channel) : channel_(channel) {
        data_.assign(SIZE, 0xFF); // erased flash = unformatted card
        // Next to the config (the game's folder), not next to the disc: a disc
        // image may be opened from anywhere through the launcher.
        const auto &cfg = Config::get();
        path_ = !cfg.config_path.empty()
                    ? std::filesystem::path(cfg.config_path).parent_path() / "memcard_A.raw"
                : cfg.game_dir.empty()
                    ? std::filesystem::path("memcard_A.raw")
                    : std::filesystem::path(cfg.game_dir).parent_path() / "memcard_A.raw";
        // NWII_MEMCARD=<file>: use another card image (test runs).
        if (const char *mc = std::getenv("NWII_MEMCARD"); mc && *mc) path_ = mc;
        if (std::ifstream f{path_, std::ios::binary}) {
            f.read(reinterpret_cast<char *>(data_.data()), SIZE);
            std::cout << "[HW EXI] memory card A: " << path_.string() << "\n";
        } else {
            std::ofstream(path_, std::ios::binary)
                .write(reinterpret_cast<const char *>(data_.data()), SIZE);
            std::cout << "[HW EXI] memory card A: created blank " << path_.string()
                      << "\n";
        }
    }

    void select() override { pos_ = 0; cmd_ = 0; addr_ = 0; prog_len_ = 0; }

    void deselect() override {
        if (pos_ == 0)
            return;
        if (cmd_ == 0xF1 && pos_ >= 3) { // sector erase
            const uint32_t base = (addr_ & (SIZE - 1)) & ~0x1FFFu;
            std::memset(data_.data() + base, 0xFF, 0x2000);
            persist(base, 0x2000);
            done();
        } else if (cmd_ == 0xF2 && pos_ >= 5) { // page program
            const uint32_t base = addr_ & (SIZE - 1);
            for (uint32_t i = 0; i < prog_len_; ++i) {
                const uint32_t a = (base & ~0x7Fu) | ((base + i) & 0x7F);
                data_[a] = prog_[i];
            }
            persist(base & ~0x7Fu, 0x80);
            done();
        } else if (cmd_ == 0xF4) { // chip erase
            std::fill(data_.begin(), data_.end(), 0xFF);
            persist(0, SIZE);
            done();
        }
    }

    uint8_t transfer(uint8_t in) override {
        const uint32_t pos = pos_++;
        if (pos == 0) {
            cmd_ = in;
            switch (cmd_) {
            case 0x89: status_ &= ~(ST_ERASEERR | ST_PROGERR); status_ |= ST_READY; break;
            case 0x87: status_ &= ~ST_SLEEP; break; // wake up
            case 0x88: status_ |= ST_SLEEP; break;  // sleep
            }
            return 0xFF;
        }
        switch (cmd_) {
        case 0x00: // Nintendo ID: dummy byte, then the size in Mbit (big endian)
            if (pos == 1) return 0x80;
            return (uint8_t)(SIZE_MBIT >> (24 - ((pos - 2) & 3) * 8));
        case 0x83: // read status
            return status_;
        case 0x81: // set interrupt enable
            if (pos == 1) irq_enable_ = in & 1;
            return 0xFF;
        case 0x52: // read array: 4 address bytes, latency bytes, then data
        case 0xF1: // sector erase: 2 address bytes
        case 0xF2: { // page program: 4 address bytes, then up to 128 bytes
            if (pos <= 4) {
                switch (pos) {
                case 1: addr_ = (uint32_t)(in & 0x7F) << 17; break;
                case 2: addr_ |= (uint32_t)in << 9; break;
                case 3: addr_ |= (uint32_t)(in & 3) << 7; break;
                case 4: addr_ |= in & 0x7F; break;
                }
                if (cmd_ != 0x52 || pos < 2) return 0xFF;
            }
            if (cmd_ == 0xF2) {
                if (pos >= 5 && prog_len_ < sizeof(prog_)) prog_[prog_len_++] = in;
                return 0xFF;
            }
            if (cmd_ != 0x52) return 0xFF;
            const uint8_t v = data_[addr_ & (SIZE - 1)];
            if (pos >= 9) // data phase: advance within the 512-byte page
                addr_ = (addr_ & ~0x1FFu) | ((addr_ + 1) & 0x1FF);
            return v;
        }
        default:
            return 0xFF;
        }
    }

private:
    void done() {
        status_ |= ST_READY;
        if (irq_enable_) {
            exi[channel_].csr |= CSR_EXIINT;
            update_interrupt();
        }
    }
    void persist(uint32_t off, uint32_t len) {
        // Write through: a crash or window close never loses a save.
        if (FILE *f = std::fopen(path_.string().c_str(), "r+b")) {
            std::fseek(f, (long)off, SEEK_SET);
            std::fwrite(data_.data() + off, 1, len, f);
            std::fclose(f);
        }
    }

    int channel_;
    std::filesystem::path path_;
    std::vector<uint8_t> data_;
    uint8_t status_ = ST_UNLOCKED | ST_READY;
    bool irq_enable_ = false;
    uint32_t pos_ = 0, addr_ = 0;
    uint8_t cmd_ = 0;
    uint8_t prog_[128] = {};
    uint32_t prog_len_ = 0;
};

// devices[channel][cs]
static std::unique_ptr<ExiDevice> devices[3][3];

static ExiDevice *device_of(int ch) {
    const int d = exi[ch].selected;
    return d >= 0 ? devices[ch][d].get() : nullptr;
}

static void ensure_devices() {
    static bool made = false;
    if (made)
        return;
    made = true; // after Config::game_dir is known (first EXI access)
    devices[0][1] = std::make_unique<IplDevice>();
    devices[0][0] = std::make_unique<MemoryCardDevice>(0);
}

static void update_interrupt() {
    bool any = false;
    for (const ExiChannel &c : exi) {
        if (((c.csr & CSR_EXIINT) && (c.csr & CSR_EXIINTMASK)) ||
            ((c.csr & CSR_TCINT) && (c.csr & CSR_TCINTMASK)) ||
            ((c.csr & CSR_EXTINT) && (c.csr & CSR_EXTINTMASK)))
            any = true;
    }
    if (any)
        trigger_pi_interrupt(PI_INT_EXI);
    else
        clear_pi_interrupt(PI_INT_EXI);
}

static void execute(int ch) {
    ExiChannel &c = exi[ch];
    const uint32_t rw = (c.cr >> 2) & 3; // 0 read, 1 write, 2 read/write
    ExiDevice *dev = device_of(ch);
    auto xfer = [&](uint8_t in) -> uint8_t { return dev ? dev->transfer(in) : 0xFF; };

    if (c.cr & CR_DMA) {
        const uint32_t addr = 0x80000000u | (c.mar & 0x01FFFFE0u);
        const uint32_t len = c.len & 0x03FFFFE0u;
        if (g_mmu) {
            for (uint32_t i = 0; i < len; ++i) {
                if (rw == 0)
                    g_mmu->write8(addr + i, xfer(0));
                else
                    xfer(g_mmu->read8(addr + i));
            }
        }
    } else {
        const uint32_t n = ((c.cr >> 4) & 3) + 1;
        uint32_t out = 0;
        for (uint32_t i = 0; i < n; ++i) {
            const uint8_t in = rw == 0 ? 0 : (uint8_t)(c.data >> (24 - 8 * i));
            out |= (uint32_t)xfer(in) << (24 - 8 * i);
        }
        if (rw != 1)
            c.data = out;
    }
    static const bool trace = std::getenv("NWII_EXITRACE") != nullptr;
    static int traced = 0;
    if (trace && ch == 0 && c.selected == 0 && traced++ < 400)
        std::cout << "[EXI] ch0 dev0 " << ((c.cr & CR_DMA) ? "DMA" : "IMM")
                  << " rw=" << rw << " len=" << std::dec
                  << ((c.cr & CR_DMA) ? (c.len & 0x03FFFFE0u) : ((c.cr >> 4) & 3) + 1)
                  << " data=0x" << std::hex << c.data << std::dec << "\n";

    // Transfers complete instantly; EXISync polls TSTART.
    c.cr &= ~CR_TSTART;
    c.csr |= CSR_TCINT;
    update_interrupt();
}

// OS interrupt numbers per channel (EXI, TC, EXT): __OS_INTERRUPT_EXI_0_EXI = 9
// onwards, three per channel, with channel 2 having no EXT line. Like
// dsp_pending_os_interrupt, this lets the PI fallback dispatch the real cause
// instead of always EXI_0_EXI, whose handler never acks a transfer-complete.
int exi_pending_os_interrupt() {
    for (int ch = 0; ch < 3; ++ch) {
        const uint32_t s = exi[ch].csr;
        if ((s & CSR_EXIINT) && (s & CSR_EXIINTMASK)) return 9 + ch * 3;
        if ((s & CSR_TCINT) && (s & CSR_TCINTMASK)) return 10 + ch * 3;
        if (ch < 2 && (s & CSR_EXTINT) && (s & CSR_EXTINTMASK)) return 11 + ch * 3;
    }
    return 9;
}

void register_exi(MMIODispatcher &dispatcher) {
    dispatcher.register_region(0xCC006800, 0xCC00683F,
        [](uint32_t addr) -> uint32_t {
            ensure_devices();
            const uint32_t rel = addr - 0xCC006800;
            const int ch = rel / 0x14;
            if (ch > 2)
                return 0;
            const ExiChannel &c = exi[ch];
            switch (rel % 0x14) {
            case 0x00: // EXT: a card sits in slot A (channel 0)
                return c.csr | (devices[ch][0] && ch < 2 ? CSR_EXT : 0);
            case 0x04: return c.mar;
            case 0x08: return c.len;
            case 0x0C: return c.cr;
            case 0x10: return c.data;
            }
            return 0;
        },
        [](uint32_t addr, uint32_t val) {
            ensure_devices();
            const uint32_t rel = addr - 0xCC006800;
            const int ch = rel / 0x14;
            if (ch > 2)
                return;
            ExiChannel &c = exi[ch];
            switch (rel % 0x14) {
            case 0x00: {
                // Interrupt status bits are write-one-to-clear; EXT is
                // read-only.
                const uint32_t keep = c.csr & CSR_INT_BITS & ~(val & CSR_INT_BITS);
                c.csr = (val & ~(CSR_INT_BITS | CSR_EXT)) | keep;
                // Chip select: bits 7..9, one per device.
                const uint32_t cs = (c.csr >> 7) & 7;
                const int sel = (cs & 1) ? 0 : (cs & 2) ? 1 : (cs & 4) ? 2 : -1;
                if (sel != c.selected) {
                    static const bool trace = std::getenv("NWII_EXITRACE") != nullptr;
                    static int traced = 0;
                    if (trace && ch == 0 && (sel == 0 || c.selected == 0) && traced++ < 200)
                        std::cout << "[EXI] ch0 cs " << c.selected << " -> " << sel
                                  << " csr=0x" << std::hex << c.csr << std::dec << "\n";
                    if (ExiDevice *old = device_of(ch)) old->deselect();
                    c.selected = sel;
                    if (ExiDevice *now = device_of(ch)) now->select();
                }
                update_interrupt();
                break;
            }
            case 0x04: c.mar = val; break;
            case 0x08: c.len = val; break;
            case 0x0C:
                c.cr = val;
                if (val & CR_TSTART)
                    execute(ch);
                break;
            case 0x10: c.data = val; break;
            }
        });
}

}
