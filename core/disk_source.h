// bitforge - disk_source.h
// A raw disk as an IByteSource: \\.\PhysicalDriveN (sector-aligned, FILE_FLAG_
// NO_BUFFERING) or a raw image file (a .vhd/.img is just sectors). Byte-granular
// reads/writes are handled internally with an aligned bounce buffer and a
// read-modify-write for partial-sector writes. Writing to a live disk locks and
// dismounts its volumes first (Windows blocks writes to mounted-volume sectors).
//
// Raw \\.\PhysicalDrive access requires Administrator. enum_disks() only queries
// geometry (open with access=0) so it works unelevated for the picker.
#pragma once
#include "byte_source.h"
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

namespace bf {

struct DiskInfo { int index; std::string model; uint64_t size; uint32_t sector; };
std::vector<DiskInfo> enum_disks();

class DiskSource : public IByteSource {
    HANDLE         h_ = INVALID_HANDLE_VALUE;
    uint64_t       size_ = 0;
    uint32_t       sector_ = 512;
    bool           writable_ = false, rawDevice_ = false;
    std::string    path_, label_;
    unsigned char* bounce_ = nullptr; size_t bounceCap_ = 0;
    std::vector<HANDLE> lockedVols_;
    void ensure_bounce(size_t n);
    void lock_volumes(int diskIndex);
public:
    ~DiskSource();
    bool open_drive(int index, bool write);                        // \\.\PhysicalDriveN
    bool open_image(const std::string& path, bool write, uint32_t sector = 512);

    size_t read (uint64_t addr, void* dst, size_t n) override;
    size_t write(uint64_t addr, const void* src, size_t n) override;
    std::vector<Region> regions() override;

    uint64_t    size() const override     { return size_; }
    bool        writable() const override { return writable_; }
    const char* kind() const override     { return "disk"; }
    std::string label() const override    { return label_; }
    uint32_t    sector() const            { return sector_; }
};

} // namespace bf
