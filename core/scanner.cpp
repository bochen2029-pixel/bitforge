// bitforge - scanner.cpp
#include "scanner.h"
#include "bit_span.h"
#include <cstring>
#include <cstdlib>
#include <algorithm>

namespace bf {

static const size_t   CHUNK   = 1u << 20;    // 1 MiB scan window
static const size_t   MAX_HITS = 4u << 20;   // 4M hit storage cap (then truncate)

int Scanner::width_of(VType t) {
    switch (t) {
        case VType::U8: case VType::I8:  return 1;
        case VType::U16: case VType::I16: return 2;
        case VType::U32: case VType::I32: case VType::F32: return 4;
        case VType::U64: case VType::I64: case VType::F64: return 8;
        default: return 0;
    }
}
const char* Scanner::type_name(VType t) {
    switch (t) {
        case VType::U8:return"u8"; case VType::U16:return"u16"; case VType::U32:return"u32"; case VType::U64:return"u64";
        case VType::I8:return"i8"; case VType::I16:return"i16"; case VType::I32:return"i32"; case VType::I64:return"i64";
        case VType::F32:return"f32"; case VType::F64:return"f64"; case VType::Binary:return"bits";
    }
    return "?";
}

static bool is_signed(VType t){ return t==VType::I8||t==VType::I16||t==VType::I32||t==VType::I64; }
static bool is_float (VType t){ return t==VType::F32||t==VType::F64; }

static uint64_t load_le(const uint8_t* p, int width){ uint64_t v=0; for(int i=0;i<width;++i) v|=(uint64_t)p[i]<<(8*i); return v; }

static int64_t sext(uint64_t v, int width){
    int bits = width*8; if (bits>=64) return (int64_t)v;
    uint64_t m = 1ull<<(bits-1); return (int64_t)((v ^ m) - m);
}

// three-way compare of two packed values under a type
static int cmp_vals(VType t, int width, uint64_t a, uint64_t b){
    if (t==VType::F32){ float fa,fb; uint32_t xa=(uint32_t)a,xb=(uint32_t)b; std::memcpy(&fa,&xa,4); std::memcpy(&fb,&xb,4); return (fa<fb)?-1:(fa>fb)?1:0; }
    if (t==VType::F64){ double da,db; std::memcpy(&da,&a,8); std::memcpy(&db,&b,8); return (da<db)?-1:(da>db)?1:0; }
    if (is_signed(t)){ int64_t sa=sext(a,width), sb=sext(b,width); return (sa<sb)?-1:(sa>sb)?1:0; }
    return (a<b)?-1:(a>b)?1:0;
}

bool Scanner::parse_value(VType t, const std::string& s, uint64_t& out){
    if (s.empty()) return false;
    if (is_float(t)){
        char* end=nullptr;
        if (t==VType::F32){ float f=strtof(s.c_str(),&end); if(end==s.c_str()) return false; uint32_t u; std::memcpy(&u,&f,4); out=u; }
        else             { double d=strtod(s.c_str(),&end); if(end==s.c_str()) return false; uint64_t u; std::memcpy(&u,&d,8); out=u; }
        return true;
    }
    char* end=nullptr;
    int base = (s.size()>2 && s[0]=='0' && (s[1]=='x'||s[1]=='X')) ? 16 : 10;
    uint64_t v = (base==16) ? strtoull(s.c_str()+2,&end,16)
               : (is_signed(t) ? (uint64_t)strtoll(s.c_str(),&end,10) : strtoull(s.c_str(),&end,10));
    if (end==s.c_str()) return false;
    int w = width_of(t);
    if (w>0 && w<8){ uint64_t mask=(1ull<<(w*8))-1; v&=mask; }
    out = v; return true;
}

bool Scanner::parse_binary(const std::string& s, uint64_t& pat, uint64_t& mask, int& nbits){
    pat=0; mask=0; nbits=0;
    for(char c : s){
        if (c==' '||c=='_'||c=='\t') continue;
        if (c=='0'||c=='1'){ pat=(pat<<1)|(uint64_t)(c=='1'); mask=(mask<<1)|1ull; }
        else if (c=='?'||c=='*'||c=='x'||c=='X'){ pat<<=1; mask<<=1; }
        else return false;
        if (++nbits>64) return false;
    }
    return nbits>0;
}

bool Scanner::read_value(const Hit& h, uint64_t& out) const {
    if (!src_) return false;
    if (type_==VType::Binary){
        int foot=(bnbits_+7)/8 + 1; uint8_t buf[9]={0};
        if (src_->read(h.addr, buf, foot)==0) return false;
        out = extract_bits(buf, (uint64_t)h.bit, bnbits_) & bmask_;
        return true;
    }
    uint8_t buf[8]={0};
    if (src_->read(h.addr, buf, width_)==0) return false;
    out = load_le(buf, width_);
    return true;
}

bool Scanner::first_scan(const std::string& valueStr){
    hits_.clear(); truncated_=false;
    if (!src_) return false;

    uint64_t target=0;
    if (type_==VType::Binary){
        if (!parse_binary(valueStr, bpat_, bmask_, bnbits_)) return false;
        width_ = (bnbits_+7)/8;
        bpat_ &= bmask_;
    } else {
        if (!parse_value(type_, valueStr, target)) return false;
    }

    const int foot = (type_==VType::Binary) ? ((bnbits_+7)/8 + 1) : width_;
    std::vector<uint8_t> buf;

    for (const Region& r : src_->regions()){
        uint64_t off = 0;
        while (off < r.size){
            const uint64_t chunkStart = off;
            const size_t   want = (size_t)std::min<uint64_t>(CHUNK, r.size - off);
            const size_t   over = (size_t)std::min<uint64_t>((uint64_t)(foot>0?foot-1:0), r.size - (off+want));
            buf.resize(want + over);
            const size_t got = src_->read(r.base + chunkStart, buf.data(), buf.size());
            off += want;
            if (got == 0) continue;

            if (type_==VType::Binary){
                // Only accept matches that START within this chunk's [0,want) so
                // the overlap bytes complete cross-chunk patterns without dupes.
                const uint64_t startCap = std::min<uint64_t>((uint64_t)want*8, (got*8>=(uint64_t)bnbits_)?(got*8-bnbits_+1):0);
                for (uint64_t gb=0; gb<startCap; ++gb){
                    if ((extract_bits(buf.data(), gb, bnbits_) & bmask_) == bpat_){
                        if (hits_.size()>=MAX_HITS){ truncated_=true; return true; }
                        hits_.push_back(Hit{ r.base+chunkStart+(gb>>3), (uint8_t)(gb&7),
                                             extract_bits(buf.data(), gb, bnbits_) & bmask_ });
                    }
                }
            } else {
                const size_t limit = (got>=(size_t)width_) ? std::min<size_t>(want, got-width_+1) : 0;
                for (size_t i=0;i<limit;++i){
                    if (load_le(buf.data()+i, width_)==target){
                        if (hits_.size()>=MAX_HITS){ truncated_=true; return true; }
                        hits_.push_back(Hit{ r.base+chunkStart+i, 0, target });
                    }
                }
            }
        }
    }
    return true;
}

bool Scanner::next_scan(Cmp c, const std::string& valueStr){
    if (!src_) return false;
    uint64_t target=0; bool haveTarget=false;
    if (c==Cmp::Exact || c==Cmp::GreaterThan || c==Cmp::LessThan){
        if (type_==VType::Binary){ int nb; if(!parse_binary(valueStr,target,bmask_,nb)) return false; target&=bmask_; }
        else if (!parse_value(type_, valueStr, target)) return false;
        haveTarget=true;
    }
    std::vector<Hit> keep; keep.reserve(hits_.size());
    for (Hit h : hits_){
        uint64_t cur;
        if (!read_value(h, cur)) continue;   // page gone / unreadable -> drop
        bool ok=false;
        switch(c){
            case Cmp::Exact:       ok = (cur==target); break;
            case Cmp::Unchanged:   ok = (cur==h.last); break;
            case Cmp::Changed:     ok = (cur!=h.last); break;
            case Cmp::Increased:   ok = cmp_vals(type_,width_,cur,h.last)>0; break;
            case Cmp::Decreased:   ok = cmp_vals(type_,width_,cur,h.last)<0; break;
            case Cmp::GreaterThan: ok = cmp_vals(type_,width_,cur,target)>0; break;
            case Cmp::LessThan:    ok = cmp_vals(type_,width_,cur,target)<0; break;
        }
        if (ok){ h.last=cur; keep.push_back(h); }
    }
    (void)haveTarget;
    hits_.swap(keep);
    return true;
}

} // namespace bf
