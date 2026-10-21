#pragma once

#include <string>
#include <vector>
#include <cstdint>
#include <functional>
#include "common/endian.h"

namespace nwii {
namespace loader {

struct DolHeader {
    be32_t text_offsets[7];
    be32_t data_offsets[11];
    be32_t text_addresses[7];
    be32_t data_addresses[11];
    be32_t text_sizes[7];
    be32_t data_sizes[11];
    be32_t bss_address;
    be32_t bss_size;
    be32_t entry_point;
    be32_t padding[7];
};

static_assert(sizeof(DolHeader) == 0x100, "DolHeader must be 256 bytes");

struct Section {
    uint32_t address;
    uint32_t size;
    std::vector<uint8_t> data;
    bool is_text;
    bool is_bss;
};

class Executable {
public:
    uint32_t entry_point = 0;
    std::vector<Section> sections;

    bool load_dol(const std::string& path);
    // DOL read through `read(offset in the DOL, dst, len)` (e.g. from a disc image).
    bool load_dol_from(const std::function<bool(uint64_t, void*, size_t)>& read);
    bool load_unpacked_game(const std::string& directory_path);

    

    

    bool load_rel(const std::string& path, uint32_t module_base,
                  uint32_t bss_base);
};

} 
} 
