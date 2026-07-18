// bitforge - bit_span.h
// The atomic layer: view and edit individual bits inside a byte buffer.
//
// Bit numbering is MSB-first within each byte -- bit 0 is 0x80, bit 7 is 0x01 --
// so a byte reads left-to-right in the grid exactly the way you'd write it on
// paper (0b10110001). That 1:1 match with the visual is worth more here than
// matching the CPU's LSB-first BT/BTS/BTR/BTC index convention.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstring>
#if defined(_MSC_VER)
#  include <intrin.h>
#endif

namespace bf {

inline int  get_bit(const uint8_t* p, uint64_t bit) {
    return (p[bit >> 3] >> (7 - (bit & 7))) & 1;
}
inline void set_bit(uint8_t* p, uint64_t bit, int v) {
    const uint8_t m = (uint8_t)(1u << (7 - (bit & 7)));
    if (v) p[bit >> 3] |= m; else p[bit >> 3] &= (uint8_t)~m;
}
inline void toggle_bit(uint8_t* p, uint64_t bit) {
    p[bit >> 3] ^= (uint8_t)(1u << (7 - (bit & 7)));
}

// Hardware note: the single-instruction forms are _bittestandcomplement & co.
// (BT/BTS/BTR/BTC), but they index LSB-first over 32-bit words. We keep the
// MSB-first byte-mask form for a correct match with the renderer; the intrinsic
// path is the drop-in for bulk internal work where index convention is hidden.

inline uint32_t popcount8(uint8_t v) {
#if defined(_MSC_VER)
    return (uint32_t)__popcnt(v);
#else
    return (uint32_t)__builtin_popcount(v);
#endif
}

// Population count over a buffer -- the "how many 1-bits" / density primitive.
inline uint64_t popcount_buf(const uint8_t* p, size_t n) {
    uint64_t c = 0; size_t i = 0;
#if defined(_MSC_VER) && defined(_M_X64)
    for (; i + 8 <= n; i += 8) { uint64_t w; std::memcpy(&w, p + i, 8); c += __popcnt64(w); }
#endif
    for (; i < n; ++i) c += popcount8(p[i]);
    return c;
}

// XOR two equal-length buffers into `out`; returns the count of differing BITS.
// This is the heart of the live "which exact bits changed" diff/heatmap view.
inline uint64_t bit_diff(const uint8_t* a, const uint8_t* b, uint8_t* out, size_t n) {
    uint64_t changed = 0;
    for (size_t i = 0; i < n; ++i) { uint8_t x = (uint8_t)(a[i] ^ b[i]); out[i] = x; changed += popcount8(x); }
    return changed;
}

// Extract `nbits` (<=64) starting at absolute bit `gbit` from a buffer, MSB-first,
// into the low bits of the result. The core of unaligned bitfield search.
inline uint64_t extract_bits(const uint8_t* buf, uint64_t gbit, int nbits) {
    uint64_t v = 0;
    for (int k = 0; k < nbits; ++k)
        v = (v << 1) | (uint64_t)((buf[(gbit + k) >> 3] >> (7 - ((gbit + k) & 7))) & 1);
    return v;
}

} // namespace bf
