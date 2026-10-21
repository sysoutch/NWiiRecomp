#include "runtime/hw/hw.h"
#include "runtime/cpu_context.h"

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>

namespace nwii::runtime {
    extern MMU* g_mmu;
}

namespace nwii::runtime::hw {

// High-level emulation of the GameCube AX DSP microcode (after Dolphin's
// AXUCode / AXVoice). The CPU-side AX library sends a command list every 5 ms;
// the microcode walks the linked list of parameter blocks (PBs), decodes each
// running voice from ARAM (ADPCM, PCM16, PCM8), resamples it, applies the
// volume envelope and mixer volumes, mixes into 32 kHz main buses and writes
// the stereo frame the audio DMA plays. Crucially it also writes the PBs back:
// a one-shot voice that reaches its end address is marked stopped, which is
// how the game learns a sound finished. Without this the runtime only answered
// the mailbox -- no audio at all, and voices never stopped, so the 64-voice
// pool ran dry, AX stole voices that game channels still held, and the same
// voice was freed twice (a cycle in the free list that hung Spyro's hub).
//
// Main L/R/S only: aux sends (reverb) are not mixed.

static constexpr int kSamplesPerMs = 32;
static constexpr int kMs = 5;
static constexpr int kFrame = kSamplesPerMs * kMs; // 160 samples = 5 ms
static constexpr int kPbWords = 118;               // 0xEC-byte PBs (stride seen in the SDK)

// PB word offsets (GameCube AXPB, this SDK revision). Verified against live
// PBs: the ITD block is 7 words (flag, buffer hi/lo, shifts, target shifts),
// one more than Dolphin's struct, so everything from the updates on sits one
// word later -- e.g. the update buffer at 39/40, ve at 50, ADPCM cur at 61/62.
enum : int {
    PB_NEXT_HI = 0, PB_NEXT_LO = 1, PB_SRC_TYPE = 4, PB_COEF_SELECT = 5,
    PB_MIXER_CONTROL = 6, PB_RUNNING = 7, PB_IS_STREAM = 8,
    PB_MIX_L = 9, PB_MIX_R = 11, PB_MIX_S = 23,           // each: volume, delta
    PB_UPD_NUM = 34, PB_UPD_HI = 39, PB_UPD_LO = 40,
    PB_DPOP_L = 41, PB_DPOP_R = 44, PB_DPOP_S = 47,
    PB_VE_CUR = 50, PB_VE_DELTA = 51,
    PB_LOOPING = 55, PB_FORMAT = 56, PB_LOOP_HI = 57, PB_LOOP_LO = 58,
    PB_END_HI = 59, PB_END_LO = 60, PB_CUR_HI = 61, PB_CUR_LO = 62,
    PB_COEFS = 63, PB_PRED_SCALE = 80, PB_YN1 = 81, PB_YN2 = 82,
    PB_RATIO_HI = 83, PB_RATIO_LO = 84, PB_FRAC = 85, PB_LAST = 86,
    PB_LOOP_PS = 90, PB_LOOP_YN1 = 91, PB_LOOP_YN2 = 92,
};

static int32_t main_l[kFrame], main_r[kFrame], main_s[kFrame];

static uint32_t phys(uint32_t a) { return 0x80000000u | (a & 0x01FFFFFFu); }
static uint16_t rd16(uint32_t a) { return g_mmu->read16(phys(a)); }
static uint32_t rd32(uint32_t a) { return ((uint32_t)rd16(a) << 16) | rd16(a + 2); }

static uint8_t aram8(uint32_t a) {
    const auto &m = g_mmu->mem2;
    return a < m.size() ? m[a] : 0;
}

// --- one voice ------------------------------------------------------------

struct Voice {
    uint16_t w[kPbWords];
    uint32_t cur, loop, end;

    int32_t tail = 0; // last decoded sample, for the de-pop fade below

    int16_t fetch() {
        // De-pop: once a voice has stopped (one-shot end), the remaining
        // samples of this millisecond fade its last sample out instead of
        // dropping to 0 -- the microcode's pop removal does the same. The step
        // from a loud last sample to silence clicked at the end of dialogue
        // lines and sounds. NWII_AX_NODEPOP=1: old behaviour.
        if (w[PB_RUNNING] != 1) {
            static const bool nodepop = std::getenv("NWII_AX_NODEPOP") != nullptr;
            if (nodepop) return 0;
            tail = tail * 7 / 8;
            return (int16_t)tail;
        }
        int16_t s = 0;
        switch (w[PB_FORMAT]) {
        case 0x00: { // 4-bit ADPCM, cur in nibbles; 8-byte frames with a header byte
            if ((cur & 15) == 0) {
                w[PB_PRED_SCALE] = aram8((cur & ~15u) >> 1);
                cur += 2;
            }
            const int ps = w[PB_PRED_SCALE];
            const int scale = 1 << (ps & 0xF);
            const int ci = (ps >> 4) & 7;
            const int32_t c1 = (int16_t)w[PB_COEFS + ci * 2];
            const int32_t c2 = (int16_t)w[PB_COEFS + ci * 2 + 1];
            const uint8_t b = aram8(cur >> 1);
            int nib = (cur & 1) ? (b & 0xF) : (b >> 4);
            if (nib >= 8) nib -= 16;
            const int32_t yn1 = (int16_t)w[PB_YN1], yn2 = (int16_t)w[PB_YN2];
            int32_t v = scale * nib + ((0x400 + c1 * yn1 + c2 * yn2) >> 11);
            v = std::clamp(v, -0x7FFF, 0x7FFF);
            w[PB_YN2] = w[PB_YN1];
            w[PB_YN1] = (uint16_t)v;
            s = (int16_t)v;
            ++cur;
            break;
        }
        case 0x0A: // PCM16, cur in samples
            s = (int16_t)((aram8(cur * 2) << 8) | aram8(cur * 2 + 1));
            w[PB_YN2] = w[PB_YN1];
            w[PB_YN1] = (uint16_t)s;
            ++cur;
            break;
        case 0x19: // PCM8, cur in bytes
            s = (int16_t)(aram8(cur) << 8);
            w[PB_YN2] = w[PB_YN1];
            w[PB_YN1] = (uint16_t)s;
            ++cur;
            break;
        default: {
            static int warned = 0;
            if (warned++ < 4)
                std::cout << "[AX] unknown sample format 0x" << std::hex
                          << w[PB_FORMAT] << std::dec << "\n";
            w[PB_RUNNING] = 0;
            return 0;
        }
        }
        tail = s;
        // End of sample: on hardware the accelerator raises an interrupt the
        // microcode services; this is what that handler does.
        if (cur >= end) {
            if ((cur & ~0x1Fu) == (end & ~0x1Fu))
                cur = loop;
            if (w[PB_LOOPING]) {
                w[PB_PRED_SCALE] = w[PB_LOOP_PS];
                if (!w[PB_IS_STREAM]) {
                    w[PB_YN1] = w[PB_LOOP_YN1];
                    w[PB_YN2] = w[PB_LOOP_YN2];
                }
            } else {
                w[PB_RUNNING] = 0; // one-shot finished: the game frees it
            }
        }
        return s;
    }

    // Resample kSamplesPerMs output samples. last_samples[2..3] are the two
    // input samples the current fractional position sits between.
    void input(int16_t *out) {
        // srcSelect 2 = no sample-rate conversion: one input sample per
        // output sample, the ratio is ignored. Spyro's music and dialogue
        // streams leave the ratio at 0 in that mode; stepping by it froze
        // them (no music, silent Sparx).
        const bool nearest = w[PB_SRC_TYPE] == 2;
        uint32_t ratio = nearest ? 0x10000u
                                 : ((uint32_t)w[PB_RATIO_HI] << 16) | w[PB_RATIO_LO];
        uint32_t pos = w[PB_FRAC];
        int16_t h[4];
        for (int i = 0; i < 4; ++i) h[i] = (int16_t)w[PB_LAST + i];
        for (int i = 0; i < kSamplesPerMs; ++i) {
            if (nearest)
                out[i] = h[3];
            else
                out[i] = (int16_t)(h[2] + (((int32_t)h[3] - h[2]) * (int32_t)pos >> 16));
            pos += ratio;
            while (pos >= 0x10000) {
                pos -= 0x10000;
                h[0] = h[1]; h[1] = h[2]; h[2] = h[3];
                h[3] = fetch();
            }
        }
        w[PB_FRAC] = (uint16_t)pos;
        for (int i = 0; i < 4; ++i) w[PB_LAST + i] = (uint16_t)h[i];
    }

    void mix(int32_t *bus, const int16_t *in, int vol_word, int dpop_word, bool ramp) {
        uint32_t vol = w[vol_word];
        const int32_t delta = ramp ? (int16_t)w[vol_word + 1] : 0;
        for (int i = 0; i < kSamplesPerMs; ++i) {
            int64_t s = ((int64_t)in[i] * vol) >> 15;
            s = std::clamp<int64_t>(s, -32767, 32767);
            bus[i] += (int32_t)s;
            vol += delta;
            w[dpop_word] = (uint16_t)(int16_t)s;
        }
        w[vol_word] = (uint16_t)vol;
    }

    void process_ms(int ms) {
        if (w[PB_RUNNING] != 1)
            return;
        int16_t s[kSamplesPerMs];
        input(s);
        uint16_t ve = w[PB_VE_CUR];
        const int16_t ved = (int16_t)w[PB_VE_DELTA];
        for (int i = 0; i < kSamplesPerMs; ++i) {
            s[i] = (int16_t)std::clamp(((int32_t)s[i] * ve) >> 15, -32767, 32767);
            ve = (uint16_t)(ve + ved);
        }
        w[PB_VE_CUR] = ve;
        const uint16_t mc = w[PB_MIXER_CONTROL];
        const bool ramp = mc & 0x0008;
        const int o = ms * kSamplesPerMs;
        mix(main_l + o, s, PB_MIX_L, PB_DPOP_L, ramp);
        mix(main_r + o, s, PB_MIX_R, PB_DPOP_R, ramp);
        if (mc & 0x0004)
            mix(main_s + o, s, PB_MIX_S, PB_DPOP_S, ramp);
    }
};

static void process_pbs(uint32_t pb_addr) {
    static const bool trace = std::getenv("NWII_AXTRACE") != nullptr;
    int guard = 0, running = 0;
    while (pb_addr && guard++ < 256) {
        Voice v;
        for (int i = 0; i < kPbWords; ++i) v.w[i] = rd16(pb_addr + i * 2);
        v.cur = ((uint32_t)v.w[PB_CUR_HI] << 16) | v.w[PB_CUR_LO];
        v.loop = ((uint32_t)v.w[PB_LOOP_HI] << 16) | v.w[PB_LOOP_LO];
        v.end = ((uint32_t)v.w[PB_END_HI] << 16) | v.w[PB_END_LO];
        const uint32_t upd = ((uint32_t)v.w[PB_UPD_HI] << 16) | v.w[PB_UPD_LO];
        uint32_t upd_idx = 0;
        if (v.w[PB_RUNNING] == 1) ++running;
        // NWII_AXVOICES=1: once a second, one line per running voice (format,
        // flags, ARAM addresses, mixer volumes words 9-26 incl. aux A/B).
        static const bool voices = std::getenv("NWII_AXVOICES") != nullptr;
        static uint64_t ax_frames = 0;
        if (voices && v.w[PB_RUNNING] == 1 && (ax_frames % 200) == 0) {
            std::printf("[AXV] f=%llu pb=%08X src=%d fmt=%X loop=%d strm=%d mc=%04X cur=%X loop=%X end=%X ve=%04X ratio=%04X%04X mix:",
                        (unsigned long long)ax_frames, pb_addr, v.w[PB_SRC_TYPE], v.w[PB_FORMAT], v.w[PB_LOOPING],
                        v.w[PB_IS_STREAM], v.w[PB_MIXER_CONTROL], v.cur, v.loop, v.end,
                        v.w[PB_VE_CUR], v.w[PB_RATIO_HI], v.w[PB_RATIO_LO]);
            for (int i = 9; i <= 26; ++i) std::printf(" %04X", v.w[i]);
            std::printf("\n");
        }
        if (guard == 1) ++ax_frames;
        if (trace && v.w[PB_RUNNING] == 1) {
            static int dumped = 0;
            if (dumped++ < 3) {
                std::cout << "[AXpb] @0x" << std::hex << pb_addr << ":";
                for (int i = 0; i < 96; ++i) std::cout << " " << v.w[i];
                std::cout << std::dec << "\n";
            }
        }
        for (int ms = 0; ms < kMs; ++ms) {
            // Parameter updates the CPU queued for this millisecond:
            // (word offset, value) pairs.
            for (int n = 0; n < v.w[PB_UPD_NUM + ms] && upd; ++n, ++upd_idx) {
                const uint16_t off = rd16(upd + upd_idx * 4);
                const uint16_t val = rd16(upd + upd_idx * 4 + 2);
                if (off < kPbWords) v.w[off] = val;
                if (off == PB_CUR_HI || off == PB_CUR_LO)
                    v.cur = ((uint32_t)v.w[PB_CUR_HI] << 16) | v.w[PB_CUR_LO];
                if (off == PB_LOOP_HI || off == PB_LOOP_LO)
                    v.loop = ((uint32_t)v.w[PB_LOOP_HI] << 16) | v.w[PB_LOOP_LO];
                if (off == PB_END_HI || off == PB_END_LO)
                    v.end = ((uint32_t)v.w[PB_END_HI] << 16) | v.w[PB_END_LO];
            }
            v.process_ms(ms);
        }
        v.w[PB_CUR_HI] = (uint16_t)(v.cur >> 16);
        v.w[PB_CUR_LO] = (uint16_t)v.cur;
        for (int i = 0; i < kPbWords; ++i) g_mmu->write16(phys(pb_addr + i * 2), v.w[i]);
        pb_addr = ((uint32_t)v.w[PB_NEXT_HI] << 16) | v.w[PB_NEXT_LO];
    }
    if (trace) {
        static int n = 0;
        if ((n++ % 400) == 0)
            std::cout << "[AX] frame: " << guard << " PBs, " << running << " running\n";
    }
}

// SETUP: initial values (+ per-sample delta) of the nine buses.
static void setup(uint32_t init) {
    int32_t *bufs[3] = {main_l, main_r, main_s};
    for (int b = 0; b < 3; ++b) {
        int32_t v = (int32_t)rd32(init + b * 6);
        const int16_t d = (int16_t)rd16(init + b * 6 + 4);
        for (int i = 0; i < kFrame; ++i) { bufs[b][i] = v; v += d; }
    }
}

// SET_LR: seed L/R from the previous frame's surround output.
static void set_main_lr(uint32_t src) {
    for (int i = 0; i < kFrame; ++i) {
        const int32_t s = (int32_t)rd32(src + i * 4);
        main_l[i] = s; main_r[i] = s; main_s[i] = 0;
    }
}

// OUTPUT: surround bus as s32, then the stereo frame (right, left pairs).
static void output(uint32_t surround, uint32_t lr) {
    for (int i = 0; i < kFrame; ++i) {
        g_mmu->write32(phys(surround + i * 4), (uint32_t)main_s[i]);
        g_mmu->write16(phys(lr + i * 4), (uint16_t)(int16_t)std::clamp(main_r[i], -32767, 32767));
        g_mmu->write16(phys(lr + i * 4 + 2), (uint16_t)(int16_t)std::clamp(main_l[i], -32767, 32767));
    }
}

void ax_process_command_list(uint32_t addr) {
    if (!g_mmu)
        return;
    uint32_t a = addr, pb_list = 0;
    auto arg32 = [&] { const uint32_t v = rd32(a); a += 4; return v; };
    for (int guard = 0; guard < 128; ++guard) {
        const uint16_t cmd = rd16(a);
        a += 2;
        switch (cmd) {
        case 0x00: setup(arg32()); break;
        case 0x01: a += 10; break;            // download + volume mix
        case 0x02: pb_list = arg32(); break;  // PB list head
        case 0x03: process_pbs(pb_list); break;
        case 0x04: case 0x05: a += 10; break; // aux A/B mix (reverb): not modelled
        case 0x06: a += 4; break;             // upload LRS
        case 0x07: set_main_lr(arg32()); break;
        case 0x08: a += 20; break;
        case 0x09: a += 4; break;
        case 0x0A: a += 4; break;             // compressor table
        case 0x0B: case 0x0C: break;
        case 0x0D: { const uint32_t next = arg32(); a = next; break; } // more (size ignored)
        case 0x0E: { const uint32_t s = arg32(); const uint32_t lr = arg32(); output(s, lr); break; }
        case 0x0F: return;                    // end
        case 0x10: a += 8; break;
        case 0x11: a += 4; break;
        case 0x12: a += 2; break;             // compressor on/off
        case 0x13: a += 12; break;
        default: {
            static int warned = 0;
            if (warned++ < 8)
                std::cout << "[AX] unknown command 0x" << std::hex << cmd
                          << " in list @0x" << addr << std::dec << "\n";
            return;
        }
        }
    }
}

}
