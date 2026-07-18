// bitforge - buffer_source.h
// The safest possible IByteSource: a chunk of memory this process owns outright
// (VirtualAlloc). Reading/writing it can't touch anything else. Used as the
// scratch sandbox for the Arecibo easter egg -- we literally write the message's
// bits into a real committed page and render them.
#pragma once
#include "byte_source.h"
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bf {

class BufferSource : public IByteSource {
    uint8_t*    p_ = nullptr;
    uint64_t    size_ = 0;
    std::string label_;
public:
    ~BufferSource() { if (p_) VirtualFree(p_, 0, MEM_RELEASE); }

    bool alloc(uint64_t size, const char* label) {
        p_ = (uint8_t*)VirtualAlloc(nullptr, (SIZE_T)size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
        if (!p_) return false;
        size_ = size; label_ = label ? label : "sandbox"; return true;
    }
    uint8_t* data()       { return p_; }
    uint64_t base() const { return (uint64_t)(uintptr_t)p_; }

    size_t read(uint64_t addr, void* dst, size_t n) override {
        uint64_t b = base(); if (addr < b || addr - b >= size_) return 0;
        size_t avail = (size_t)(size_ - (addr - b)); size_t k = n < avail ? n : avail;
        std::memcpy(dst, p_ + (addr - b), k); return k;
    }
    size_t write(uint64_t addr, const void* src, size_t n) override {
        uint64_t b = base(); if (addr < b || addr - b >= size_) return 0;
        size_t avail = (size_t)(size_ - (addr - b)); size_t k = n < avail ? n : avail;
        std::memcpy(p_ + (addr - b), src, k); return k;
    }
    std::vector<Region> regions() override {
        Region r; r.base = base(); r.size = size_; r.readable = true; r.writable = true; r.tag = "sandbox";
        return { r };
    }
    uint64_t    size() const override     { return size_; }
    bool        writable() const override { return true; }
    const char* kind() const override     { return "sandbox"; }
    std::string label() const override    { return label_; }
};

} // namespace bf
