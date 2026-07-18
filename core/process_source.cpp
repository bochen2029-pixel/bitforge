// bitforge - process_source.cpp
#include "process_source.h"
#include <tlhelp32.h>
#include <cstdio>

namespace bf {

std::vector<ProcInfo> enum_processes() {
    std::vector<ProcInfo> out;
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return out;
    PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
    if (Process32FirstW(snap, &pe)) {
        do {
            // Convert the wide exe name to UTF-8-ish narrow for the simple UI.
            char name[MAX_PATH]; int len = WideCharToMultiByte(
                CP_UTF8, 0, pe.szExeFile, -1, name, sizeof(name), nullptr, nullptr);
            out.push_back(ProcInfo{ (uint32_t)pe.th32ProcessID,
                                    std::string(name, len > 0 ? len - 1 : 0) });
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return out;
}

ProcessSource::~ProcessSource() { if (h_) CloseHandle(h_); }

bool ProcessSource::open(uint32_t pid, bool write) {
    if (h_) { CloseHandle(h_); h_ = nullptr; }
    DWORD rights = PROCESS_QUERY_INFORMATION | PROCESS_VM_READ;
    if (write) rights |= PROCESS_VM_WRITE | PROCESS_VM_OPERATION;
    h_ = OpenProcess(rights, FALSE, (DWORD)pid);
    if (!h_) return false;
    pid_ = pid; writable_ = write;

    // Best-effort friendly name (image base name).
    char base[MAX_PATH] = {0};
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap != INVALID_HANDLE_VALUE) {
        PROCESSENTRY32W pe{}; pe.dwSize = sizeof(pe);
        if (Process32FirstW(snap, &pe)) {
            do { if ((uint32_t)pe.th32ProcessID == pid) {
                WideCharToMultiByte(CP_UTF8, 0, pe.szExeFile, -1, base, sizeof(base), nullptr, nullptr);
                break;
            } } while (Process32NextW(snap, &pe));
        }
        CloseHandle(snap);
    }
    name_ = base;
    return true;
}

size_t ProcessSource::read(uint64_t addr, void* dst, size_t n) {
    SIZE_T got = 0;
    if (!ReadProcessMemory(h_, (LPCVOID)(uintptr_t)addr, dst, n, &got)) {
        // Partial reads are common at region edges; report whatever landed.
        return (size_t)got;
    }
    return (size_t)got;
}

size_t ProcessSource::write(uint64_t addr, const void* src, size_t n) {
    if (!writable_) return 0;
    SIZE_T put = 0;
    if (WriteProcessMemory(h_, (LPVOID)(uintptr_t)addr, src, n, &put) && put == n)
        return (size_t)put;

    // Fallback: the target page is likely read-only (code, or PAGE_READONLY
    // data). Temporarily grant write, poke, then restore the original bits.
    DWORD oldProt = 0;
    if (VirtualProtectEx(h_, (LPVOID)(uintptr_t)addr, n, PAGE_EXECUTE_READWRITE, &oldProt)) {
        BOOL ok = WriteProcessMemory(h_, (LPVOID)(uintptr_t)addr, src, n, &put);
        DWORD tmp; VirtualProtectEx(h_, (LPVOID)(uintptr_t)addr, n, oldProt, &tmp);
        if (ok) { FlushInstructionCache(h_, (LPCVOID)(uintptr_t)addr, n); return (size_t)put; }
    }
    return 0;
}

std::vector<Region> ProcessSource::regions() {
    std::vector<Region> out;
    MEMORY_BASIC_INFORMATION mbi{};
    uint64_t addr = 0;
    // Walk the whole user address space region-by-region. The loop ends when
    // VirtualQueryEx walks off the top of addressable memory.
    while (VirtualQueryEx(h_, (LPCVOID)(uintptr_t)addr, &mbi, sizeof(mbi)) == sizeof(mbi)) {
        uint64_t regBase = (uint64_t)(uintptr_t)mbi.BaseAddress;
        uint64_t regSize = (uint64_t)mbi.RegionSize;
        if (regSize == 0) break;

        const DWORD p = mbi.Protect;
        const bool guard    = (p & PAGE_GUARD) != 0;
        const bool noaccess = (p & PAGE_NOACCESS) != 0;
        const bool committed = mbi.State == MEM_COMMIT;
        const bool readable  = committed && !guard && !noaccess && p != 0;

        if (readable) {
            Region r;
            r.base = regBase; r.size = regSize;
            r.protect = p; r.state = mbi.State; r.type = mbi.Type;
            r.readable = true;
            r.writable = (p & (PAGE_READWRITE | PAGE_WRITECOPY |
                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            r.executable = (p & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                 PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
            switch (mbi.Type) {
                case MEM_IMAGE:   r.tag = "image";   break;
                case MEM_MAPPED:  r.tag = "mapped";  break;
                case MEM_PRIVATE: r.tag = "private"; break;
                default:          r.tag = "?";       break;
            }
            out.push_back(r);
        }
        addr = regBase + regSize;
    }
    return out;
}

std::string ProcessSource::label() const {
    char buf[160];
    snprintf(buf, sizeof(buf), "pid %u : %s%s", pid_, name_.c_str(), writable_ ? " [rw]" : " [ro]");
    return std::string(buf);
}

} // namespace bf
