// bitforge - gpu_bitforge.cu
// CUDA analytics island: the embarrassingly-parallel compute that the CPU is slow
// at -- unaligned bit-pattern search, value scan, and per-block entropy -- offloaded
// to the GPU. Compiled two ways:
//   nvcc -O3 -DGPU_STANDALONE gpu_bitforge.cu -o bitforge_gpu.exe   (CPU-vs-GPU bench)
//   nvcc -O3 -c gpu_bitforge.cu -o gpu_bitforge.obj                 (link into bitforge)
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <chrono>

#define CK(x) do{ cudaError_t e_=(x); if(e_!=cudaSuccess){ printf("[cuda] %s @ %d: %s\n",#x,__LINE__,cudaGetErrorString(e_)); } }while(0)

// ---- kernels ----
// Each thread tests one starting BIT offset -> the unaligned bit search. This is
// 8x the work of a byte scan (every sub-byte offset), which is why it's a CPU dog
// and a GPU sweet spot.
__global__ void k_bitsearch(const uint8_t* d, uint64_t totalBits, uint64_t pat,
                            uint64_t mask, int nbits, uint64_t* hits, int* count, int maxhits){
    uint64_t i = blockIdx.x*(uint64_t)blockDim.x + threadIdx.x;
    if(i + (uint64_t)nbits > totalBits) return;
    uint64_t v=0;
    for(int k=0;k<nbits;k++){ uint64_t b=i+k; int bit=(d[b>>3]>>(7-(b&7)))&1; v=(v<<1)|(uint64_t)bit; }
    if((v & mask) == pat){ int s=atomicAdd(count,1); if(s<maxhits) hits[s]=i; }
}

__global__ void k_valuescan(const uint8_t* d, uint64_t n, uint64_t target, int width,
                            uint64_t* hits, int* count, int maxhits){
    uint64_t i = blockIdx.x*(uint64_t)blockDim.x + threadIdx.x;
    if(i + (uint64_t)width > n) return;
    uint64_t v=0; for(int k=0;k<width;k++) v |= (uint64_t)d[i+k] << (8*k);
    if(width<8) v &= ((1ull<<(width*8))-1);
    if(v==target){ int s=atomicAdd(count,1); if(s<maxhits) hits[s]=i; }
}

// One thread per map cell: Shannon entropy of a sampled block -> 0..255.
__global__ void k_entropy(const uint8_t* d, uint64_t n, uint64_t block, int cells, int sample, uint8_t* out){
    int c = blockIdx.x*blockDim.x + threadIdx.x; if(c>=cells) return;
    uint64_t base=(uint64_t)c*block;
    if(base>=n){ out[c]=0; return; }
    int cnt[256];
    for(int i=0;i<256;i++) cnt[i]=0;
    int S = (int)((block<(uint64_t)sample)?block:(uint64_t)sample);
    if(base+(uint64_t)S>n) S=(int)(n-base);
    for(int i=0;i<S;i++) cnt[d[base+i]]++;
    float e=0.f, inv=(S>0)?1.f/(float)S:0.f;
    for(int i=0;i<256;i++){ if(!cnt[i]) continue; float p=cnt[i]*inv; e-=p*log2f(p); }
    int v=(int)(e*(255.f/8.f)); out[c]=(uint8_t)(v<0?0:(v>255?255:v));
}

// ---- host wrappers (C linkage so the C++ side can call them) ----
extern "C" {

int bf_gpu_init(){ int n=0; if(cudaGetDeviceCount(&n)!=cudaSuccess) return 0; return n; }

const char* bf_gpu_name(){
    static char nm[256]="none"; cudaDeviceProp p;
    if(cudaGetDeviceProperties(&p,0)==cudaSuccess)
        snprintf(nm,sizeof(nm),"%s  (%.1f GB VRAM, %d SMs)",p.name,p.totalGlobalMem/1073741824.0,p.multiProcessorCount);
    return nm;
}

long long bf_gpu_bitsearch(const unsigned char* data, unsigned long long n, unsigned long long pat,
                           unsigned long long mask, int nbits, unsigned long long* hits, int maxhits,
                           double* ms_kernel, double* ms_total){
    auto t0=std::chrono::high_resolution_clock::now();
    uint8_t* dd=nullptr; uint64_t* dh=nullptr; int* dc=nullptr;
    CK(cudaMalloc(&dd,n)); CK(cudaMalloc(&dh,sizeof(uint64_t)*maxhits)); CK(cudaMalloc(&dc,sizeof(int)));
    CK(cudaMemcpy(dd,data,n,cudaMemcpyHostToDevice)); CK(cudaMemset(dc,0,sizeof(int)));
    uint64_t totalBits=n*8ull; int bs=256; uint64_t grid=(totalBits+bs-1)/bs;
    cudaEvent_t e0,e1; cudaEventCreate(&e0); cudaEventCreate(&e1);
    cudaEventRecord(e0);
    k_bitsearch<<<(unsigned)grid,bs>>>(dd,totalBits,pat,mask,nbits,dh,dc,maxhits);
    cudaEventRecord(e1); cudaEventSynchronize(e1);
    float km=0; cudaEventElapsedTime(&km,e0,e1); if(ms_kernel)*ms_kernel=km;
    int cnt=0; CK(cudaMemcpy(&cnt,dc,sizeof(int),cudaMemcpyDeviceToHost));
    int cp=cnt<maxhits?cnt:maxhits; if(cp>0) CK(cudaMemcpy(hits,dh,sizeof(uint64_t)*cp,cudaMemcpyDeviceToHost));
    cudaFree(dd); cudaFree(dh); cudaFree(dc); cudaEventDestroy(e0); cudaEventDestroy(e1);
    auto t1=std::chrono::high_resolution_clock::now();
    if(ms_total)*ms_total=std::chrono::duration<double,std::milli>(t1-t0).count();
    return cnt;
}

long long bf_gpu_valuescan(const unsigned char* data, unsigned long long n, unsigned long long target,
                           int width, unsigned long long* hits, int maxhits, double* ms_kernel){
    uint8_t* dd=nullptr; uint64_t* dh=nullptr; int* dc=nullptr;
    CK(cudaMalloc(&dd,n)); CK(cudaMalloc(&dh,sizeof(uint64_t)*maxhits)); CK(cudaMalloc(&dc,sizeof(int)));
    CK(cudaMemcpy(dd,data,n,cudaMemcpyHostToDevice)); CK(cudaMemset(dc,0,sizeof(int)));
    int bs=256; uint64_t grid=(n+bs-1)/bs;
    cudaEvent_t e0,e1; cudaEventCreate(&e0); cudaEventCreate(&e1); cudaEventRecord(e0);
    k_valuescan<<<(unsigned)grid,bs>>>(dd,n,target,width,dh,dc,maxhits);
    cudaEventRecord(e1); cudaEventSynchronize(e1);
    float km=0; cudaEventElapsedTime(&km,e0,e1); if(ms_kernel)*ms_kernel=km;
    int cnt=0; CK(cudaMemcpy(&cnt,dc,sizeof(int),cudaMemcpyDeviceToHost));
    int cp=cnt<maxhits?cnt:maxhits; if(cp>0) CK(cudaMemcpy(hits,dh,sizeof(uint64_t)*cp,cudaMemcpyDeviceToHost));
    cudaFree(dd); cudaFree(dh); cudaFree(dc); cudaEventDestroy(e0); cudaEventDestroy(e1);
    return cnt;
}

void bf_gpu_entropy(const unsigned char* data, unsigned long long n, unsigned long long block,
                    int cells, int sample, unsigned char* out, double* ms_kernel){
    uint8_t* dd=nullptr; uint8_t* dout=nullptr;
    CK(cudaMalloc(&dd,n)); CK(cudaMalloc(&dout,cells));
    CK(cudaMemcpy(dd,data,n,cudaMemcpyHostToDevice));
    int bs=128; int grid=(cells+bs-1)/bs;
    cudaEvent_t e0,e1; cudaEventCreate(&e0); cudaEventCreate(&e1); cudaEventRecord(e0);
    k_entropy<<<grid,bs>>>(dd,n,block,cells,sample,dout);
    cudaEventRecord(e1); cudaEventSynchronize(e1);
    float km=0; cudaEventElapsedTime(&km,e0,e1); if(ms_kernel)*ms_kernel=km;
    CK(cudaMemcpy(out,dout,cells,cudaMemcpyDeviceToHost));
    cudaFree(dd); cudaFree(dout); cudaEventDestroy(e0); cudaEventDestroy(e1);
}

} // extern "C"

// ================= standalone CPU-vs-GPU benchmark =================
#ifdef GPU_STANDALONE
static long long cpu_bitsearch(const uint8_t* d, uint64_t n, uint64_t pat, uint64_t mask, int nbits){
    uint64_t totalBits=n*8ull; long long cnt=0;
    for(uint64_t i=0;i+nbits<=totalBits;i++){
        uint64_t v=0; for(int k=0;k<nbits;k++){ uint64_t b=i+k; int bit=(d[b>>3]>>(7-(b&7)))&1; v=(v<<1)|(uint64_t)bit; }
        if((v&mask)==pat) cnt++;
    }
    return cnt;
}
static uint8_t cpu_entropy_cell(const uint8_t* d, uint64_t base, int S){
    int cnt[256]={0}; for(int i=0;i<S;i++) cnt[d[base+i]]++;
    double e=0, inv=1.0/S; for(int i=0;i<256;i++){ if(!cnt[i])continue; double p=cnt[i]*inv; e-=p*log2((double)p); }
    int v=(int)(e/8.0*255.0); return (uint8_t)(v<0?0:v>255?255:v);
}
static double ms_since(std::chrono::high_resolution_clock::time_point t){
    return std::chrono::duration<double,std::milli>(std::chrono::high_resolution_clock::now()-t).count();
}

int main(){
    int nd=bf_gpu_init();
    printf("=== bitforge GPU bench ===\nCUDA devices: %d\n", nd);
    if(nd<=0){ printf("no CUDA device found\n"); return 1; }
    printf("device: %s\n\n", bf_gpu_name());

    // ---------- unaligned bit-pattern search ----------
    uint64_t N = 16ull*1024*1024;                       // 16 MB
    uint8_t* buf=(uint8_t*)malloc(N);
    uint32_t s=12345; for(uint64_t i=0;i<N;i++){ s^=s<<13; s^=s>>17; s^=s<<5; buf[i]=(uint8_t)s; }
    const int PLANT=5; uint8_t pb[4]={0x09,0x1F,0xC3,0xA5};
    for(int j=0;j<PLANT;j++){ uint64_t off=((N/(PLANT+1))*(j+1)) & ~3ull; memcpy(buf+off,pb,4); }
    uint64_t pat=0; for(int k=0;k<4;k++) for(int b=0;b<8;b++) pat=(pat<<1)|((pb[k]>>(7-b))&1);
    uint64_t mask=0xFFFFFFFFull;

    uint64_t hits[64]; double km=0,kt=0;
    long long g=bf_gpu_bitsearch(buf,N,pat,mask,32,hits,64,&km,&kt);
    auto c0=std::chrono::high_resolution_clock::now();
    long long c=cpu_bitsearch(buf,N,pat,mask,32);
    double cms=ms_since(c0);
    printf("[unaligned 32-bit search over 16 MB = %llu bit offsets]\n", (unsigned long long)(N*8));
    printf("  GPU: %lld hits   kernel %.2f ms   (with H2D copy %.2f ms)\n", g, km, kt);
    printf("  CPU: %lld hits   %.1f ms  (single thread)\n", c, cms);
    printf("  --> %.0fx faster on GPU (kernel), %.0fx end-to-end   [hits match: %s]\n\n",
           cms/km, cms/kt, (g==c)?"YES":"NO");

    // ---------- value scan over 128 MB ----------
    uint64_t M=128ull*1024*1024; uint8_t* big=(uint8_t*)malloc(M);
    for(uint64_t i=0;i<M;i++){ s^=s<<13; s^=s>>17; s^=s<<5; big[i]=(uint8_t)s; }
    uint32_t rare=0xDEADBEEF; int pv=7;
    for(int j=0;j<pv;j++){ uint64_t off=((M/(pv+1))*(j+1)) & ~3ull; memcpy(big+off,&rare,4); }
    double vkm=0; long long gv=bf_gpu_valuescan(big,M,rare,4,hits,64,&vkm);
    printf("[value scan u32=0xDEADBEEF over 128 MB]\n  GPU: %lld hits   kernel %.2f ms\n\n", gv, vkm);

    // ---------- entropy map (16384 cells) ----------
    int cells=128*128; uint64_t block=M/cells; int sample=512;
    uint8_t* eg=(uint8_t*)malloc(cells); double ekm=0;
    bf_gpu_entropy(big,M,block,cells,sample,eg,&ekm);
    auto e0=std::chrono::high_resolution_clock::now();
    int mism=0; for(int cix=0;cix<cells;cix++){ uint64_t base=(uint64_t)cix*block; int S=(int)(block<(uint64_t)sample?block:sample); uint8_t cpu=cpu_entropy_cell(big,base,S); if(abs((int)cpu-(int)eg[cix])>2) mism++; }
    double ems=ms_since(e0);
    printf("[entropy map 128x128 over 128 MB]\n  GPU kernel %.2f ms   CPU %.1f ms   cells differing>2: %d/%d\n", ekm, ems, mism, cells);

    free(buf); free(big); free(eg);
    printf("\n=== done ===\n");
    return 0;
}
#endif
