#pragma once
// NalAnimCodec.h - C++ port of pcanim_codec.py
// Full NAL entropy decoder with all 64 codecs + per-component frame integrators
// LemonHaze / Claude - 2025

#include <cstdint>
#include <cmath>
#include <cstring>
#include <vector>
#include <functional>

// ─── Component IDs (match pcanim_codec.py) ───
namespace NalComp {
    constexpr int ARBITRARY_PO  = 0;
    constexpr int GENERIC       = 1;
    constexpr int FAKEROOT_STD  = 2;
    constexpr int TORSO_HEAD    = 3;
    constexpr int TORSO_HEAD_STD = 4;
    constexpr int LEGS          = 5;
    constexpr int LEGS_IK       = 6;
    constexpr int ARMS          = 7;
    constexpr int ARMS_IK       = 8;
    constexpr int TENTACLE      = 9;
    constexpr int FING52        = 10;
    constexpr int FING5_CURL    = 11;
    constexpr int FING5_REDUCED = 12;
    constexpr int FING5         = 13;
}

// ─── Constants ───
constexpr float ENTROPY_BASE_QUANT_STEP = 0.25f;
constexpr float DEQUANT_SCALE = 0.0009765625f; // 1/1024
constexpr int INITIAL_VALUES_BIT_TABLE[] = {2, 4, 7, 20};
constexpr int SCENE_INITIAL_VALUES_BIT_TABLE[] = {4, 7, 12, 30};

// ─── Helpers ───
static inline int nal_popcount(uint32_t v) { return __builtin_popcount(v); }

static inline int nal_count(uint32_t mask, uint32_t filt, int weight = 1) {
    return weight * nal_popcount(mask & filt);
}

// ─── Track count / byte size per component (matches pcanim_codec.py) ───
static inline int nal_get_num_tracks_for_comp(int comp_ix, uint32_t mask) {
    switch (comp_ix) {
    case NalComp::ARBITRARY_PO: return 3 * nal_popcount(mask & 0xFFFF);
    case NalComp::GENERIC:      return 0;
    case NalComp::FAKEROOT_STD: { int t = 9; if (mask & 1) t += 6; if (mask & 2) t += 1; return t; }
    case NalComp::TORSO_HEAD:
    case NalComp::TORSO_HEAD_STD: return nal_count(mask, 0x1F, 3) + nal_count(mask, 0x20, 6);
    case NalComp::LEGS:
    case NalComp::ARMS:           return nal_count(mask, 0xFF, 3);
    case NalComp::LEGS_IK:
    case NalComp::ARMS_IK: {
        int t = 0;
        if (mask & 1) t += 3; if (mask & 2) t += 3;
        if (mask & 4) t += 7; if (mask & 8) t += 7;
        return t;
    }
    case NalComp::TENTACLE:      return nal_popcount(mask & 0x7FFF);
    case NalComp::FING52:
    case NalComp::FING5_REDUCED: return nal_popcount(mask & 0x3FFFFFFF) + nal_popcount(mask & 0x3FF) + nal_popcount(mask & 0x3);
    case NalComp::FING5_CURL:    return 15 + nal_count(mask, 0x3FF, 2) + nal_count(mask, 0x3, 2);
    case NalComp::FING5:         return 61 + nal_count(mask, 0x3FFFFFFF, 3);
    default: return 3 * nal_popcount(mask & 0x1F) + ((mask & 0x20) ? 6 : 0);
    }
}

static inline int nal_get_num_bytes_for_comp(int comp_ix, uint32_t mask) {
    auto to_bytes = [](int tracks, int hdr = 0) { return tracks * 16 + hdr; };
    switch (comp_ix) {
    case NalComp::ARBITRARY_PO: return to_bytes(nal_get_num_tracks_for_comp(comp_ix, mask));
    case NalComp::GENERIC:      return 0;
    case NalComp::FAKEROOT_STD: return to_bytes(9 + nal_count(mask, 0x1, 6) + nal_count(mask, 0x2, 1));
    case NalComp::TORSO_HEAD:
    case NalComp::TORSO_HEAD_STD: return to_bytes(nal_count(mask, 0x1F, 3) + nal_count(mask, 0x20, 6));
    case NalComp::LEGS:
    case NalComp::ARMS:           return to_bytes(17 + nal_count(mask, 0xFF, 3));
    case NalComp::LEGS_IK:
    case NalComp::ARMS_IK:       return to_bytes(nal_count(mask, 0xF, 3) + nal_count(mask, 0xC, 4));
    case NalComp::TENTACLE:      return to_bytes(nal_popcount(mask & 0x7FFF), 136);
    case NalComp::FING52:
    case NalComp::FING5_REDUCED: {
        int t = nal_popcount(mask & 0x3FFFFFFF) + nal_popcount(mask & 0x3FF) + nal_popcount(mask & 0x3);
        return to_bytes(t);
    }
    case NalComp::FING5_CURL:    return to_bytes(15 + nal_count(mask, 0x3FF, 2) + nal_count(mask, 0x3, 2));
    case NalComp::FING5:         return to_bytes(61 + nal_count(mask, 0x3FFFFFFF, 3));
    default: return -1;
    }
}

static inline bool nal_has_track(int flags) {
    return (flags & 0x3) == 0x3; // HAS_TRACK_DATA | HAS_PER_ANIM_DATA
}

// ─── BitStream ───
class NalBitStream {
    const uint8_t* data_;
    size_t size_;
public:
    int bitpos = 0;

    NalBitStream(const uint8_t* d, size_t sz) : data_(d), size_(sz) {}

    int get_bit(int idx) const {
        if (idx < 0) return 0;
        int byte_idx = idx >> 3;
        if ((size_t)byte_idx >= size_) return 0;
        return (data_[byte_idx] >> (idx & 7)) & 1;
    }

    uint32_t peek_bits(int n) const {
        uint32_t out = 0;
        for (int i = 0; i < n; ++i) out |= (uint32_t)get_bit(bitpos + i) << i;
        return out;
    }

    uint32_t read_bits(int n) {
        uint32_t out = peek_bits(n);
        bitpos += n;
        return out;
    }

    void consume(int n) { bitpos += n; }

    int32_t read_signed_bits(int n) {
        uint32_t raw = read_bits(1 + n);
        bool neg = (raw & 1) != 0;
        int32_t value = (int32_t)(raw >> 1);
        return neg ? -value : value;
    }
};

// ─── Decoder result: (run_length, decoded_value) ───
struct DecResult { int run; int val; };

// ─── All 64 entropy decoders (exact port of pcanim_codec.py) ───
static inline DecResult dec_0(NalBitStream&) { return {0, 0}; }

static inline DecResult dec_1a(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(2);
    if (c & 1) { bs.consume(2); return {1, (int)c - 2}; }
    bs.consume(1); return {1, 0};
}
static inline DecResult dec_1b(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(4);
    if (c & 1) {
        if (c & 2) { if (c & 4) { bs.consume(4); return {1, (int)(c >> 2) - 2}; } bs.consume(3); return {1, 0}; }
        bs.consume(2); return {2, 0};
    }
    bs.consume(1); return {7, 0};
}
static inline DecResult dec_1c(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(3);
    if (c & 1) { if (c & 2) { bs.consume(3); return {1, (int)(c >> 1) - 2}; } bs.consume(2); return {1, 0}; }
    bs.consume(1); return {3, 0};
}
static inline DecResult dec_1d(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(4);
    if (c & 1) {
        if (c & 2) { if (c & 4) { bs.consume(4); return {1, (int)(c >> 2) - 2}; } bs.consume(3); return {1, 0}; }
        bs.consume(2); return {2, 0};
    }
    bs.consume(1); return {4, 0};
}
static inline DecResult dec_1e(NalBitStream& bs) {
    uint32_t c = bs.read_bits(2);
    if (c != 0) return {1, (int)c - 2};
    return {6, 0};
}
static inline DecResult dec_2a(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(3);
    if (c & 1) { bs.consume(3); return {1, (int)(c >> 2) + (int)(c >> 1) - 2}; }
    bs.consume(1); return {1, 0};
}
static inline DecResult dec_2b(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(3);
    if ((c & 3) != 0) { int v = c & 3; bs.consume(2); return {1, v - 2}; }
    bs.consume(3); return {1, (int)(c & 3) - 2};
}
static inline DecResult dec_2c(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(5);
    if (c & 1) {
        if (c & 2) { if (c & 4) { bs.consume(5); return {1, (int)(c >> 4) + (int)(c >> 3) - 2}; } bs.consume(3); return {1, 0}; }
        bs.consume(2); return {2, 0};
    }
    bs.consume(1); return {4, 0};
}
static inline DecResult dec_3a(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(4);
    if ((c & 3) != 0) { bs.consume(2); return {1, (int)(c & 3) - 2}; }
    int tmp = (int)(c >> 2); if ((tmp & 2) == 0) tmp -= 3;
    bs.consume(4); return {1, tmp};
}
static inline DecResult dec_3b(NalBitStream& bs) {
    uint32_t c = bs.read_bits(3);
    if (c != 0) return {1, (int)c - 4};
    return {3, 0};
}
static inline DecResult dec_5a(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(5);
    if ((c & 3) != 0) { bs.consume(2); return {1, (int)(c & 3) - 2}; }
    int tmp = (int)(c >> 2); if (tmp & 4) tmp -= 2; else tmp -= 5;
    bs.consume(5); return {1, tmp};
}
static inline DecResult dec_7a(NalBitStream& bs) {
    uint32_t c = bs.read_bits(4);
    if (c != 0) return {1, (int)c - 8};
    return {4, 0};
}
static inline DecResult dec_7b(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(5);
    int low3 = c & 7;
    if (low3 < 3) {
        if (low3 == 2) { int v = (c & 8) ? 3 : -3; bs.consume(4); return {1, v}; }
        int v = (c & 1) ? -(int)(4 + (c >> 3)) : (int)((c >> 3) + 4);
        bs.consume(5); return {1, v};
    }
    bs.consume(3); return {1, low3 - 5};
}
static inline DecResult dec_7c(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(6);
    if (c & 3) { bs.consume(2); return {1, (int)(c & 3) - 2}; }
    if (c & 4) { int tmp = (int)(c >> 3); if ((tmp & 4) == 0) tmp -= 7; bs.consume(6); return {1, tmp}; }
    int tmp = (int)((c >> 3) & 3); if ((tmp & 2) == 0) tmp -= 3; bs.consume(5); return {1, tmp};
}
static inline DecResult dec_f15a(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(5);
    if (c & 1) {
        int half = (int)(c >> 1), quarter = (int)(c >> 2), lf = half & 1;
        if ((quarter & 4) == 0) quarter -= 7;
        bs.consume(5); return {1, quarter << lf};
    }
    int low4 = c & 0xF; bs.consume(4);
    if (low4 != 0) return {1, (int)(low4 >> 1) - 4};
    return {4, 0};
}
static inline DecResult dec_f15b(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(7);
    if (c & 3) { bs.consume(2); return {1, (int)(c & 3) - 2}; }
    if (c & 4) {
        int half = (int)(c >> 3), high = (int)(c >> 4), lf = half & 1;
        if ((high & 4) == 0) high -= 7;
        bs.consume(7); return {1, high << lf};
    }
    int tmp = (int)((c >> 3) & 3); if ((tmp & 2) == 0) tmp -= 3; bs.consume(5); return {1, tmp};
}
static inline DecResult dec_f15c(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(7);
    if (c & 1) {
        if (c & 2) {
            if (c & 4) { int half = (int)(c >> 3), high = (int)(c >> 4), lf = half & 1; if ((high & 4) == 0) high -= 7; bs.consume(7); return {1, high << lf}; }
            int tmp = (int)((c >> 3) & 3); if ((tmp & 2) == 0) tmp -= 3; bs.consume(5); return {1, tmp};
        }
        bs.consume(3); return {1, (int)((c >> 1) & 2) - 1};
    }
    bs.consume(2); if ((c & 2) == 0) return {8, 0}; return {1, 0};
}
static inline DecResult dec_f31a(NalBitStream& bs) {
    uint32_t c = bs.read_bits(5);
    if (c == 0) return {5, 0};
    int low2 = c & 3, high = (int)(c >> 2);
    if (low2 != 0) { int shift = low2 - 1; if ((high & 4) == 0) high -= 7; return {1, high << shift}; }
    return {1, high - 4};
}
static inline DecResult dec_f31b(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(6);
    if (c & 1) {
        int shift, high;
        if (c & 2) { shift = ((c >> 2) & 1) + 1; high = (int)(c >> 3); bs.consume(6); }
        else { shift = 0; high = (int)((c >> 2) & 7); bs.consume(5); }
        if ((high & 4) == 0) high -= 7;
        return {1, high << shift};
    }
    int low4 = c & 0xF; bs.consume(4);
    if (low4 != 0) return {1, (int)(low4 >> 1) - 4};
    return {4, 0};
}
static inline DecResult dec_f31c(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(7);
    int low3 = c & 7;
    if (low3 < 3) {
        if ((c & 7) != 0) {
            int shift, high;
            if (low3 == 1) { shift = 0; high = (int)((c >> 3) & 7); bs.consume(6); }
            else { shift = ((c >> 3) & 1) + 1; high = (int)(c >> 4); bs.consume(7); }
            if ((high & 4) == 0) high -= 7; return {1, high << shift};
        }
        int v = (c & 8) ? 3 : -3; bs.consume(4); return {1, v};
    }
    bs.consume(3); return {1, low3 - 5};
}
static inline DecResult dec_f31d(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(7);
    if (c & 1) {
        if (c & 2) {
            if (c & 0xC) { int shift = ((c >> 2) & 3) - 1, high = (int)(c >> 4); if ((high & 4) == 0) high -= 7; bs.consume(7); return {1, high << shift}; }
            int high = (int)((c >> 4) & 3); if ((high & 2) == 0) high -= 3; bs.consume(6); return {1, high};
        }
        bs.consume(3); return {1, (int)((c >> 1) & 2) - 1};
    }
    bs.consume(2); if ((c & 2) == 0) return {8, 0}; return {1, 0};
}
static inline DecResult dec_f63a(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(6);
    if (c & 1) { int half = (int)(c >> 1), high = (int)(c >> 3), shift = half & 3; if ((high & 4) == 0) high -= 7; bs.consume(6); return {1, high << shift}; }
    int low4 = c & 0xF; bs.consume(4);
    if (low4 != 0) return {1, (int)(low4 >> 1) - 4}; return {4, 0};
}
static inline DecResult dec_f63b(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(6);
    if (c & 1) {
        int shift, high;
        if (c & 2) { shift = ((c >> 2) & 1) + 2; high = (int)(c >> 3); bs.consume(6); }
        else { high = (int)((c >> 2) & 7); shift = 1; bs.consume(5); }
        if ((high & 4) == 0) high -= 7; return {1, high << shift};
    }
    int low5 = c & 0x1F; bs.consume(5);
    if (low5 != 0) return {1, (int)(low5 >> 1) - 8}; return {5, 0};
}
static inline DecResult dec_f127(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(6);
    if (c & 1) { int low2 = ((c >> 1) & 3), high = (int)(c >> 3), shift = low2 + 1; if ((high & 4) == 0) high -= 7; bs.consume(6); return {1, high << shift}; }
    int low5 = c & 0x1F; bs.consume(5);
    if (low5 != 0) return {1, (int)(low5 >> 1) - 8}; return {5, 0};
}
static inline DecResult dec_f255(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(6);
    int low3 = c & 7;
    if (low3 >= 2) { int high = (int)(c >> 3), shift = low3 - 2; if ((high & 4) == 0) high -= 7; bs.consume(6); return {1, high << shift}; }
    int low5 = c & 0x1F; bs.consume(5);
    if (low5 != 0) { if (low5 & 1) return {1, (int)(low5 >> 3)}; return {1, -(int)(low5 >> 3)}; }
    return {5, 0};
}
static inline DecResult dec_f2047(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(7);
    if (c & 1) { int low3 = ((c >> 1) & 7), high = (int)(c >> 4), shift = low3 + 1; if ((high & 4) == 0) high -= 7; bs.consume(7); return {1, high << shift}; }
    int low5 = c & 0x1F; bs.consume(5);
    if (low5 != 0) return {1, (int)(low5 >> 1) - 8}; return {5, 0};
}
static inline DecResult dec_f15bit(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(9);
    int low3 = c & 7;
    if (low3 >= 2) {
        int high, shift;
        if (low3 == 7) { shift = ((c >> 3) & 7) + 5; high = (int)(c >> 6); bs.consume(9); }
        else { high = (int)((c >> 3) & 7); shift = low3 - 2; bs.consume(6); }
        if ((high & 4) == 0) high -= 7; return {1, high << shift};
    }
    bs.consume(5); int low5 = c & 0x1F;
    if (low5 != 0) return {1, (low5 & 1) ? (int)(low5 >> 3) : -(int)(low5 >> 3)};
    return {5, 0};
}
static inline DecResult dec_f23bit(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(10);
    int low3 = c & 7;
    if (low3 >= 2) {
        int high, shift;
        if (low3 == 7) { shift = ((c >> 3) & 0xF) + 5; high = (int)(c >> 7); bs.consume(10); }
        else { high = (int)((c >> 3) & 7); shift = low3 - 2; bs.consume(6); }
        if ((high & 4) == 0) high -= 7; return {1, high << shift};
    }
    bs.consume(5); int low5 = c & 0x1F;
    if (low5 != 0) return {1, (low5 & 1) ? (int)(low5 >> 3) : -(int)(low5 >> 3)};
    return {5, 0};
}
static inline DecResult dec_f31bit(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(8);
    int low5 = c & 0x1F;
    if (low5 == 2) { bs.consume(5); return {1, 0}; }
    if (low5 >= 2) { int high = (int)(c >> 5), shift = low5 - 3; if ((high & 4) == 0) high -= 7; bs.consume(8); return {1, high << shift}; }
    int v; if (c & 1) v = ((c >> 5) & 3) + 1; else v = -1 - ((c >> 5) & 3);
    bs.consume(7); return {1, v};
}
// --- Upper half (indices 32-63: different output mapping) ---
static inline DecResult dec_15(NalBitStream& bs) { uint32_t c = bs.read_bits(5); if (c != 0) return {1, (int)c - 16}; return {5, 0}; }
static inline DecResult dec_0_16(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(7);
    if (c & 1) { if (c & 2) { bs.consume(7); return {1, (int)((c >> 6) + (c >> 2)) - 16}; } bs.consume(2); return {1, 0}; }
    bs.consume(1); return {4, 0};
}
static inline DecResult dec_0_1_17(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(7);
    if (c & 1) {
        if (c & 2) { int tmp = (int)(c >> 2); int v = (tmp & 0x10) ? tmp - 14 : tmp - 17; bs.consume(7); return {1, v}; }
        bs.consume(3); return {1, (int)((c >> 1) & 2) - 1};
    }
    bs.consume(2); if (c & 2) return {1, 0}; return {8, 0};
}
static inline DecResult dec_1_17(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(7);
    if ((c & 3) != 0) { bs.consume(2); return {1, (int)(c & 3) - 2}; }
    bs.consume(7); int tmp = (int)(c >> 2); int v = (tmp & 0x10) ? tmp - 14 : tmp - 17; return {1, v};
}
static inline DecResult dec_31(NalBitStream& bs) { uint32_t c = bs.read_bits(6); if (c != 0) return {1, (int)c - 32}; return {6, 0}; }
static inline DecResult dec_0_1_33(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(8);
    if (c & 1) {
        if (c & 2) { int tmp = (int)(c >> 2); int v = (tmp & 0x20) ? tmp - 30 : tmp - 33; bs.consume(8); return {1, v}; }
        bs.consume(3); return {1, (int)((c >> 1) & 2) - 1};
    }
    bs.consume(2); if (c & 2) return {1, 0}; return {8, 0};
}
static inline DecResult dec_3_35(NalBitStream& bs) {
    uint32_t c = bs.peek_bits(8);
    int low3 = c & 7;
    if (low3 < 3) {
        if (c & 2) { bs.consume(4); return {1, (c & 8) ? 3 : -3}; }
        int v = (c & 1) ? (int)((c >> 3) + 4) : -(int)(4 + (c >> 3));
        bs.consume(8); return {1, v};
    }
    bs.consume(3); return {1, low3 - 5};
}
static inline DecResult dec_63(NalBitStream& bs)  { uint32_t c = bs.read_bits(7);  if (c != 0) return {1, (int)c - 64};  return {7, 0}; }
static inline DecResult dec_127(NalBitStream& bs) { uint32_t c = bs.read_bits(8);  if (c != 0) return {1, (int)c - 128}; return {8, 0}; }
static inline DecResult dec_255(NalBitStream& bs) { uint32_t c = bs.read_bits(9);  if (c != 0) return {1, (int)c - 256}; return {8, 0}; }
static inline DecResult dec_511(NalBitStream& bs) { uint32_t c = bs.read_bits(10); if (c != 0) return {1, (int)c - 512}; return {8, 0}; }
static inline DecResult dec_1023(NalBitStream& bs){ uint32_t c = bs.read_bits(11); if (c != 0) return {1, (int)c - 1024};return {8, 0}; }
static inline DecResult dec_15bit(NalBitStream& bs){ uint32_t c = bs.read_bits(16); if (c != 0) return {1, (int)c - 0x8000}; return {8, 0}; }
static inline DecResult dec_23bit(NalBitStream& bs){ uint32_t c = bs.read_bits(24); if (c != 0) return {1, (int)c - 0x800000}; return {8, 0}; }
static inline DecResult dec_31bit_upper(NalBitStream& bs){
    uint32_t c = bs.read_bits(32);
    if (c == 0x80000000u) return {8, 0};
    int32_t v = (int32_t)c;
    return {1, v};
}
static inline DecResult dec_err(NalBitStream&) { return {0, 0}; }

// ─── Decoder table (64 entries) ───
using DecoderFn = DecResult(*)(NalBitStream&);
static const DecoderFn DECODER_TABLE[64] = {
    dec_0, dec_1a, dec_1b, dec_1c, dec_1d, dec_1e, dec_2a, dec_2b,           // 0-7
    dec_2c, dec_3a, dec_3b, dec_5a, dec_7a, dec_7b, dec_7c, dec_f15a,        // 8-15
    dec_f15b, dec_f15c, dec_f31a, dec_f31b, dec_f31c, dec_f31d, dec_f63a, dec_f63b, // 16-23
    dec_f127, dec_f255, dec_f2047, dec_f15bit, dec_f23bit, dec_f31bit, dec_err, dec_err, // 24-31
    dec_0, dec_1a, dec_1b, dec_1c, dec_1d, dec_1e, dec_2a, dec_2b,           // 32-39
    dec_2c, dec_3a, dec_3b, dec_5a, dec_7a, dec_7b, dec_7c, dec_15,          // 40-47
    dec_0_16, dec_0_1_17, dec_1_17, dec_31, dec_0_1_33, dec_3_35, dec_63, dec_127, // 48-55
    dec_255, dec_511, dec_1023, dec_15bit, dec_23bit, dec_31bit_upper, dec_err, dec_err, // 56-63
};

// ─── Track state ───
struct NalTrackState {
    float whole     = 0.f;
    float delta     = 0.f;
    float sec_delta = 0.f;
    int   zeros     = 0;
};

// ─── Quaternion helpers (for track reconstruction) ───
static inline void nal_quat_compose_xyz(float x, float y, float z, float out[4]) {
    out[0] = x; out[1] = y; out[2] = z;
    out[3] = sqrtf(fabsf(1.f - (x*x + y*y + z*z)));
}
static inline void nal_quat_norm(float q[4]) {
    float len2 = q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3];
    if (len2 <= 0.f) { q[0]=0; q[1]=0; q[2]=0; q[3]=1; return; }
    float inv = 1.f / sqrtf(len2);
    q[0]*=inv; q[1]*=inv; q[2]*=inv; q[3]*=inv;
}
static inline void nal_quat_mul(const float a[4], const float b[4], float out[4]) {
    out[0] = b[3]*a[0] + a[1]*b[2] - a[2]*b[1] + b[0]*a[3];
    out[1] = a[1]*b[3] + a[3]*b[1] - b[2]*a[0] + b[0]*a[2];
    out[2] = a[0]*b[1] - a[1]*b[0] + b[3]*a[2] + b[2]*a[3];
    out[3] = b[3]*a[3] - (a[1]*b[1] + a[0]*b[0] + b[2]*a[2]);
}

// Apply delta quaternion to track xyz values (matches _apply_quat_delta_to_tracks)
static inline void nal_apply_quat_delta(NalTrackState* tracks, int idx) {
    auto& tx = tracks[idx], &ty = tracks[idx+1], &tz = tracks[idx+2];
    float q_base[4], q_delta[4], out[4];
    nal_quat_compose_xyz(tx.whole, ty.whole, tz.whole, q_base);
    nal_quat_compose_xyz(tx.delta, ty.delta, tz.delta, q_delta);
    nal_quat_norm(q_delta);
    nal_quat_mul(q_delta, q_base, out);
    if (out[3] < 0.f) { out[0]=-out[0]; out[1]=-out[1]; out[2]=-out[2]; }
    tx.whole = out[0]; ty.whole = out[1]; tz.whole = out[2];
}

static inline void nal_reconstruct_quat_initial(NalTrackState* tracks, int idx) {
    nal_apply_quat_delta(tracks, idx);
}

static inline void nal_apply_quat_delta_accum(NalTrackState* tracks, int idx) {
    tracks[idx].delta += tracks[idx].sec_delta;
    tracks[idx+1].delta += tracks[idx+1].sec_delta;
    tracks[idx+2].delta += tracks[idx+2].sec_delta;
    nal_apply_quat_delta(tracks, idx);
}

// ─── Dequantize all tracks for one frame (matches _dequant_tracks) ───
static inline void nal_dequant_tracks(NalTrackState* tracks, const uint8_t* codec_ixs, int ntracks,
                                       NalBitStream& dec, int frame, float scaled_quant, bool is_scene_anim)
{
    for (int t = 0; t < ntracks; ++t) {
        auto& track = tracks[t];
        uint8_t cb = codec_ixs[t];
        int mask_idx = cb >> 6;
        int num = cb & 0x3F;

        if (frame == 0) {
            int bits = is_scene_anim ? SCENE_INITIAL_VALUES_BIT_TABLE[3] : INITIAL_VALUES_BIT_TABLE[3];
            int base = dec.read_signed_bits(bits);
            track.zeros = 0;
            track.whole = (float)base * (scaled_quant * ENTROPY_BASE_QUANT_STEP);
            continue;
        }
        if (frame == 1) {
            int bits = is_scene_anim ? SCENE_INITIAL_VALUES_BIT_TABLE[mask_idx] : INITIAL_VALUES_BIT_TABLE[mask_idx];
            int d0 = dec.read_signed_bits(bits);
            track.delta = (float)d0 * scaled_quant;
            continue;
        }
        if (num == 0) { track.sec_delta = 0.f; continue; }
        if (track.zeros == 0) {
            auto decoder_fn = DECODER_TABLE[num];
            auto [runlen, decoded] = decoder_fn(dec);
            track.zeros = runlen - 1;
            track.sec_delta = (float)decoded * scaled_quant;
        } else {
            track.zeros -= 1;
            track.sec_delta = 0.f;
        }
    }
}

// ─── Per-component frame integrators (match pcanim_codec.py) ───
using Integrator = void(*)(NalTrackState*, const uint8_t*, uint32_t, int, NalBitStream&, float, bool);

static void integrate_torso(NalTrackState* t, const uint8_t* c, uint32_t mask, int frame, NalBitStream& dec, float ts, bool sa) {
    float sq = DEQUANT_SCALE * ts;
    int ntracks = nal_count(mask, 0x1F, 3) + nal_count(mask, 0x20, 6);
    nal_dequant_tracks(t, c, ntracks, dec, frame, sq, sa);
    if (frame == 0) return;
    if (frame == 1) {
        int di = 0;
        for (int i = 0; i < 5; ++i) { if (mask & (1 << i)) { nal_reconstruct_quat_initial(t, di); di += 3; } }
        if (mask & 0x20) { nal_reconstruct_quat_initial(t, di); int ti = di + 3; t[ti].whole += t[ti].delta; t[ti+1].whole += t[ti+1].delta; t[ti+2].whole += t[ti+2].delta; }
        return;
    }
    int ti = 0;
    for (int i = 0; i < 5; ++i) { if (mask & (1 << i)) { nal_apply_quat_delta_accum(t, ti); ti += 3; } }
    if (mask & 0x20) {
        nal_apply_quat_delta_accum(t, ti);
        int tmp = ti + 3;
        for (int e = 0; e < 3; ++e) { float d = t[tmp+e].sec_delta + t[tmp+e].delta; t[tmp+e].delta = d; t[tmp+e].whole += d; }
    }
}

static void integrate_quat_masked(NalTrackState* t, const uint8_t* c, uint32_t mask, int frame, NalBitStream& dec, float ts, bool sa, int bit_count = 8) {
    float sq = DEQUANT_SCALE * ts;
    int ntracks = nal_count(mask, (1u << bit_count) - 1, 3);
    nal_dequant_tracks(t, c, ntracks, dec, frame, sq, sa);
    if (frame == 0) return;
    int ti = 0;
    for (int bit = 0; bit < bit_count; ++bit) {
        if ((mask & (1 << bit)) == 0) continue;
        if (frame == 1) nal_reconstruct_quat_initial(t, ti);
        else nal_apply_quat_delta_accum(t, ti);
        ti += 3;
    }
}

static void integrate_fakeroot(NalTrackState* t, const uint8_t* c, uint32_t mask, int frame, NalBitStream& dec, float ts, bool sa) {
    float sq = DEQUANT_SCALE * ts;
    int ntracks = 9 + ((mask & 1) ? 6 : 0) + ((mask & 2) ? 1 : 0);
    nal_dequant_tracks(t, c, ntracks, dec, frame, sq, sa);
    if (frame == 0) return;
    int ti = 0;
    if (mask & 1) {
        if (frame == 1) { nal_reconstruct_quat_initial(t, ti); for (int j = 3; j < 6; ++j) t[ti+j].whole += t[ti+j].delta; }
        else { nal_apply_quat_delta_accum(t, ti); for (int j = 3; j < 6; ++j) { float d = t[ti+j].sec_delta + t[ti+j].delta; t[ti+j].delta = d; t[ti+j].whole += d; } }
        ti = 6;
    }
    if ((mask & 2) && ti < ntracks) {
        if (frame == 1) t[ti].whole += t[ti].delta;
        else { float d = t[ti].sec_delta + t[ti].delta; t[ti].delta = d; t[ti].whole += d; }
    }
}

static void integrate_ik(NalTrackState* t, const uint8_t* c, uint32_t mask, int frame, NalBitStream& dec, float ts, bool sa) {
    float sq = DEQUANT_SCALE * ts;
    int ntracks = nal_count(mask, 0xF, 3) + nal_count(mask, 0xC, 4);
    nal_dequant_tracks(t, c, ntracks, dec, frame, sq, sa);
    if (frame == 0) return;
    int ti = 0;
    for (int bit = 0; bit < 2; ++bit) {
        if ((mask & (1 << bit)) == 0) continue;
        if (frame == 1) nal_reconstruct_quat_initial(t, ti);
        else nal_apply_quat_delta_accum(t, ti);
        ti += 3;
    }
    for (int bit = 2; bit < 4; ++bit) {
        if ((mask & (1 << bit)) == 0) continue;
        if (frame == 1) { nal_reconstruct_quat_initial(t, ti); for (int j = 3; j < 7; ++j) t[ti+j].whole += t[ti+j].delta; }
        else { nal_apply_quat_delta_accum(t, ti); for (int j = 3; j < 7; ++j) { float d = t[ti+j].sec_delta + t[ti+j].delta; t[ti+j].delta = d; t[ti+j].whole += d; } }
        ti += 7;
    }
}

static void integrate_linear(NalTrackState* t, const uint8_t* c, uint32_t mask, int frame, NalBitStream& dec, float ts, bool sa) {
    float sq = DEQUANT_SCALE * ts;
    // Count all tracks present
    int ntracks = nal_get_num_tracks_for_comp(-1, mask); // generic
    // For linear components, all tracks are just integrated linearly
    // We need to figure out ntracks from the codec_ixs length — use the mask
    // Actually we need to pass ntracks separately. For now use a reasonable upper bound.
    // This gets called for tentacles, fingers, etc.
    // Caller should set ntracks correctly.
    nal_dequant_tracks(t, c, ntracks, dec, frame, sq, sa);
    if (frame == 0) return;
    for (int i = 0; i < ntracks; ++i) {
        if (frame == 1) t[i].whole += t[i].delta;
        else { float d = t[i].sec_delta + t[i].delta; t[i].delta = d; t[i].whole += d; }
    }
}

static void integrate_arbitrary(NalTrackState* t, const uint8_t* c, uint32_t mask, int frame, NalBitStream& dec, float ts, bool sa) {
    float sq = DEQUANT_SCALE * ts;
    int ntracks = 3 * nal_popcount(mask & 0xFFFF);
    nal_dequant_tracks(t, c, ntracks, dec, frame, sq, sa);
    if (frame == 0) return;
    int ti = 0;
    for (int bit = 0; bit < 16; ++bit) {
        if ((mask & (1 << bit)) == 0) continue;
        if (bit < 12) {
            if (frame == 1) nal_reconstruct_quat_initial(t, ti);
            else nal_apply_quat_delta_accum(t, ti);
        } else {
            for (int j = 0; j < 3; ++j) {
                if (frame == 1) t[ti+j].whole += t[ti+j].delta;
                else { float d = t[ti+j].sec_delta + t[ti+j].delta; t[ti+j].delta = d; t[ti+j].whole += d; }
            }
        }
        ti += 3;
    }
}

static void integrate_noop(NalTrackState*, const uint8_t*, uint32_t, int, NalBitStream&, float, bool) {}

static void integrate_torso_head(NalTrackState* t, const uint8_t* c, uint32_t m, int f, NalBitStream& d, float ts, bool sa)   { integrate_torso(t,c,m,f,d,ts,sa); }
static void integrate_legs(NalTrackState* t, const uint8_t* c, uint32_t m, int f, NalBitStream& d, float ts, bool sa)          { integrate_quat_masked(t,c,m,f,d,ts,sa,8); }
static void integrate_arms(NalTrackState* t, const uint8_t* c, uint32_t m, int f, NalBitStream& d, float ts, bool sa)          { integrate_quat_masked(t,c,m,f,d,ts,sa,8); }
static void integrate_legs_ik(NalTrackState* t, const uint8_t* c, uint32_t m, int f, NalBitStream& d, float ts, bool sa)       { integrate_ik(t,c,m,f,d,ts,sa); }
static void integrate_arms_ik(NalTrackState* t, const uint8_t* c, uint32_t m, int f, NalBitStream& d, float ts, bool sa)       { integrate_ik(t,c,m,f,d,ts,sa); }

// Tentacles, fingers: all use linear integration
static void integrate_tentacle(NalTrackState* t, const uint8_t* c, uint32_t m, int f, NalBitStream& d, float ts, bool sa) {
    float sq = DEQUANT_SCALE * ts;
    int nt = nal_popcount(m & 0x7FFF);
    nal_dequant_tracks(t, c, nt, d, f, sq, sa);
    if (f == 0) return;
    for (int i = 0; i < nt; ++i) { if (f == 1) t[i].whole += t[i].delta; else { float dd = t[i].sec_delta + t[i].delta; t[i].delta = dd; t[i].whole += dd; } }
}

static void integrate_fing_linear(NalTrackState* t, const uint8_t* c, uint32_t m, int f, NalBitStream& d, float ts, bool sa) {
    float sq = DEQUANT_SCALE * ts;
    int nt = nal_get_num_tracks_for_comp(NalComp::FING52, m); // works for fing52/reduced
    nal_dequant_tracks(t, c, nt, d, f, sq, sa);
    if (f == 0) return;
    for (int i = 0; i < nt; ++i) { if (f == 1) t[i].whole += t[i].delta; else { float dd = t[i].sec_delta + t[i].delta; t[i].delta = dd; t[i].whole += dd; } }
}

// ─── Integrator dispatch ───
static inline Integrator nal_get_integrator(int comp_ix) {
    switch (comp_ix) {
    case NalComp::ARBITRARY_PO:   return integrate_arbitrary;
    case NalComp::GENERIC:        return integrate_noop;
    case NalComp::FAKEROOT_STD:   return integrate_fakeroot;
    case NalComp::TORSO_HEAD:
    case NalComp::TORSO_HEAD_STD: return integrate_torso_head;
    case NalComp::LEGS:           return integrate_legs;
    case NalComp::LEGS_IK:        return integrate_legs_ik;
    case NalComp::ARMS:           return integrate_arms;
    case NalComp::ARMS_IK:        return integrate_arms_ik;
    case NalComp::TENTACLE:       return integrate_tentacle;
    case NalComp::FING52:
    case NalComp::FING5_CURL:
    case NalComp::FING5_REDUCED:
    case NalComp::FING5:          return integrate_fing_linear;
    default: return integrate_noop;
    }
}

// ─── Decode all frames for a component ───
struct NalDecodedFrames {
    std::vector<std::vector<float>> frames; // [frame_index][track_values]
};

static inline NalDecodedFrames nal_decode_component_frames(
    int comp_ix,
    const std::vector<uint8_t>& codec_ixs,
    const std::vector<uint8_t>& encoded_data,
    uint32_t mask,
    int frame_count,
    float current_time,
    bool is_scene_anim)
{
    NalDecodedFrames result;
    if (frame_count <= 0 || codec_ixs.empty()) return result;

    Integrator integrator = nal_get_integrator(comp_ix);
    int ntracks = (int)codec_ixs.size();
    std::vector<NalTrackState> tracks(ntracks);
    NalBitStream dec(encoded_data.data(), encoded_data.size());

    result.frames.resize(frame_count);
    for (int frame = 0; frame < frame_count; ++frame) {
        integrator(tracks.data(), codec_ixs.data(), mask, frame, dec, current_time, is_scene_anim);

        auto& fv = result.frames[frame];
        fv.resize(ntracks);
        for (int t = 0; t < ntracks; ++t) fv[t] = tracks[t].whole;
    }
    return result;
}
