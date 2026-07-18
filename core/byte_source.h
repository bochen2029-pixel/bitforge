// bitforge - byte_source.h
// The single interface the whole application talks to. A disk, a file, a live
// process, physical RAM, or a DMA device are all just implementations of this.
// Milestone 1 ships FileSource and ProcessSource; PhysicalMemory / DMA sources
// drop in later behind the exact same interface without touching the UI, the
// bit_span, the scanner, or the renderer.
#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <vector>

namespace bf {

// One contiguous region of a source's address space: a VirtualQueryEx region for
// a process, or the single [0,size) span of a file. Addresses live in the
// source's own coordinate space -- a virtual address for a process, a file
// offset for a file. That is deliberately the ONLY thing the rest of the code
// needs to know about "where".
struct Region {
    uint64_t    base = 0;
    uint64_t    size = 0;
    uint32_t    protect = 0;   // native protection flags (PAGE_* for a process)
    uint32_t    state   = 0;   // MEM_COMMIT / MEM_RESERVE / ... (process only)
    uint32_t    type    = 0;   // MEM_IMAGE / MEM_MAPPED / MEM_PRIVATE (process)
    bool        readable   = false;
    bool        writable   = false;
    bool        executable = false;
    std::string tag;           // module name, "image"/"mapped"/"private", "file"
};

class IByteSource {
public:
    virtual ~IByteSource() = default;

    // Read/write n bytes at absolute address `addr`. Returns bytes transferred
    // (may be < n at a region edge or an unreadable page); 0 on hard failure.
    virtual size_t read (uint64_t addr, void* dst, size_t n) = 0;
    virtual size_t write(uint64_t addr, const void* src, size_t n) = 0;

    // Scannable regions, pre-filtered to committed & readable for a process.
    virtual std::vector<Region> regions() = 0;

    // Total addressable size when meaningful (a file). 0 for a sparse process
    // address space -- use regions() there instead of assuming a flat extent.
    virtual uint64_t size() const = 0;

    virtual bool        writable() const = 0;
    virtual const char* kind()     const = 0;   // "file" | "process" | ...
    virtual std::string label()    const = 0;   // human label for status bar
};

} // namespace bf
