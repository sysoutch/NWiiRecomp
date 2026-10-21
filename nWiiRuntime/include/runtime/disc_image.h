#pragma once
#include <cstdint>
#include <memory>
#include <string>

namespace nwii::runtime {

// Random access to a GameCube disc image file: raw (.iso/.gcm) or RVZ
// (Dolphin's compressed format; zstd or stored chunks).
class DiscImage {
public:
    virtual ~DiscImage() = default;

    // nullptr when the file is not a supported GameCube disc image; `error`
    // (optional) then says why, in words for the user.
    static std::unique_ptr<DiscImage> open(const std::string &path, std::string *error = nullptr);

    virtual uint64_t size() const = 0; // uncompressed disc size
    // len bytes at offset; bytes past the end of the data read as zero.
    // False on an I/O or decompression error. Thread-safe.
    virtual bool read(uint64_t offset, uint32_t len, uint8_t *dst) = 0;
};

} // namespace nwii::runtime
