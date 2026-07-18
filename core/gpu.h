// bitforge - gpu.h
// C++ interface to the CUDA analytics island (core/../gpu/gpu_bitforge.cu).
// Compiled in only when the build links CUDA (BITFORGE_CUDA); otherwise the
// helpers degrade to "not present" and callers fall back to the CPU paths.
#pragma once

#ifdef BITFORGE_CUDA
extern "C" {
    int         bf_gpu_init();          // number of CUDA devices (0 = none)
    const char* bf_gpu_name();
    long long   bf_gpu_bitsearch(const unsigned char* data, unsigned long long n,
                                 unsigned long long pat, unsigned long long mask, int nbits,
                                 unsigned long long* hits, int maxhits,
                                 double* ms_kernel, double* ms_total);
    long long   bf_gpu_valuescan(const unsigned char* data, unsigned long long n,
                                 unsigned long long target, int width,
                                 unsigned long long* hits, int maxhits, double* ms_kernel);
    void        bf_gpu_entropy(const unsigned char* data, unsigned long long n,
                               unsigned long long block, int cells, int sample,
                               unsigned char* out, double* ms_kernel);
}
inline bool bf_gpu_present(){ return bf_gpu_init() > 0; }
#else
inline bool        bf_gpu_present(){ return false; }
inline const char* bf_gpu_name(){ return "CPU only (built without CUDA)"; }
#endif
