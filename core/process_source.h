// bitforge - process_source.h
// A live process's virtual address space as an IByteSource. This is the
// "practical rung" of the memory ladder: user mode, no driver, works on any
// process you own. Reads via ReadProcessMemory, writes via WriteProcessMemory
// with a VirtualProtectEx fallback for read-only pages, region map via
// VirtualQueryEx (committed + readable only -- the space is sparse, never
// brute-scanned).
#pragma once
#include "byte_source.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bf {

struct ProcInfo { uint32_t pid; std::string name; };

// Snapshot of currently-running processes for the attach picker.
std::vector<ProcInfo> enum_processes();

class ProcessSource : public IByteSource {
    HANDLE      h_ = nullptr;
    uint32_t    pid_ = 0;
    std::string name_;
    bool        writable_ = false;
public:
    ~ProcessSource();

    bool open(uint32_t pid, bool write);
    bool is_open() const { return h_ != nullptr; }
    uint32_t pid() const { return pid_; }

    size_t read (uint64_t addr, void* dst, size_t n) override;
    size_t write(uint64_t addr, const void* src, size_t n) override;
    std::vector<Region> regions() override;

    uint64_t    size() const override     { return 0; }   // sparse -> use regions()
    bool        writable() const override { return writable_; }
    const char* kind() const override     { return "process"; }
    std::string label() const override;
};

} // namespace bf
