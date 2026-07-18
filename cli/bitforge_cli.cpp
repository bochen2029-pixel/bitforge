// bitforge - bitforge_cli.cpp
// Console harness that drives the core engine end-to-end -- the proof that the
// IByteSource + bit_span + scanner stack actually reads, scans, and edits memory
// (and files) at the bit level, with zero GUI dependencies.
#include "byte_source.h"
#include "bit_span.h"
#include "source_ops.h"
#include "file_source.h"
#include "process_source.h"
#include "disk_source.h"
#include "scanner.h"
#include "gpu.h"
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <memory>

using namespace bf;

static bool parse_type(const char* s, VType& t) {
    std::string v = s;
    if (v=="u8")t=VType::U8; else if(v=="u16")t=VType::U16; else if(v=="u32")t=VType::U32; else if(v=="u64")t=VType::U64;
    else if(v=="i8")t=VType::I8; else if(v=="i16")t=VType::I16; else if(v=="i32")t=VType::I32; else if(v=="i64")t=VType::I64;
    else if(v=="f32")t=VType::F32; else if(v=="f64")t=VType::F64;
    else if(v=="bits"||v=="bin"||v=="binary")t=VType::Binary;
    else return false;
    return true;
}
static uint64_t parse_addr(const char* s){ return strtoull((s[0]=='0'&&(s[1]=='x'||s[1]=='X'))?s+2:s, nullptr, 16); }

static void hexdump(IByteSource& s, uint64_t addr, size_t n) {
    std::vector<uint8_t> b(n);
    size_t got = s.read(addr, b.data(), n);
    printf("read %zu / %zu bytes at 0x%llX\n", got, n, (unsigned long long)addr);
    for (size_t i=0;i<got;i+=16){
        printf("%012llX  ", (unsigned long long)(addr+i));
        for (size_t j=0;j<16;++j){ if(i+j<got) printf("%02X ", b[i+j]); else printf("   "); }
        printf(" ");
        for (size_t j=0;j<16 && i+j<got;++j){ uint8_t c=b[i+j]; putchar((c>=32&&c<127)?c:'.'); }
        putchar('\n');
    }
    if (got && n<=8){
        printf("bits (MSB-first): ");
        for (size_t i=0;i<got;++i){ for(int k=0;k<8;++k) putchar(get_bit(&b[i], (uint64_t)k)?'1':'0'); putchar(' '); }
        putchar('\n');
    }
}

static void print_hits(Scanner& sc){
    printf("hits: %zu%s\n", sc.count(), sc.truncated()?" (truncated)":"");
    size_t shown=0;
    for (const Hit& h : sc.hits()){
        printf("  0x%llX", (unsigned long long)h.addr);
        if (h.bit) printf(".b%u", h.bit);
        printf(" = %llu (0x%llX)\n", (unsigned long long)h.last, (unsigned long long)h.last);
        if (++shown>=20){ printf("  ...\n"); break; }
    }
}

static int usage(){
    printf("bitforge_cli - bit-level memory/file inspector\n"
           "  ps\n"
           "  regions <pid>\n"
           "  read    <pid> <hexaddr> <n>\n"
           "  scan    <pid> <type> <value>       (type: u8..u64,i8..i64,f32,f64,bits)\n"
           "  set     <pid> <hexaddr> <type> <value>\n"
           "  poke    <pid> <hexaddr> <bit0-7> <0|1>\n"
           "  fread   <file> <hexaddr> <n>\n"
           "  fscan   <file> <type> <value>\n"
           "  fpoke   <file> <hexaddr> <bit0-7> <0|1>\n"
           "  disks\n"
           "  dread   <index> <hexaddr> <n>       (raw \\\\.\\PhysicalDriveN, admin)\n"
           "  dscan   <index> <type> <value>\n"
           "  dset    <index> <hexaddr> <type> <value>\n"
           "  dpoke   <index> <hexaddr> <bit0-7> <0|1>\n"
           "  iread   <image> <hexaddr> <n>       (raw disk image file)\n"
           "  ipoke   <image> <hexaddr> <bit0-7> <0|1>\n"
           "  gpu\n"
           "  gpuscan <file> <bitpattern>          (GPU unaligned bit search)\n");
    return 1;
}

int main(int argc, char** argv){
    if (argc<2) return usage();
    std::string cmd = argv[1];

    if (cmd=="ps"){
        for (auto& p : enum_processes()) printf("%6u  %s\n", p.pid, p.name.c_str());
        return 0;
    }
    // ---- process commands ----
    if (cmd=="regions" && argc>=3){
        ProcessSource ps; if(!ps.open((uint32_t)atoi(argv[2]),false)){ printf("open failed (err %lu)\n",GetLastError()); return 1; }
        auto rs = ps.regions(); uint64_t total=0;
        for (auto& r : rs){ total+=r.size;
            printf("0x%012llX  %10llu  %c%c%c  %s\n",(unsigned long long)r.base,(unsigned long long)r.size,
                   r.readable?'r':'-', r.writable?'w':'-', r.executable?'x':'-', r.tag.c_str()); }
        printf("%zu regions, %.1f MiB committed+readable\n", rs.size(), total/1048576.0);
        return 0;
    }
    if (cmd=="read" && argc>=5){
        ProcessSource ps; if(!ps.open((uint32_t)atoi(argv[2]),false)){ printf("open failed\n"); return 1; }
        hexdump(ps, parse_addr(argv[3]), (size_t)atoll(argv[4])); return 0;
    }
    if (cmd=="scan" && argc>=5){
        ProcessSource ps; if(!ps.open((uint32_t)atoi(argv[2]),false)){ printf("open failed\n"); return 1; }
        VType t; if(!parse_type(argv[3],t)) return usage();
        Scanner sc; sc.bind(&ps); sc.set_type(t);
        if(!sc.first_scan(argv[4])){ printf("scan parse failed\n"); return 1; }
        print_hits(sc); return 0;
    }
    if (cmd=="set" && argc>=6){
        ProcessSource ps; if(!ps.open((uint32_t)atoi(argv[2]),true)){ printf("open(rw) failed (err %lu)\n",GetLastError()); return 1; }
        uint64_t addr=parse_addr(argv[3]); VType t; if(!parse_type(argv[4],t)) return usage();
        uint64_t v; if(!Scanner::parse_value(t,argv[5],v)){ printf("bad value\n"); return 1; }
        int w=Scanner::width_of(t); uint8_t buf[8]; for(int i=0;i<w;++i) buf[i]=(uint8_t)(v>>(8*i));
        size_t put=ps.write(addr,buf,w); printf("wrote %zu/%d bytes\n",put,w); hexdump(ps,addr,w); return 0;
    }
    if (cmd=="poke" && argc>=5){
        ProcessSource ps; if(!ps.open((uint32_t)atoi(argv[2]),true)){ printf("open(rw) failed\n"); return 1; }
        uint64_t addr=parse_addr(argv[3]); int bit=atoi(argv[4]); int val=atoi(argv[5]);
        printf("before: "); hexdump(ps,addr,1);
        bool ok=write_bit(ps,addr,bit,val); printf("write_bit -> %s\n", ok?"ok":"FAIL");
        printf("after:  "); hexdump(ps,addr,1); return 0;
    }
    // ---- file commands (zero-risk path) ----
    if (cmd=="fread" && argc>=5){
        FileSource fs; if(!fs.open(argv[2],false)){ printf("open failed\n"); return 1; }
        hexdump(fs, parse_addr(argv[3]), (size_t)atoll(argv[4])); return 0;
    }
    if (cmd=="fscan" && argc>=5){
        FileSource fs; if(!fs.open(argv[2],false)){ printf("open failed\n"); return 1; }
        VType t; if(!parse_type(argv[3],t)) return usage();
        Scanner sc; sc.bind(&fs); sc.set_type(t);
        if(!sc.first_scan(argv[4])){ printf("scan parse failed\n"); return 1; }
        print_hits(sc); return 0;
    }
    if (cmd=="fpoke" && argc>=6){
        FileSource fs; if(!fs.open(argv[2],true)){ printf("open(rw) failed\n"); return 1; }
        uint64_t addr=parse_addr(argv[3]); int bit=atoi(argv[4]); int val=atoi(argv[5]);
        printf("before: "); hexdump(fs,addr,1);
        bool ok=write_bit(fs,addr,bit,val); printf("write_bit -> %s\n", ok?"ok":"FAIL");
        printf("after:  "); hexdump(fs,addr,1); return 0;
    }
    // ---- raw disk commands (\\.\PhysicalDriveN, needs admin) ----
    if (cmd=="disks"){
        for(auto& d : enum_disks())
            printf("PhysicalDrive%d  %-28s  %8.0f MB  %u B/sec\n", d.index, d.model.c_str(), d.size/1048576.0, d.sector);
        return 0;
    }
    if (cmd=="dread" && argc>=5){
        DiskSource ds; if(!ds.open_drive(atoi(argv[2]),false)){ printf("open failed (err %lu) - needs Administrator\n",GetLastError()); return 1; }
        printf("[%s]\n", ds.label().c_str()); hexdump(ds, parse_addr(argv[3]), (size_t)atoll(argv[4])); return 0;
    }
    if (cmd=="dscan" && argc>=5){
        DiskSource ds; if(!ds.open_drive(atoi(argv[2]),false)){ printf("open failed (err %lu)\n",GetLastError()); return 1; }
        VType t; if(!parse_type(argv[3],t)) return usage();
        Scanner sc; sc.bind(&ds); sc.set_type(t); if(!sc.first_scan(argv[4])){ printf("parse fail\n"); return 1; }
        print_hits(sc); return 0;
    }
    if (cmd=="dset" && argc>=6){
        DiskSource ds; if(!ds.open_drive(atoi(argv[2]),true)){ printf("open(rw) failed (err %lu) - needs Administrator\n",GetLastError()); return 1; }
        uint64_t addr=parse_addr(argv[3]); VType t; if(!parse_type(argv[4],t)) return usage();
        uint64_t v; if(!Scanner::parse_value(t,argv[5],v)){ printf("bad value\n"); return 1; }
        int w=Scanner::width_of(t); uint8_t buf[8]; for(int i=0;i<w;++i) buf[i]=(uint8_t)(v>>(8*i));
        size_t put=ds.write(addr,buf,w); printf("wrote %zu/%d bytes\n",put,w); hexdump(ds,addr,w); return 0;
    }
    if (cmd=="dpoke" && argc>=6){
        DiskSource ds; if(!ds.open_drive(atoi(argv[2]),true)){ printf("open(rw) failed (err %lu) - needs Administrator\n",GetLastError()); return 1; }
        uint64_t addr=parse_addr(argv[3]); int bit=atoi(argv[4]), val=atoi(argv[5]);
        printf("before: "); hexdump(ds,addr,1);
        bool ok=write_bit(ds,addr,bit,val); printf("write_bit -> %s\n", ok?"ok":"FAIL");
        printf("after:  "); hexdump(ds,addr,1); return 0;
    }
    if (cmd=="iread" && argc>=5){
        DiskSource ds; if(!ds.open_image(argv[2],false)){ printf("open failed\n"); return 1; }
        hexdump(ds, parse_addr(argv[3]), (size_t)atoll(argv[4])); return 0;
    }
    if (cmd=="ipoke" && argc>=6){
        DiskSource ds; if(!ds.open_image(argv[2],true)){ printf("open(rw) failed\n"); return 1; }
        uint64_t addr=parse_addr(argv[3]); int bit=atoi(argv[4]), val=atoi(argv[5]);
        printf("before: "); hexdump(ds,addr,1);
        bool ok=write_bit(ds,addr,bit,val); printf("write_bit -> %s\n", ok?"ok":"FAIL");
        printf("after:  "); hexdump(ds,addr,1); return 0;
    }
    // ---- GPU (CUDA analytics island) ----
    if (cmd=="gpu"){ printf("%s\n", bf_gpu_name()); return 0; }
    if (cmd=="gpuscan" && argc>=4){
        if(!bf_gpu_present()){ printf("no GPU (built without CUDA, or no device)\n"); return 1; }
#ifdef BITFORGE_CUDA
        FileSource fs; if(!fs.open(argv[2],false)){ printf("open failed\n"); return 1; }
        uint64_t n=fs.size(); if(n==0){ printf("empty file\n"); return 1; }
        std::vector<uint8_t> buf((size_t)n); fs.read(0,buf.data(),(size_t)n);
        uint64_t pat,mask; int nbits; if(!Scanner::parse_binary(argv[3],pat,mask,nbits)){ printf("bad bit pattern\n"); return 1; }
        unsigned long long hits[64]; double km=0,kt=0;
        long long c=bf_gpu_bitsearch(buf.data(),n,pat&mask,mask,nbits,hits,64,&km,&kt);
        printf("GPU bit-search '%s' over %.1f MB: %lld hits  (kernel %.2f ms, total %.2f ms)\n",argv[3],n/1048576.0,c,km,kt);
        int show=(int)(c<10?c:10); for(int i=0;i<show;++i) printf("  bit 0x%llX = byte 0x%llX .b%llu\n",hits[i],hits[i]>>3,hits[i]&7);
#endif
        return 0;
    }
    return usage();
}
