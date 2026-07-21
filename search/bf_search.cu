// bf_search - GPU-resident instant content search + saturation bench.
//
// The idea (two design laws):
//   * Carmack's law  - compute in the cheapest isomorphic domain: one thread
//     per byte offset, bit-parallel quick-reject, warp-aggregated hit writes,
//     __popcll streaming. The automaton lives in registers, not tables.
//   * Graham's law   - the bottleneck is the abstraction stack, so delete it:
//     the corpus is staged ONCE into VRAM; every query afterwards is a pure
//     GPU sweep at memory-bandwidth speed. No index, no per-query I/O.
//
// Modes:
//   bf_search bench                       M0: prove all SMs saturate (GB/s vs peak)
//   bf_search index <dir> [--max MB]      M1: stage corpus, search-as-you-type
//
// Query syntax: literal text, '?' = any single byte, ":i" toggles case-fold,
// ":stats" reprints corpus info, ":q" quits.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <cuda_runtime.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cstdint>
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <algorithm>

#define CK(x) do{ cudaError_t e=(x); if(e!=cudaSuccess){ \
    fprintf(stderr,"CUDA error %s at %s:%d\n",cudaGetErrorString(e),__FILE__,__LINE__); exit(1);} }while(0)

using u8=uint8_t; using u32=uint32_t; using u64=unsigned long long;

// ------------------------------------------------------------------ kernels

// Pure-bandwidth popcount: grid-stride uint64 stream, warp-reduce, one atomic
// per warp. The "fill every core" proof.
__global__ void k_popcount(const u64* __restrict__ data, u64 nwords, u64* __restrict__ out){
    u64 i=(u64)blockIdx.x*blockDim.x+threadIdx.x;
    u64 stride=(u64)gridDim.x*blockDim.x;
    u64 acc=0;
    for(u64 k=i;k<nwords;k+=stride) acc+=__popcll(data[k]);
    for(int o=16;o;o>>=1) acc+=__shfl_down_sync(0xffffffffu,acc,o);
    if((threadIdx.x&31)==0 && acc) atomicAdd(out,acc);
}

// Literal search with '?' wildcards and optional case-fold.
//
// Fast path (literal first byte, case-sensitive): SWAR quick-reject. Each
// thread swallows 16 bytes as one uint4 and asks "does pat[0] occur anywhere
// in here?" with the classic XOR-broadcast zero-byte trick -- ~6 ALU ops to
// reject 16 candidate offsets, no extra memory traffic. Only the ~1/256 of
// windows that contain the first byte at all fall to the precise compare.
// Result: the kernel runs at memory-bandwidth speed like popcount.
//
// Slow path (wildcard first byte, or case-fold): byte-per-thread scan, still
// correct for every pattern.
__device__ __forceinline__ bool match_at(const u8* __restrict__ data, u64 n,
                                         const u8* __restrict__ pat, const u8* __restrict__ pmask,
                                         int m, int ci, u64 i){
    if(i+(u64)m>(u64)n) return false;
    for(int k=0;k<m;++k){
        if(pmask[k]){
            u8 a=data[i+k], b=pat[k];
            if(ci){ if(a>='A'&&a<='Z')a+=32; if(b>='A'&&b<='Z')b+=32; }
            if(a!=b) return false;
        }
    }
    return true;
}

__global__ void k_search(const u8* __restrict__ data, u64 n,
                         const u8* __restrict__ pat, const u8* __restrict__ pmask,
                         int m, int ci,
                         u64* __restrict__ hits, u32* __restrict__ hitCount, u32 maxHits){
    u64 tid=(u64)blockIdx.x*blockDim.x+threadIdx.x;
    const bool swar=(!ci && pmask[0] && n>=32);
    u64 base; int span;
    if(swar){ base=tid*32; span=base<n?(int)((n-base)<32?(n-base):32):0; }
    else    { base=tid;    span=base<n?1:0; }

    // SWAR whole-window pre-test: does pat[0] occur in these 32 bytes at all?
    bool possible=true;
    if(swar && span==32){
        uint4 v0=((const uint4*)data)[tid*2], v1=((const uint4*)data)[tid*2+1];
        u32 bcast=(u32)pat[0]*0x01010101u;
        u32 w[8]={v0.x,v0.y,v0.z,v0.w,v1.x,v1.y,v1.z,v1.w}; possible=false;
        #pragma unroll
        for(int j=0;j<8;++j){
            u32 x=w[j]^bcast;
            if((x-0x01010101u)&~x&0x80808080u){ possible=true; break; }
        }
    }

    // pass 1: count my hits (0..16)
    int cnt=0;
    if(possible){
        for(int j=0;j<span;++j){
            if(swar && data[base+j]!=pat[0]) continue;
            if(match_at(data,n,pat,pmask,m,ci,base+j)) cnt++;
        }
    }

    // warp inclusive-scan of cnt -> per-lane slot bases, ONE atomic per warp
    if(!__any_sync(0xffffffffu,cnt)) return;            // whole warp empty: skip
    u32 lane=threadIdx.x&31u;
    int scan=cnt;
    #pragma unroll
    for(int o=1;o<32;o<<=1){ int t=__shfl_up_sync(0xffffffffu,scan,o); if(lane>=(u32)o) scan+=t; }
    u32 warpTotal=__shfl_sync(0xffffffffu,scan,31);
    u32 warpBase=0;
    if(lane==0 && warpTotal) warpBase=atomicAdd(hitCount,warpTotal);
    warpBase=__shfl_sync(0xffffffffu,warpBase,0);
    u64 slot=(u64)warpBase+(u32)(scan-cnt);

    // pass 2: write my hits (window bytes are L1-hot)
    if(possible){
        for(int j=0;j<span;++j){
            if(swar && data[base+j]!=pat[0]) continue;
            if(match_at(data,n,pat,pmask,m,ci,base+j)){
                if(slot<maxHits) hits[slot]=base+j;
                ++slot;
            }
        }
    }
}

// Threads needed for a sweep: SWAR windows of 32, or one byte per thread.
static inline u64 threads_for(u64 n, int ci, const u8* pmsk){
    return (!ci && pmsk[0] && n>=32) ? (n+31)/32 : n;
}

// ------------------------------------------------------------------- bench

static double now_ms(){
    LARGE_INTEGER f,c; QueryPerformanceFrequency(&f); QueryPerformanceCounter(&c);
    return 1000.0*c.QuadPart/f.QuadPart;
}

static void run_bench(cudaDeviceProp& p){
    printf("\n== M0 saturation bench ==\n");
    int sm=p.multiProcessorCount;
    int memClk=0,busW=0;   // kHz, bits (removed from cudaDeviceProp in CUDA 13)
    CK(cudaDeviceGetAttribute(&memClk,cudaDevAttrMemoryClockRate,0));
    CK(cudaDeviceGetAttribute(&busW,cudaDevAttrGlobalMemoryBusWidth,0));
    double peak=(double)memClk*2.0*busW/8.0/1e6; // GB/s
    printf("device: %s | %d SMs | %.1f GB VRAM | theoretical peak %.0f GB/s\n\n",
           p.name, sm, p.totalGlobalMem/1073741824.0, peak);

    const size_t GB=1ull<<30;
    for(size_t sz : {GB, 4*GB, 12*GB}){
        u8* h=nullptr; CK(cudaHostAlloc(&h,sz,cudaHostAllocDefault));
        // xorshift fill (content for the search test; popcount doesn't care)
        u64* w=(u64*)h; u64 s=0x9E3779B97F4A7C15ull;
        for(size_t k=0;k<sz/8;++k){ s^=s<<13; s^=s>>7; s^=s<<17; w[k]=s; }
        u8* d=nullptr; CK(cudaMalloc(&d,sz));
        CK(cudaMemcpy(d,h,sz,cudaMemcpyHostToDevice));

        // -- popcount sweep --
        u64* dcnt; CK(cudaMalloc(&dcnt,8)); CK(cudaMemset(dcnt,0,8));
        int blocks=sm*8, thr=256;
        float best=1e30f;
        for(int r=0;r<3;++r){
            cudaEvent_t a,b; cudaEventCreate(&a); cudaEventCreate(&b);
            cudaEventRecord(a);
            k_popcount<<<blocks,thr>>>((const u64*)d,sz/8,dcnt);
            cudaEventRecord(b); cudaEventSynchronize(b);
            float ms; cudaEventElapsedTime(&ms,a,b); if(ms<best)best=ms;
        }
        double gbps=sz/best/1e6;
        printf("popcount  %5.1f GB  %7.2f ms  %7.1f GB/s  (%4.1f%% of peak)\n",
               sz/1073741824.0,best,gbps,100.0*gbps/peak);

        // -- literal search sweep (rare pattern -> full stream, no hit flood) --
        const char* needle="BITFORGE-NEEDLE-0123456789";
        int m=(int)strlen(needle);
        u8 *dpat,*dmsk; CK(cudaMalloc(&dpat,m)); CK(cudaMalloc(&dmsk,m));
        std::vector<u8> mm(m,1); CK(cudaMemcpy(dpat,needle,m,cudaMemcpyHostToDevice));
        CK(cudaMemcpy(dmsk,mm.data(),m,cudaMemcpyHostToDevice));
        u32* dhits; CK(cudaMalloc(&dhits,sizeof(u32)*1024*1024));
        u32* dhc;   CK(cudaMalloc(&dhc,4));
        CK(cudaMemset(dhc,0,4));
        size_t sb=(size_t)((threads_for(sz,0,mm.data())+255)/256);
        best=1e30f;
        for(int r=0;r<3;++r){
            CK(cudaMemset(dhc,0,4));
            cudaEvent_t a,b; cudaEventCreate(&a); cudaEventCreate(&b);
            cudaEventRecord(a);
            k_search<<<(unsigned)sb,256>>>(d,sz,dpat,dmsk,m,0,(u64*)dhits,dhc,1024*1024);
            cudaEventRecord(b); cudaEventSynchronize(b);
            float ms; cudaEventElapsedTime(&ms,a,b); if(ms<best)best=ms;
        }
        gbps=sz/best/1e6;
        printf("search    %5.1f GB  %7.2f ms  %7.1f GB/s  (%4.1f%% of peak)\n",
               sz/1073741824.0,best,gbps,100.0*gbps/peak);

        cudaFree(d); cudaFreeHost(h); cudaFree(dcnt); cudaFree(dpat); cudaFree(dmsk); cudaFree(dhits); cudaFree(dhc);
    }
    printf("\nall %d SMs engaged by every launch; the card is the pipeline.\n",sm);
}

// ------------------------------------------------------------------ loader

struct FileEnt { std::string path; u64 off; u64 size; };

static void walk(const std::string& root, std::vector<std::pair<std::string,u64>>& out,
                 u64& total, u64 capBytes, size_t capFiles){
    std::vector<std::string> stack{root};
    while(!stack.empty() && total<capBytes && out.size()<capFiles){
        std::string dir=stack.back(); stack.pop_back();
        WIN32_FIND_DATAA fd;
        HANDLE h=FindFirstFileExA((dir+"\\*").c_str(),FindExInfoBasic,&fd,FindExSearchNameMatch,nullptr,0);
        if(h==INVALID_HANDLE_VALUE) continue;
        do{
            if(fd.dwFileAttributes&FILE_ATTRIBUTE_REPARSE_POINT) continue;     // no junctions/symlinks
            const char* n=fd.cFileName;
            if(!strcmp(n,".")||!strcmp(n,"..")) continue;
            std::string full=dir+"\\"+n;
            if(fd.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY){ stack.push_back(full); continue; }
            u64 sz=((u64)fd.nFileSizeHigh<<32)|fd.nFileSizeLow;
            if(!sz) continue;
            out.emplace_back(full,sz); total+=sz;
        }while(FindNextFileA(h,&fd));
        FindClose(h);
    }
}

// ------------------------------------------------------------------- main

static int usage(){
    printf("bf_search - GPU-resident instant content search\n"
           "  bench                          saturation proof (GB/s vs peak)\n"
           "  index <dir> [--max MB]         stage corpus into VRAM, search-as-you-type\n"
           "queries: literal text, '?' = any byte, :i case-fold, :stats, :q quit\n");
    return 1;
}

int main(int argc,char** argv){
    if(argc<2) return usage();
    int ndev=0; CK(cudaGetDeviceCount(&ndev));
    if(!ndev){ printf("no CUDA device\n"); return 1; }
    CK(cudaSetDevice(0));
    cudaDeviceProp prop{}; CK(cudaGetDeviceProperties(&prop,0));

    if(!strcmp(argv[1],"bench")){ run_bench(prop); return 0; }
    if(strcmp(argv[1],"index") || argc<3) return usage();

    u64 capBytes=12288ull<<20;                       // default stage up to 12 GB
    for(int i=3;i+1<argc;++i) if(!strcmp(argv[i],"--max")) capBytes=(u64)atoll(argv[i+1])<<20;

    // keep 1.5 GB of VRAM headroom for hits/workspace
    size_t freeB=0,totB=0; CK(cudaMemGetInfo(&freeB,&totB));
    u64 vramCap=freeB>(1ull<<30)+(1ull<<29)?freeB-(1ull<<30)-(1ull<<29):0;
    if(capBytes>vramCap) capBytes=vramCap;
    printf("staging corpus from %s  (cap %.1f GB, VRAM free %.1f GB)\n",
           argv[2],capBytes/1073741824.0,freeB/1073741824.0);

    double t0=now_ms();
    std::vector<std::pair<std::string,u64>> files; u64 total=0;
    walk(argv[2],files,total,capBytes,500000);
    if(total>capBytes) total=capBytes;
    printf("found %zu files (%.2f GB) in %.0f ms; loading...\n",files.size(),(double)total/1073741824.0,now_ms()-t0);
    if(files.empty()) return 1;

    // assign offsets
    std::vector<FileEnt> table; table.reserve(files.size());
    u64 off=0;
    for(auto& f:files){ if(off+f.second>capBytes) break; table.push_back({f.first,off,f.second}); off+=f.second; }
    u64 corpus=off;

    u8* host=nullptr; CK(cudaHostAlloc(&host,corpus,cudaHostAllocDefault));
    std::atomic<size_t> next{0}; std::atomic<u64> loaded{0};
    int nthreads=(int)std::min<unsigned>(std::thread::hardware_concurrency(),16);
    t0=now_ms();
    {
        std::vector<std::thread> pool;
        for(int t=0;t<nthreads;++t) pool.emplace_back([&](){
            std::vector<u8> buf(8u<<20);
            for(;;){
                size_t idx=next.fetch_add(1); if(idx>=table.size()) break;
                FileEnt& fe=table[idx];
                HANDLE h=CreateFileA(fe.path.c_str(),GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,
                                     nullptr,OPEN_EXISTING,FILE_FLAG_SEQUENTIAL_SCAN,nullptr);
                if(h==INVALID_HANDLE_VALUE){ fe.size=0; continue; }
                u64 done=0; bool ok=true;
                while(done<fe.size){
                    DWORD want=(DWORD)std::min<u64>(buf.size(),fe.size-done), got=0;
                    if(!ReadFile(h,buf.data(),want,&got,nullptr)||!got){ ok=false; break; }
                    memcpy(host+fe.off+done,buf.data(),got); done+=got;
                }
                CloseHandle(h);
                if(!ok && done<fe.size) memset(host+fe.off+done,0,(size_t)(fe.size-done)); // zero the unread tail
                loaded+=done;
            }
        });
        for(auto& t:pool) t.join();
    }
    double loadS=(now_ms()-t0)/1000.0;
    printf("loaded %.2f GB in %.1f s (%.2f GB/s)\n",corpus/1073741824.0,loadS,corpus/1073741824.0/(loadS>0?loadS:1));

    t0=now_ms();
    u8* d_data=nullptr; CK(cudaMalloc(&d_data,corpus));
    CK(cudaMemcpy(d_data,host,corpus,cudaMemcpyHostToDevice));
    double upS=(now_ms()-t0)/1000.0;

    const u32 MAXH=100000;
    u64* d_hits=nullptr; u32* d_hc=nullptr;
    CK(cudaMalloc(&d_hits,sizeof(u64)*MAXH)); CK(cudaMalloc(&d_hc,4));
    u8 *d_pat=nullptr,*d_pmsk=nullptr; CK(cudaMalloc(&d_pat,4096)); CK(cudaMalloc(&d_pmsk,4096));

    printf("uploaded in %.1f s (%.1f GB/s). %d SMs / %s ready.\n",
           upS,corpus/1073741824.0/(upS>0?upS:1),prop.multiProcessorCount,prop.name);
    printf("corpus: %.2f GB, %zu files. Type to search the WHOLE corpus.\n\n",
           corpus/1073741824.0,table.size());

    // ---------------- interactive search-as-you-type ----------------
    int ci=0;
    char line[2048];
    for(;;){
        printf("query%s> ",ci?" [ci]":"");
        if(!fgets(line,sizeof(line),stdin)) break;
        std::string q=line;
        while(!q.empty()&&(q.back()=='\n'||q.back()=='\r')) q.pop_back();
        if(q.empty()) continue;
        if(q==":q") break;
        if(q==":i"){ ci=!ci; printf("case-fold %s\n",ci?"ON":"OFF"); continue; }
        if(q==":stats"){ printf("%.2f GB, %zu files\n",corpus/1073741824.0,table.size()); continue; }
        if(q.size()>4090){ printf("(too long)\n"); continue; }

        std::vector<u8> pat(q.size()), pmsk(q.size());
        for(size_t k=0;k<q.size();++k){
            if(q[k]=='?'){ pmsk[k]=0; pat[k]=0; }
            else { pmsk[k]=1; u8 b=(u8)q[k]; if(ci&&b>='A'&&b<='Z')b+=32; pat[k]=b; }
        }
        int m=(int)q.size();
        CK(cudaMemcpy(d_pat,pat.data(),m,cudaMemcpyHostToDevice));
        CK(cudaMemcpy(d_pmsk,pmsk.data(),m,cudaMemcpyHostToDevice));
        CK(cudaMemset(d_hc,0,4));

        cudaEvent_t ea,eb; cudaEventCreate(&ea); cudaEventCreate(&eb);
        cudaEventRecord(ea);
        k_search<<<(unsigned)((threads_for(corpus,ci,pmsk.data())+255)/256),256>>>(d_data,corpus,d_pat,d_pmsk,m,ci,d_hits,d_hc,MAXH);
        cudaEventRecord(eb); cudaEventSynchronize(eb);
        float ms; cudaEventElapsedTime(&ms,ea,eb);

        u32 hc=0; CK(cudaMemcpy(&hc,d_hc,4,cudaMemcpyDeviceToHost));
        u32 show=hc<20?hc:20;
        std::vector<u64> hits(show);
        if(show) CK(cudaMemcpy(hits.data(),d_hits,show*sizeof(u64),cudaMemcpyDeviceToHost));
        std::sort(hits.begin(),hits.end());

        printf("%u hit%s  |  swept %.2f GB in %.2f ms  =  %.1f GB/s%s\n",
               hc,hc==1?"":"s",corpus/1073741824.0,ms,corpus/ms/1e6, hc>MAXH?" (count exact, list capped)":"");
        for(u64 ho:hits){
            // map offset -> file (upper_bound on start offsets)
            size_t lo=0,hi=table.size();
            while(lo+1<hi){ size_t mid=(lo+hi)/2; if(table[mid].off<=ho) lo=mid; else hi=mid; }
            FileEnt& fe=table[lo];
            u64 rel=ho-fe.off;
            // context from the host copy
            size_t c0=(size_t)(rel>24?rel-24:0), c1=(size_t)std::min<u64>(rel+40,fe.size);
            std::string ctx;
            for(size_t c=c0;c<c1;++c){ u8 b=host[fe.off+c]; ctx+=(b>=32&&b<127)?(char)b:'.'; }
            printf("  %s +0x%llX  ...%s...\n",fe.path.c_str(),rel,ctx.c_str());
        }
        if(hc>show) printf("  ... (%u more)\n",hc-show);
    }
    printf("bye.\n");
    return 0;
}
