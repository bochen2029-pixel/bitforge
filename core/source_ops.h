// bitforge - source_ops.h
// Bit-level read/modify/write helpers that work over ANY IByteSource. Because
// the smallest thing a CPU (or ReadProcessMemory, or a disk) can move is a byte,
// editing one bit is always: read the containing byte -> flip the bit in RAM ->
// write the byte back. This is that operation, once, for every source.
#pragma once
#include "byte_source.h"
#include "bit_span.h"

namespace bf {

inline int read_bit(IByteSource& s, uint64_t addr, int bit) {
    uint8_t b; if (s.read(addr, &b, 1) != 1) return -1; return get_bit(&b, (uint64_t)bit);
}

inline bool write_bit(IByteSource& s, uint64_t addr, int bit, int val) {
    uint8_t b; if (s.read(addr, &b, 1) != 1) return false;
    set_bit(&b, (uint64_t)bit, val);
    return s.write(addr, &b, 1) == 1;
}

inline bool toggle_bit_src(IByteSource& s, uint64_t addr, int bit) {
    uint8_t b; if (s.read(addr, &b, 1) != 1) return false;
    toggle_bit(&b, (uint64_t)bit);
    return s.write(addr, &b, 1) == 1;
}

} // namespace bf
