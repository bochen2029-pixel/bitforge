// bitforge - file_source.h
// The zero-risk IByteSource: a plain file. Every code path in the app (grid,
// scanner, bit editing) runs against this with no chance of harming the OS, so
// it is the right thing to develop and demo against. The raw-disk source
// (\\.\PhysicalDriveN) is this same shape plus sector alignment; the VHDX path
// is literally this pointed at a mounted virtual disk.
#pragma once
#include "byte_source.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bf {

class FileSource : public IByteSource {
    HANDLE      h_ = INVALID_HANDLE_VALUE;
    uint64_t    size_ = 0;
    std::string path_;
    bool        writable_ = false;
public:
    ~FileSource() { if (h_ != INVALID_HANDLE_VALUE) CloseHandle(h_); }

    bool open(const std::string& path, bool write) {
        h_ = CreateFileA(path.c_str(),
                         GENERIC_READ | (write ? GENERIC_WRITE : 0),
                         FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                         OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h_ == INVALID_HANDLE_VALUE) return false;
        LARGE_INTEGER li{}; GetFileSizeEx(h_, &li);
        size_ = (uint64_t)li.QuadPart; path_ = path; writable_ = write; return true;
    }

    size_t read(uint64_t addr, void* dst, size_t n) override {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)addr;
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return 0;
        DWORD got = 0; if (!ReadFile(h_, dst, (DWORD)n, &got, nullptr)) return 0;
        return got;
    }
    size_t write(uint64_t addr, const void* src, size_t n) override {
        if (!writable_) return 0;
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)addr;
        if (!SetFilePointerEx(h_, li, nullptr, FILE_BEGIN)) return 0;
        DWORD put = 0; if (!WriteFile(h_, src, (DWORD)n, &put, nullptr)) return 0;
        return put;
    }
    std::vector<Region> regions() override {
        Region r; r.base = 0; r.size = size_; r.readable = true;
        r.writable = writable_; r.tag = "file"; return { r };
    }
    uint64_t    size() const override     { return size_; }
    bool        writable() const override { return writable_; }
    const char* kind() const override     { return "file"; }
    std::string label() const override    { return "file: " + path_; }
};

} // namespace bf
