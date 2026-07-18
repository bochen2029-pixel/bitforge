// bitforge - scanner.h
// The Cheat-Engine loop, extended to bit granularity: first_scan collects
// candidates, next_scan refines them with a comparator (unchanged / changed /
// increased / decreased / exact). Numeric scans are byte-aligned (bit 0); the
// Binary scan slides a masked pattern across the raw BITSTREAM at every one of
// the 8 sub-byte offsets, so a hit can start at any bit. That unaligned search
// is exactly the embarrassingly-parallel work the GPU stage will devour later.
#pragma once
#include "byte_source.h"
#include <string>
#include <vector>

namespace bf {

enum class VType { U8, U16, U32, U64, I8, I16, I32, I64, F32, F64, Binary };
enum class Cmp   { Exact, Unchanged, Changed, Increased, Decreased, GreaterThan, LessThan };

struct Hit {
    uint64_t addr;   // byte address of the match
    uint8_t  bit;    // starting bit within that byte (0 for aligned scans)
    uint64_t last;   // last observed value (numeric packed LE, or extracted bits)
};

class Scanner {
    IByteSource* src_ = nullptr;
    VType        type_ = VType::U32;
    int          width_ = 4;           // bytes touched per position
    uint64_t     bpat_ = 0, bmask_ = 0;
    int          bnbits_ = 0;          // Binary pattern length in bits
    std::vector<Hit> hits_;
    bool         truncated_ = false;
public:
    void  bind(IByteSource* s) { src_ = s; }
    void  set_type(VType t)    { type_ = t; if (t != VType::Binary) width_ = width_of(t); }
    VType type() const         { return type_; }
    void  reset()              { hits_.clear(); truncated_ = false; }

    size_t                  count()     const { return hits_.size(); }
    const std::vector<Hit>& hits()      const { return hits_; }
    bool                    truncated() const { return truncated_; }

    // First scan: `valueStr` is a number (decimal or 0x..) for numeric types, or
    // a bit pattern like "1011?01" (0/1/? wildcard) when type_ == Binary.
    bool first_scan(const std::string& valueStr);

    // Next scan: refine existing hits. `valueStr` used by Exact/Greater/Less.
    bool next_scan(Cmp c, const std::string& valueStr = "");

    // Current value at a hit (for live refresh / display / freeze capture).
    bool read_value(const Hit& h, uint64_t& out) const;

    static int         width_of(VType t);
    static const char* type_name(VType t);
    static bool        parse_value(VType t, const std::string& s, uint64_t& out);
    static bool        parse_binary(const std::string& s, uint64_t& pat, uint64_t& mask, int& nbits);
};

} // namespace bf
