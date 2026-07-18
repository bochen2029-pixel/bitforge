// bitforge - address.h
// Translate between an absolute bit index and (byte address, bit-in-byte).
// A "hit" in this tool is bit-addressed: byte-aligned scans report bit 0, the
// unaligned bitfield scanner can report any bit 0..7. That is the whole point --
// the atomic unit is the bit, not the byte.
#pragma once
#include <cstdint>

namespace bf {

struct BitAddr { uint64_t byte; int bit; };   // bit in [0,7], MSB-first

inline uint64_t bit_index(uint64_t byte, int bit) { return byte * 8 + (uint64_t)bit; }
inline BitAddr  from_bit (uint64_t b)             { return BitAddr{ b >> 3, (int)(b & 7) }; }

} // namespace bf
