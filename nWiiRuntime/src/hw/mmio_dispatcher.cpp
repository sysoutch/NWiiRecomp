#include "runtime/hw/mmio_dispatcher.h"
#include "runtime/cpu_context.h"
#include <iostream>
#include <algorithm>
#include <unordered_map>

namespace nwii::runtime {

// Unhandled accesses are reported a few times per address and kind, then
// suppressed (unless NWII_VERBOSE): games poll unmodelled registers -- Spyro
// writes the SI poll register (0xCC006430) every frame -- and a line per
// access, flushed, slowed everything down.
static void report_unhandled(const char *kind, uint32_t addr, uint32_t val, bool has_val) {
    static std::unordered_map<uint64_t, uint32_t> seen;
    const uint32_t n = ++seen[((uint64_t)kind[0] << 40) ^ ((uint64_t)kind[5] << 32) ^ addr];
    if (!g_verbose && n > 4)
        return;
    std::cout << "[MMIO] Unhandled " << kind << " at 0x" << std::hex << addr;
    if (has_val)
        std::cout << " val=0x" << val;
    std::cout << std::dec;
    if (!g_verbose && n == 4)
        std::cout << " (further accesses suppressed; NWII_VERBOSE=1 shows all)";
    std::cout << '\n';
}

void MMIODispatcher::register_region(uint32_t start, uint32_t end, MMIORegionReadCallback read_cb, MMIORegionWriteCallback write_cb) {
    m_regions.push_back({start, end, std::move(read_cb), std::move(write_cb), nullptr, nullptr});
}

void MMIODispatcher::register_region(uint32_t start, uint32_t end, MMIORegionReadCallback read_cb, MMIORegionWriteCallback write_cb,
                                     MMIORegionRead16Callback read16_cb, MMIORegionWrite16Callback write16_cb) {
    m_regions.push_back({start, end, std::move(read_cb), std::move(write_cb),
                         std::move(read16_cb), std::move(write16_cb)});
}

uint32_t MMIODispatcher::read32(uint32_t addr) {
    for (const auto& region : m_regions) {
        if (addr >= region.start_addr && addr <= region.end_addr) {
            if (region.read_cb) return region.read_cb(addr);
            return 0;
        }
    }
    report_unhandled("read32", addr, 0, false);
    return 0;
}

void MMIODispatcher::write32(uint32_t addr, uint32_t val) {
    for (const auto& region : m_regions) {
        if (addr >= region.start_addr && addr <= region.end_addr) {
            if (region.write_cb) {
                region.write_cb(addr, val);
            }
            return;
        }
    }
    report_unhandled("write32", addr, val, true);
}

uint16_t MMIODispatcher::read16(uint32_t addr) {
    for (const auto& region : m_regions) {
        if (addr >= region.start_addr && addr <= region.end_addr) {
            if (region.read16_cb) return region.read16_cb(addr);
            if (region.read_cb) return (uint16_t)region.read_cb(addr);
            return 0;
        }
    }
    report_unhandled("read16", addr, 0, false);
    return 0;
}

void MMIODispatcher::write16(uint32_t addr, uint16_t val) {
    for (const auto& region : m_regions) {
        if (addr >= region.start_addr && addr <= region.end_addr) {
            if (region.write16_cb) region.write16_cb(addr, val);
            else if (region.write_cb) region.write_cb(addr, val);
            return;
        }
    }
    report_unhandled("write16", addr, val, true);
}

void MMIODispatcher::clear() {
    m_regions.clear();
}

}
