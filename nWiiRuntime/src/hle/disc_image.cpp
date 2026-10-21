#include "runtime/disc_image.h"

#include <zstd.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <mutex>
#include <vector>

namespace nwii::runtime {

namespace {

uint32_t be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}
uint64_t be64(const uint8_t *p) { return ((uint64_t)be32(p) << 32) | be32(p + 4); }

// A file opened for 64-bit random reads.
class File {
public:
    explicit File(const std::string &path) : f_(std::fopen(path.c_str(), "rb")) {}
    ~File() {
        if (f_) std::fclose(f_);
    }
    File(const File &) = delete;
    File &operator=(const File &) = delete;
    explicit operator bool() const { return f_ != nullptr; }

    bool read(uint64_t offset, void *dst, size_t len) {
#ifdef _WIN32
        if (_fseeki64(f_, (long long)offset, SEEK_SET) != 0) return false;
#else
        if (fseeko(f_, (off_t)offset, SEEK_SET) != 0) return false;
#endif
        return std::fread(dst, 1, len, f_) == len;
    }
    uint64_t size() {
#ifdef _WIN32
        _fseeki64(f_, 0, SEEK_END);
        return (uint64_t)_ftelli64(f_);
#else
        fseeko(f_, 0, SEEK_END);
        return (uint64_t)ftello(f_);
#endif
    }

private:
    FILE *f_;
};

// --- Raw .iso / .gcm ---------------------------------------------------------
class IsoImage final : public DiscImage {
public:
    explicit IsoImage(const std::string &path) : file_(path), size_(file_ ? file_.size() : 0) {}
    uint64_t size() const override { return size_; }
    bool read(uint64_t offset, uint32_t len, uint8_t *dst) override {
        std::lock_guard<std::mutex> lock(mtx_);
        std::memset(dst, 0, len);
        if (offset >= size_) return true;
        const uint32_t n = (uint32_t)std::min<uint64_t>(len, size_ - offset);
        return file_.read(offset, dst, n);
    }

private:
    File file_;
    uint64_t size_;
    std::mutex mtx_;
};

// --- RVZ ---------------------------------------------------------------------
// Dolphin's format (docs/WiaAndRvz.md): the disc is split into fixed-size
// chunks ("groups"), each stored zstd-compressed and optionally "packed",
// where runs of the disc's pseudo-random padding are replaced by the seed of
// the generator that produced them.

// Dolphin's lagged Fibonacci generator for that padding ("junk") data.
class Lfg {
public:
    static constexpr int kSeedWords = 17, K = 521, J = 32;

    void set_seed(const uint8_t *seed) {
        for (int i = 0; i < kSeedWords; ++i) buf_[i] = be32(seed + i * 4);
        for (int i = kSeedWords; i < K; ++i)
            buf_[i] = (buf_[i - 17] << 23) ^ (buf_[i - 16] >> 9) ^ buf_[i - 1];
        // The output takes bits 18..25 for the second byte; fold that in now.
        for (uint32_t &x : buf_) x = (x & 0xFF00FFFF) | ((x >> 2) & 0x00FF0000);
        pos_ = 0;
        for (int i = 0; i < 4; ++i) forward();
    }
    void skip(size_t n) {
        pos_ += n;
        while (pos_ >= K * 4) {
            forward();
            pos_ -= K * 4;
        }
    }
    void get(uint8_t *out, size_t n) {
        while (n--) {
            *out++ = (uint8_t)(buf_[pos_ / 4] >> (24 - 8 * (pos_ % 4)));
            if (++pos_ == K * 4) {
                forward();
                pos_ = 0;
            }
        }
    }

private:
    void forward() {
        for (int i = 0; i < J; ++i) buf_[i] ^= buf_[i + K - J];
        for (int i = J; i < K; ++i) buf_[i] ^= buf_[i - J];
    }
    uint32_t buf_[K] = {};
    size_t pos_ = 0;
};

class RvzImage final : public DiscImage {
public:
    explicit RvzImage(const std::string &path) : file_(path) {}

    // Reads and checks the headers; on failure returns the reason.
    std::string init() {
        if (!file_) return "The file can't be opened.";
        uint8_t h1[0x48];
        if (!file_.read(0, h1, sizeof(h1))) return "The RVZ file is truncated.";
        size_ = be64(h1 + 0x24);
        const uint32_t h2_size = be32(h1 + 0x0C);
        std::vector<uint8_t> h2(std::max<uint32_t>(h2_size, 0xDC));
        if (h2_size < 0xDC || !file_.read(0x48, h2.data(), h2_size)) return "The RVZ header is damaged.";
        if (be32(&h2[0x00]) != 1) return "That RVZ holds a Wii disc, not a GameCube disc.";
        compression_ = be32(&h2[0x04]);
        if (compression_ != 0 && compression_ != 5)
            return "This RVZ uses a compression other than zstd. Re-convert it in Dolphin "
                   "(Convert File... > RVZ, zstd) or to ISO.";
        chunk_ = be32(&h2[0x0C]);
        if (chunk_ < 0x8000 || chunk_ > (64u << 20)) return "The RVZ chunk size is invalid.";
        std::memcpy(disc_header_, &h2[0x10], sizeof(disc_header_));

        const uint32_t n_raw = be32(&h2[0xB4]), raw_size = be32(&h2[0xC0]);
        const uint32_t n_groups = be32(&h2[0xC4]), groups_size = be32(&h2[0xD0]);
        std::vector<uint8_t> raw, groups;
        if (!table(be64(&h2[0xB8]), raw_size, n_raw * 24ull, raw) ||
            !table(be64(&h2[0xC8]), groups_size, n_groups * 12ull, groups))
            return "The RVZ tables are damaged.";
        for (uint32_t i = 0; i < n_raw; ++i) {
            const uint8_t *e = &raw[i * 24];
            raw_.push_back({be64(e), be64(e + 8), be32(e + 16), be32(e + 20)});
        }
        for (uint32_t i = 0; i < n_groups; ++i) {
            const uint8_t *e = &groups[i * 12];
            groups_.push_back({be32(e), be32(e + 4), be32(e + 8)});
        }
        return {};
    }

    uint64_t size() const override { return size_; }

    bool read(uint64_t offset, uint32_t len, uint8_t *dst) override {
        std::lock_guard<std::mutex> lock(mtx_);
        while (len > 0) {
            uint32_t take = len;
            if (offset < sizeof(disc_header_)) { // first 0x80 bytes: in the header
                take = (uint32_t)std::min<uint64_t>(len, sizeof(disc_header_) - offset);
                std::memcpy(dst, disc_header_ + offset, take);
            } else if (const RawData *r = find_raw(offset)) {
                // Raw data starts on a 32 KiB boundary of the disc.
                const uint64_t base = r->offset - r->offset % 0x8000;
                const uint64_t i = (offset - base) / chunk_;
                const std::vector<uint8_t> *g = group(r->group + (uint32_t)i, base + i * chunk_);
                if (!g) return false;
                const uint64_t within = offset - base - i * chunk_;
                take = (uint32_t)std::min<uint64_t>({len, chunk_ - within, r->offset + r->size - offset});
                std::memcpy(dst, g->data() + within, take);
            } else {
                // Not covered by any entry: zeros up to the next entry.
                uint64_t next = UINT64_MAX;
                for (const RawData &rd : raw_)
                    if (rd.offset > offset) next = std::min(next, rd.offset);
                take = (uint32_t)std::min<uint64_t>(len, next - offset);
                std::memset(dst, 0, take);
            }
            offset += take;
            dst += take;
            len -= take;
        }
        return true;
    }

private:
    struct RawData {
        uint64_t offset, size;
        uint32_t group, n_groups;
    };
    struct Group {
        uint32_t offset4;     // file offset / 4
        uint32_t size;        // stored size; top bit set = compressed
        uint32_t packed_size; // non-zero: the data is packed (junk as seeds)
    };

    // A table: stored zstd-compressed (or as is when uncompressed).
    bool table(uint64_t offset, uint32_t stored, uint64_t want, std::vector<uint8_t> &out) {
        std::vector<uint8_t> in(stored);
        if (!file_.read(offset, in.data(), stored)) return false;
        if (compression_ == 0) {
            out = std::move(in);
        } else {
            out.resize(want);
            const size_t n = ZSTD_decompress(out.data(), out.size(), in.data(), in.size());
            if (ZSTD_isError(n)) return false;
        }
        return out.size() >= want;
    }

    const RawData *find_raw(uint64_t offset) const {
        for (const RawData &r : raw_) {
            const uint64_t base = r.offset - r.offset % 0x8000;
            if (offset >= base && offset < r.offset + r.size && (offset - base) / chunk_ < r.n_groups)
                return &r;
        }
        return nullptr;
    }

    // Group `index` (disc data starting at `disc_offset`), decoded; cached.
    const std::vector<uint8_t> *group(uint32_t index, uint64_t disc_offset) {
        for (auto &c : cache_)
            if (c.index == index) {
                c.stamp = ++clock_;
                return &c.data;
            }
        if (index >= groups_.size()) return nullptr;
        const Group &g = groups_[index];
        std::vector<uint8_t> data(chunk_, 0);
        const uint32_t stored = g.size & 0x7FFFFFFF;
        if (stored) {
            std::vector<uint8_t> in(stored);
            if (!file_.read((uint64_t)g.offset4 << 2, in.data(), stored)) return nullptr;
            std::vector<uint8_t> plain;
            if (g.size & 0x80000000) {
                plain.resize(g.packed_size ? g.packed_size : chunk_);
                const size_t n = ZSTD_decompress(plain.data(), plain.size(), in.data(), in.size());
                if (ZSTD_isError(n)) {
                    std::cout << "[RVZ] group " << index << ": " << ZSTD_getErrorName(n) << std::endl;
                    return nullptr;
                }
                plain.resize(n);
            } else {
                plain = std::move(in);
            }
            if (g.packed_size)
                unpack(plain, disc_offset, data);
            else
                std::memcpy(data.data(), plain.data(), std::min<size_t>(plain.size(), chunk_));
        }
        // Keep the 8 most recently used groups (1 MiB at the usual 128 KiB).
        Cached *slot = nullptr;
        if (cache_.size() < 8) {
            slot = &cache_.emplace_back();
        } else {
            slot = &*std::min_element(cache_.begin(), cache_.end(),
                                      [](const Cached &a, const Cached &b) { return a.stamp < b.stamp; });
        }
        slot->index = index;
        slot->stamp = ++clock_;
        slot->data = std::move(data);
        return &slot->data;
    }

    // Packed data: entries of a 4-byte size, then either that many bytes of
    // data or (top bit set) a 68-byte generator seed for that many junk bytes.
    static void unpack(const std::vector<uint8_t> &in, uint64_t disc_offset, std::vector<uint8_t> &out) {
        size_t p = 0, o = 0;
        Lfg lfg;
        while (p + 4 <= in.size() && o < out.size()) {
            uint32_t n = be32(&in[p]);
            p += 4;
            const bool junk = n & 0x80000000;
            n &= 0x7FFFFFFF;
            const size_t take = std::min<size_t>(n, out.size() - o);
            if (junk) {
                if (p + 68 > in.size()) return;
                lfg.set_seed(&in[p]);
                p += 68;
                lfg.skip((disc_offset + o) % 0x8000);
                lfg.get(&out[o], take);
            } else {
                if (p + n > in.size()) return;
                std::memcpy(&out[o], &in[p], take);
                p += n;
            }
            o += n;
        }
    }

    struct Cached {
        uint32_t index = UINT32_MAX;
        uint64_t stamp = 0;
        std::vector<uint8_t> data;
    };

    File file_;
    uint64_t size_ = 0;
    uint32_t compression_ = 0, chunk_ = 0;
    uint8_t disc_header_[0x80] = {};
    std::vector<RawData> raw_;
    std::vector<Group> groups_;
    std::vector<Cached> cache_;
    uint64_t clock_ = 0;
    std::mutex mtx_;
};

} // namespace

std::unique_ptr<DiscImage> DiscImage::open(const std::string &path, std::string *error) {
    auto fail = [&](const std::string &why) -> std::unique_ptr<DiscImage> {
        if (error) *error = why;
        return nullptr;
    };
    File f(path);
    uint8_t head[0x20] = {};
    if (!f || !f.read(0, head, sizeof(head))) return fail("The disc file can't be read.");
    if (!std::memcmp(head, "RVZ\x01", 4)) {
        auto img = std::make_unique<RvzImage>(path);
        if (std::string why = img->init(); !why.empty()) return fail(why);
        return img;
    }
    if (be32(head + 0x1C) == 0xC2339F3D) return std::make_unique<IsoImage>(path);
    const char *fmt = !std::memcmp(head, "WIA\x01", 4)            ? "WIA"
                      : !std::memcmp(head, "CISO", 4)             ? "CISO"
                      : !std::memcmp(head, "WBFS", 4)             ? "WBFS"
                      : !std::memcmp(head, "\x01\xC0\x0B\xB1", 4) ? "GCZ"
                                                                   : nullptr;
    if (fmt)
        return fail(std::string(fmt) + " images can't be read. Convert it to ISO or RVZ first: "
                                       "in Dolphin, right-click the game > Convert File...");
    if (be32(head + 0x18) == 0x5D1C9EA3) return fail("That is a Wii disc, not a GameCube disc.");
    return fail("That file is not a GameCube disc image.");
}

} // namespace nwii::runtime
