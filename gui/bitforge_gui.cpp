// bitforge - bitforge_gui.cpp
// Zero-dependency Win32 + GDI bit-level viewer/editor. The Windows-98-defrag
// aesthetic, but every little square is ONE bit: blue = 1, dark = 0, click to
// toggle it straight into the live source. Attaches to a process (RPM/WPM) or a
// file, walks its regions, and drives the same Scanner the CLI proved out.
//
// Everything talks to IByteSource, so the disk / VHDX / physical-memory / DMA
// sources drop in behind the same UI with no changes here.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include "byte_source.h"
#include "bit_span.h"
#include "source_ops.h"
#include "file_source.h"
#include "process_source.h"
#include "scanner.h"
#include "buffer_source.h"
#include "arecibo.h"
#include "alien.h"
#include <memory>
#include <vector>
#include <string>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>

using namespace bf;

// ---- layout constants ----
enum { LEFTW = 300, BOTH = 172, STATUSH = 40, GAP = 1 };
#define RX (LEFTW + 4)

enum {
    ID_PROCS = 1001, ID_REGIONS, ID_OPENFILE, ID_REFRESH, ID_RW,
    ID_TYPE, ID_VALUE, ID_FIRST, ID_CMP, ID_NEXT, ID_RESULTS, ID_FREEZE, ID_CLEARFRZ,
    ID_ARECIBO, ID_SETI, ID_HUNT, ID_MAP,
    ID_TIMER = 1
};

// ---- global state ----
static HWND  g_main=nullptr, g_procs=nullptr, g_regions=nullptr, g_openfile=nullptr,
             g_refresh=nullptr, g_rw=nullptr, g_type=nullptr, g_value=nullptr,
             g_first=nullptr, g_cmp=nullptr, g_next=nullptr, g_results=nullptr,
             g_freeze=nullptr, g_clearfrz=nullptr, g_arecibo=nullptr, g_seti=nullptr, g_huntbtn=nullptr, g_mapbtn=nullptr;
static HFONT g_font=nullptr;
static HBRUSH g_bBg=nullptr, g_b0=nullptr, g_b1=nullptr, g_b0s=nullptr, g_b1s=nullptr, g_bUnk=nullptr;
static HBRUSH g_heat1=nullptr, g_heat2=nullptr, g_heat3=nullptr;  // recently-flipped bits glow

static std::unique_ptr<IByteSource> g_src;
static Scanner                      g_scan;
static std::vector<ProcInfo>        g_procList;
static std::vector<Region>          g_regionList;
static std::vector<Hit>             g_shown;

static uint64_t g_view = 0;        // top byte address of the grid view
static uint64_t g_selAddr = 0;     // selected byte
static int      g_selBit = 0;      // selected bit within byte (0..7 MSB-first)
static int      g_cell = 11;       // pixels per bit cell
static int      g_cols = 64;       // bits per row (multiple of 8)
static std::vector<uint8_t> g_buf;
static bool g_hunt=false, g_map=false;   // active overlay mode (else: the bit grid)

struct Frozen { uint64_t addr; int width; uint64_t val; };
static std::vector<Frozen> g_frozen;

static const VType kTypes[] = { VType::U8,VType::U16,VType::U32,VType::U64,
                                VType::I8,VType::I16,VType::I32,VType::I64,
                                VType::F32,VType::F64,VType::Binary };
static const Cmp   kCmps[]  = { Cmp::Exact,Cmp::Unchanged,Cmp::Changed,Cmp::Increased,Cmp::Decreased };

// ---- helpers ----
struct Geo { RECT area; int step, rows, cols, bytesPerRow, cellsBottom; };
static Geo geo(HWND h){
    RECT rc; GetClientRect(h,&rc);
    Geo g; g.area.left=RX; g.area.top=4; g.area.right=rc.right-4; g.area.bottom=rc.bottom-BOTH-6;
    g.cols=g_cols; g.step=g_cell+GAP; g.cellsBottom=g.area.bottom-STATUSH;
    g.rows=std::max(1,(int)((g.cellsBottom-g.area.top)/g.step)); g.bytesPerRow=g.cols/8;
    return g;
}
static void set_font(HWND h){ if(h) SendMessage(h,WM_SETFONT,(WPARAM)g_font,TRUE); }

static void refresh_procs(){
    g_procList = enum_processes();
    SendMessage(g_procs, LB_RESETCONTENT, 0, 0);
    char line[300];
    for (auto& p : g_procList){
        snprintf(line,sizeof(line),"%6u  %s", p.pid, p.name.c_str());
        SendMessageA(g_procs, LB_ADDSTRING, 0, (LPARAM)line);
    }
}
static void refresh_regions(){
    SendMessage(g_regions, LB_RESETCONTENT, 0, 0);
    g_regionList.clear();
    if (!g_src) return;
    g_regionList = g_src->regions();
    char line[300];
    for (auto& r : g_regionList){
        double kb = r.size/1024.0;
        snprintf(line,sizeof(line),"%-7s %011llX %8.0fK %c%c%c", r.tag.c_str(),
                 (unsigned long long)r.base, kb,
                 r.readable?'r':'-', r.writable?'w':'-', r.executable?'x':'-');
        SendMessageA(g_regions, LB_ADDSTRING, 0, (LPARAM)line);
    }
    char cap[160]; snprintf(cap,sizeof(cap),"%zu regions", g_regionList.size());
    SetWindowTextA(g_main, (std::string("bitforge  -  ")+g_src->label()).c_str());
}
static void jump_to(uint64_t addr){
    Geo g = geo(g_main);
    uint64_t bpr = g.bytesPerRow ? g.bytesPerRow : 8;
    g_view = (addr/bpr)*bpr;              // align view to a row boundary
    if (g_view > bpr* (uint64_t)(g.rows/4)) g_view -= bpr*(uint64_t)(g.rows/4); // center-ish
    g_selAddr = addr; g_selBit = 0;
    InvalidateRect(g_main,nullptr,FALSE);
}

static void attach_process(uint32_t pid, bool rw){
    auto ps = std::make_unique<ProcessSource>();
    if (!ps->open(pid, rw)){
        char m[128]; snprintf(m,sizeof(m),"OpenProcess failed (err %lu).\nTry another process or run elevated.",GetLastError());
        MessageBoxA(g_main,m,"bitforge",MB_ICONWARNING); return;
    }
    g_src = std::move(ps); g_scan.bind(g_src.get()); g_scan.reset(); g_frozen.clear();
    g_cols = 64; g_hunt=false; g_map=false;
    refresh_regions();
    if (!g_regionList.empty()) jump_to(g_regionList.front().base);
    SendMessage(g_results, LB_RESETCONTENT, 0, 0); g_shown.clear();
}
static void attach_file(const char* path, bool rw){
    auto fs = std::make_unique<FileSource>();
    if (!fs->open(path, rw)){ MessageBoxA(g_main,"CreateFile failed.","bitforge",MB_ICONWARNING); return; }
    g_src = std::move(fs); g_scan.bind(g_src.get()); g_scan.reset(); g_frozen.clear();
    g_cols = 64; g_hunt=false; g_map=false;
    refresh_regions();
    g_view=0; g_selAddr=0; g_selBit=0;
    SendMessage(g_results, LB_RESETCONTENT, 0, 0); g_shown.clear();
    InvalidateRect(g_main,nullptr,FALSE);
}

// ---- Arecibo easter egg: transmit (write) + SETI (detect) the message ----
static void do_arecibo(HWND h, bool announce){
    auto bs = std::make_unique<BufferSource>();
    if(!bs->alloc(4096,"Arecibo sandbox")){ MessageBoxA(h,"VirtualAlloc failed.","bitforge",MB_ICONWARNING); return; }
    uint64_t base = bs->base();
    bs->write(base, AR_MSG, sizeof(AR_MSG));               // literal bits into our own memory
    g_src = std::move(bs); g_scan.bind(g_src.get()); g_scan.reset(); g_frozen.clear();
    g_hunt=false; g_map=false;
    refresh_regions();
    SendMessage(g_results, LB_RESETCONTENT, 0, 0); g_shown.clear();
    RECT wr; GetWindowRect(h,&wr);
    SetWindowPos(h,nullptr,0,0,wr.right-wr.left,980,SWP_NOMOVE|SWP_NOZORDER);  // tall like the real bitmap
    RECT rc; GetClientRect(h,&rc);
    int gh = (int)(rc.bottom-BOTH-6-STATUSH-4);
    g_cols = AR_COLS;                                      // 23 -> the raw bits ARE the picture
    g_cell = std::max(4, std::min(16, gh/AR_ROWS - GAP));
    g_view = base; g_selAddr = base; g_selBit = 0;
    InvalidateRect(h,nullptr,FALSE);
    if(announce){
        char msg[400];
        snprintf(msg,sizeof(msg),
          "The Arecibo message now lives as literal bits in a fresh VirtualAlloc sandbox at 0x%llX "
          "(this process -- perfectly safe).\n\n"
          "1679 bits, 73 x 23. Population field: 4,292,853,750 (1974)  ->  %llu (2026).\n\n"
          "The grid is 23 columns wide, so the raw memory bits ARE the picture. Click a cell to edit "
          "humanity's message -- then hit SETI to listen for it.",
          (unsigned long long)base,(unsigned long long)AR_POP_NOW);
        MessageBoxA(h,msg,"bitforge  -  Arecibo easter egg",MB_ICONINFORMATION);
    }
}

// Is the full 1679-bit Arecibo message present at absolute bit offset startBit?
static bool arecibo_at(IByteSource& s, uint64_t startBit){
    for(int i=0;i<AR_BITS;++i){
        uint64_t gb = startBit + (uint64_t)i;
        uint8_t byte; if(s.read(gb>>3,&byte,1)!=1) return false;
        if(get_bit(&byte, gb&7) != get_bit(AR_MSG,(uint64_t)i)) return false;
    }
    return true;
}

// SETI: bit-search the attached source for a distinctive 64-bit Arecibo signature,
// then verify each candidate is the whole message before calling it a signal.
static void do_seti(HWND h, bool announce){
    if(!g_src){ MessageBoxA(h,"Point the dish somewhere first: attach a process, open a file, or run the Arecibo easter egg.","bitforge - SETI",MB_ICONINFORMATION); return; }
    const int off = 36;                                    // a dense, distinctive slice
    char sig[65]; for(int i=0;i<64;++i) sig[i] = get_bit(AR_MSG,(uint64_t)(off*8+i))?'1':'0'; sig[64]=0;
    g_scan.set_type(VType::Binary);
    g_scan.first_scan(sig);                                // unaligned bit search
    size_t echoes = g_scan.count();
    std::vector<Hit> confirmed;
    for(const Hit& hh : g_scan.hits()){
        uint64_t sigBit = hh.addr*8 + (uint64_t)hh.bit;
        if(sigBit < (uint64_t)off*8) continue;
        uint64_t startBit = sigBit - (uint64_t)off*8;
        if(arecibo_at(*g_src, startBit)){
            Hit c; c.addr = startBit>>3; c.bit = (uint8_t)(startBit&7); c.last = AR_POP_NOW;
            confirmed.push_back(c);
        }
    }
    SendMessage(g_results, LB_RESETCONTENT, 0, 0); g_shown.clear();
    char line[128];
    for(const Hit& c : confirmed){ g_shown.push_back(c);
        if(c.bit) snprintf(line,sizeof(line),">>> SIGNAL  %011llX.b%u",(unsigned long long)c.addr,c.bit);
        else      snprintf(line,sizeof(line),">>> SIGNAL  %011llX",(unsigned long long)c.addr);
        SendMessageA(g_results, LB_ADDSTRING, 0, (LPARAM)line);
    }
    if(announce){
        char msg[380];
        if(!confirmed.empty())
            snprintf(msg,sizeof(msg),"SETI scan of %s\n\n%zu confirmed Arecibo transmission(s) detected.\nFirst contact at 0x%llX.\n\nDouble-click a SIGNAL to jump to it.",
                     g_src->label().c_str(), confirmed.size(), (unsigned long long)confirmed.front().addr);
        else
            snprintf(msg,sizeof(msg),"SETI scan of %s\n\nNo Arecibo transmission found (%zu signature echo(es), none verified).\n\nRun the Arecibo easter egg to transmit one, then scan again.",
                     g_src->label().c_str(), echoes);
        MessageBoxA(h,msg,"bitforge  -  SETI scanner",MB_ICONINFORMATION);
    }
}

// ================= SETI hunt: a memory-sweeping screensaver =================
// A SETI@home-style waterfall that sweeps the attached source's memory looking
// for structured "signals"; when it finds and verifies the real Arecibo message
// it locks on (CONTACT) and transmits our own alien reply back into memory.
static bool     g_contact=false, g_reply=false;
static uint64_t g_contactAddr=0, g_replyAddr=0;
static const int HB_BINS=48, HB_HIST=150;               // waterfall bins x history
static std::vector<uint8_t> g_wf; static int g_wfHead=0;
static uint64_t g_huntTotal=0, g_huntSwept=0, g_huntCurAddr=0, g_huntOff=0;
static size_t   g_huntRegion=0; static long g_huntChunks=0, g_huntCand=0; static int g_huntBest=0;
static std::vector<Region> g_huntRegions;
static char     g_hlog[5][96]; static int g_hlogN=0;
static HBRUSH   g_ramp[24]={0};

static void heat_ramp(int v,int&r,int&g,int&b){        // v 0..255 -> SETI heat colour
    if(v<64){ r=0; g=0; b=40+v*3; }
    else if(v<128){ int t=v-64; r=0; g=t*4; b=255-t*3; }
    else if(v<192){ int t=v-128; r=t*4; g=255; b=0; }
    else { int t=v-192; r=255; g=255-t*3; b=t*2; }
    if(r>255)r=255; if(g>255)g=255; if(g<0)g=0; if(b>255)b=255; if(b<0)b=0;
}
static void ensure_ramp(){
    if(g_ramp[0]) return;
    for(int i=0;i<24;i++){ int r,g,b; heat_ramp(i*255/23,r,g,b); g_ramp[i]=CreateSolidBrush(RGB(r,g,b)); }
}
static void hunt_log(const char* s){
    if(g_hlogN<5){ strncpy(g_hlog[g_hlogN],s,95); g_hlog[g_hlogN][95]=0; g_hlogN++; }
    else { for(int i=0;i<4;i++) memcpy(g_hlog[i],g_hlog[i+1],96); strncpy(g_hlog[4],s,95); g_hlog[4][95]=0; }
}
static const char* seti_class(uint32_t s){ static const char* k[]={"Gaussian","Pulse","Triplet","Spike"}; return k[s&3]; }

static void on_contact(){
    char l[96]; snprintf(l,sizeof(l),">>> CONTACT: Arecibo signal @ 0x%llX",(unsigned long long)g_contactAddr); hunt_log(l);
    unsigned char reply[210]; generate_alien_reply(reply,(uint32_t)(g_contactAddr^0x5e71u));
    uint64_t rpos=(g_contactAddr + sizeof(AR_MSG) + 4095) & ~(uint64_t)4095;   // next 4K page
    if(g_src && g_src->write(rpos,reply,sizeof(reply))==sizeof(reply)){
        g_reply=true; g_replyAddr=rpos;
        snprintf(l,sizeof(l),"<<< REPLY transmitted @ 0x%llX",(unsigned long long)rpos); hunt_log(l);
    } else hunt_log("reply held: source is read-only");
}

static void hunt_tick(){
    if(g_huntRegions.empty()) return;
    const int PERTICK=3; const size_t CH=4096;
    static std::vector<uint8_t> buf;
    for(int t=0;t<PERTICK;t++){
        Region& r=g_huntRegions[g_huntRegion];
        if(g_huntOff>=r.size){ g_huntOff=0; g_huntRegion=(g_huntRegion+1)%g_huntRegions.size(); if(g_huntRegion==0) g_huntSwept=0; continue; }
        uint64_t addr=r.base+g_huntOff;
        size_t want=(size_t)((r.size-g_huntOff<CH)?(r.size-g_huntOff):CH);
        buf.assign(want,0);
        size_t got=g_src->read(addr,buf.data(),want);
        g_huntCurAddr=addr;
        uint8_t spec[HB_BINS];
        for(int i=0;i<HB_BINS;i++){
            size_t s=(size_t)i*got/HB_BINS, e=(size_t)(i+1)*got/HB_BINS; uint64_t pc=0;
            for(size_t k=s;k<e;k++) pc+=popcount8(buf[k]);
            spec[i]=(e>s)?(uint8_t)(pc*255/((e-s)*8)):0;
        }
        memcpy(&g_wf[(size_t)g_wfHead*HB_BINS],spec,HB_BINS); g_wfHead=(g_wfHead+1)%HB_HIST;
        int sum=0,peak=0; for(int i=0;i<HB_BINS;i++){ sum+=spec[i]; if(spec[i]>peak)peak=spec[i]; }
        int mean=sum/HB_BINS; int score=peak*10/(mean+1);
        if(score>g_huntBest) g_huntBest=score;
        if(score>=16 && (g_huntChunks%13==0)){ char l[96]; snprintf(l,sizeof(l)," candidate: %s @ 0x%llX  power %d",seti_class((uint32_t)(addr>>7)),(unsigned long long)addr,score); hunt_log(l); g_huntCand++; }
        if(!g_contact){
            for(size_t b=0;b+8<=got;b++){
                if(memcmp(&buf[b],AR_MSG+36,8)==0){
                    uint64_t sb=addr+b; if(sb>=36){ sb-=36; if(arecibo_at(*g_src,sb*8)){ g_contact=true; g_contactAddr=sb; on_contact(); } }
                    break;
                }
            }
        }
        g_huntChunks++; g_huntSwept+=want; g_huntOff+=want;
    }
}

static void hunt_render(HDC mem,int gw,int gh){
    ensure_ramp();
    SelectObject(mem,g_font); SetBkMode(mem,TRANSPARENT);
    int specH=96, wfTop=specH+20, wfH=gh-wfTop-4; if(wfH<10) wfH=10;
    int latest=(g_wfHead-1+HB_HIST)%HB_HIST;
    // power spectrum
    for(int i=0;i<HB_BINS;i++){
        int v=g_wf.empty()?0:g_wf[(size_t)latest*HB_BINS+i];
        int bh=v*specH/255, x0=i*gw/HB_BINS, x1=(i+1)*gw/HB_BINS-1;
        RECT br={x0,specH-bh,x1,specH}; FillRect(mem,&br,g_ramp[v*24/256]);
    }
    { int sx=(int)((g_huntChunks*7)%(gw>0?gw:1)); HPEN pen=CreatePen(PS_SOLID,1,RGB(0,255,120));
      HGDIOBJ op=SelectObject(mem,pen); MoveToEx(mem,sx,0,0); LineTo(mem,sx,specH); SelectObject(mem,op); DeleteObject(pen); }
    // waterfall
    int rowH=wfH/HB_HIST; if(rowH<1) rowH=1;
    for(int j=0;j<HB_HIST;j++){
        int ring=(g_wfHead-1-j+HB_HIST*2)%HB_HIST, y0=wfTop+j*rowH;
        for(int i=0;i<HB_BINS;i++){
            int v=g_wf.empty()?0:g_wf[(size_t)ring*HB_BINS+i]; if(v<10) continue;
            RECT cr={i*gw/HB_BINS,y0,(i+1)*gw/HB_BINS,y0+rowH}; FillRect(mem,&cr,g_ramp[v*24/256]);
        }
    }
    // telemetry
    char t[180]; int pct=g_huntTotal?(int)(g_huntSwept*100/g_huntTotal):0;
    SetTextColor(mem,RGB(0,255,140));
    snprintf(t,sizeof(t),"SETI :: hunting %s", g_src?g_src->label().c_str():"nothing"); TextOutA(mem,6,4,t,(int)strlen(t));
    snprintf(t,sizeof(t),"sweep %011llX   %d%%   chunks %ld   candidates %ld   peak %d",
             (unsigned long long)g_huntCurAddr,pct,g_huntChunks,g_huntCand,g_huntBest); TextOutA(mem,6,specH+2,t,(int)strlen(t));
    SetTextColor(mem,RGB(110,210,120));
    for(int i=0;i<g_hlogN;i++) TextOutA(mem,6, gh-4-(g_hlogN-i)*16, g_hlog[i],(int)strlen(g_hlog[i]));
    // contact banner
    if(g_contact){
        int bw=gw-40,bh=64,bx=20,by=gh/2-52;
        RECT box={bx,by,bx+bw,by+bh}; HBRUSH bb=CreateSolidBrush(RGB(16,54,20)); FillRect(mem,&box,bb); DeleteObject(bb);
        HPEN pen=CreatePen(PS_SOLID,2,RGB(0,255,120)); HGDIOBJ op=SelectObject(mem,pen); HGDIOBJ ob=SelectObject(mem,GetStockObject(NULL_BRUSH));
        Rectangle(mem,bx,by,bx+bw,by+bh); SelectObject(mem,ob); SelectObject(mem,op); DeleteObject(pen);
        SetTextColor(mem,RGB(140,255,170));
        snprintf(t,sizeof(t),"  >>> CONTACT -- ARECIBO SIGNAL @ 0x%llX",(unsigned long long)g_contactAddr); TextOutA(mem,bx+10,by+12,t,(int)strlen(t));
        if(g_reply) snprintf(t,sizeof(t),"  reply transmitted @ 0x%llX  (our own message is now in memory)",(unsigned long long)g_replyAddr);
        else        snprintf(t,sizeof(t),"  population field decodes to today's Earth: 8,303,169,803");
        TextOutA(mem,bx+10,by+36,t,(int)strlen(t));
    }
}

static void hunt_setup_demo(){                          // 2 MB of noise with a planted Arecibo message
    auto bs=std::make_unique<BufferSource>();
    if(!bs->alloc(2u*1024*1024,"SETI hunt space")) return;
    uint8_t* p=bs->data(); size_t N=2u*1024*1024;
    uint32_t s=2463534242u; auto rnd=[&](){ s^=s<<13; s^=s>>17; s^=s<<5; return s; };
    for(size_t i=0;i<N;i++){ uint32_t x=rnd(); p[i]=(uint8_t)((x&1)?((x>>3)&0x0F):((x>>5)&0xFF)); }
    size_t plant=((size_t)(N*0.68))&~(size_t)4095; memcpy(p+plant,AR_MSG,sizeof(AR_MSG));
    g_src=std::move(bs); g_scan.bind(g_src.get()); g_scan.reset(); g_frozen.clear(); refresh_regions();
}
static void enter_hunt(){
    if(!g_src) hunt_setup_demo();
    if(!g_src) return;
    g_huntRegions=g_src->regions();
    g_huntTotal=0; for(auto&r:g_huntRegions) g_huntTotal+=r.size;
    g_huntRegion=0; g_huntOff=0; g_huntSwept=0; g_huntChunks=0; g_huntCand=0; g_huntBest=0;
    g_contact=false; g_reply=false; g_wf.assign((size_t)HB_HIST*HB_BINS,0); g_wfHead=0; g_hlogN=0;
    hunt_log("listening... sweeping local memory for structured signals");
    g_map=false; g_hunt=true; SetWindowTextA(g_main,"bitforge  ::  SETI hunt");
}
static void exit_hunt(){ g_hunt=false; if(g_src) refresh_regions(); InvalidateRect(g_main,nullptr,FALSE); }

// ================= structure map: entropy overview (binvis / Veles style) =================
// Render a whole region as a 2-D map where each cell is a block of bytes coloured by
// local Shannon entropy, laid out along a Hilbert curve so nearby offsets stay spatially
// adjacent (structure blooms into blobs). Click a cell to drill down into its bits.
static bool     g_mapHilbert=true;
static const int MAP_N=128;
static uint64_t g_mapBase=0, g_mapSpan=0, g_mapBlock=1;
static std::vector<uint8_t> g_mapEnt;

static void hilbert_d2xy(int n,uint32_t d,int&x,int&y){ int rx,ry; uint32_t t=d; x=0; y=0;
    for(int s=1;s<n;s*=2){ rx=1&(int)(t/2); ry=1&(int)(t^(uint32_t)rx);
        if(ry==0){ if(rx==1){ x=s-1-x; y=s-1-y; } int tmp=x; x=y; y=tmp; }
        x+=s*rx; y+=s*ry; t/=4; } }
static uint32_t hilbert_xy2d(int n,int x,int y){ int rx,ry; uint32_t d=0;
    for(int s=n/2;s>0;s/=2){ rx=(x&s)>0?1:0; ry=(y&s)>0?1:0; d+=(uint32_t)s*(uint32_t)s*(uint32_t)((3*rx)^ry);
        if(ry==0){ if(rx==1){ x=s-1-x; y=s-1-y; } int tmp=x; x=y; y=tmp; } } return d; }

static uint8_t buf_entropy(const uint8_t* p,size_t n){ if(!n) return 0; int c[256]; memset(c,0,sizeof(c));
    for(size_t i=0;i<n;i++) c[p[i]]++; double e=0;
    for(int i=0;i<256;i++){ if(!c[i]) continue; double pr=(double)c[i]/(double)n; e-=pr*log2(pr); }
    int v=(int)(e/8.0*255.0); if(v<0)v=0; if(v>255)v=255; return (uint8_t)v; }

static void map_geo(int gw,int gh,int&ox,int&oy,int&cell){ int side=(gw<gh?gw:gh)-40; if(side<MAP_N) side=MAP_N; cell=side/MAP_N; if(cell<1)cell=1; ox=8; oy=30; }

static void enter_map(){
    if(!g_src) return;
    auto rs=g_src->regions(); if(rs.empty()) return;
    Region reg=rs[0]; for(auto&r:rs){ if(g_view>=r.base && g_view<r.base+r.size){ reg=r; break; } }
    g_mapBase=reg.base; g_mapSpan=reg.size;
    int cells=MAP_N*MAP_N; g_mapBlock=g_mapSpan/(uint64_t)cells; if(g_mapBlock<1) g_mapBlock=1;
    g_mapEnt.assign(cells,0);
    size_t S=(size_t)(g_mapBlock<512?g_mapBlock:512); if(S<1)S=1;
    std::vector<uint8_t> sb(S);
    for(int i=0;i<cells;i++){ uint64_t a=g_mapBase+(uint64_t)i*g_mapBlock; size_t got=g_src->read(a,sb.data(),S); g_mapEnt[i]=buf_entropy(sb.data(),got); }
    g_map=true; g_hunt=false; SetWindowTextA(g_main,"bitforge  ::  structure map");
}
static void exit_map(){ g_map=false; if(g_src) refresh_regions(); InvalidateRect(g_main,nullptr,FALSE); }

static void map_render(HDC mem,int gw,int gh){
    ensure_ramp(); SelectObject(mem,g_font); SetBkMode(mem,TRANSPARENT);
    int ox,oy,cell; map_geo(gw,gh,ox,oy,cell); int px=cell*MAP_N;
    for(int d=0;d<MAP_N*MAP_N;d++){ int x,y; if(g_mapHilbert) hilbert_d2xy(MAP_N,(uint32_t)d,x,y); else { x=d%MAP_N; y=d/MAP_N; }
        int v=g_mapEnt[d]; RECT r={ox+x*cell,oy+y*cell,ox+x*cell+cell,oy+y*cell+cell}; FillRect(mem,&r,g_ramp[v*24/256]); }
    char t[200]; SetTextColor(mem,RGB(200,220,255));
    snprintf(t,sizeof(t),"MAP :: entropy  -  %s layout  -  base %011llX  span %.2f MB  block %llu B",
        g_mapHilbert?"Hilbert":"linear",(unsigned long long)g_mapBase,g_mapSpan/1048576.0,(unsigned long long)g_mapBlock);
    TextOutA(mem,8,6,t,(int)strlen(t));
    SetTextColor(mem,RGB(140,150,170));
    { const char* s="[H] Hilbert/linear     click a cell to drill into its bits     [Esc] exit"; TextOutA(mem,ox,oy+px+8,s,(int)strlen(s)); }
    int lx=ox+px+24, lw=gw-lx-16;
    if(lw>48){ for(int i=0;i<24;i++){ RECT r={lx+i*lw/24,oy,lx+(i+1)*lw/24,oy+14}; FillRect(mem,&r,g_ramp[i]); }
        SetTextColor(mem,RGB(160,170,190)); TextOutA(mem,lx,oy+18,"low  <--  entropy  -->  high",28); }
}

static void populate_results(){
    SendMessage(g_results, LB_RESETCONTENT, 0, 0);
    g_shown.clear();
    char line[160];
    size_t n=0;
    for (const Hit& h : g_scan.hits()){
        g_shown.push_back(h);
        if (h.bit) snprintf(line,sizeof(line),"%011llX.b%u = %llu",(unsigned long long)h.addr,h.bit,(unsigned long long)h.last);
        else       snprintf(line,sizeof(line),"%011llX = %llu",(unsigned long long)h.addr,(unsigned long long)h.last);
        SendMessageA(g_results, LB_ADDSTRING, 0, (LPARAM)line);
        if (++n >= 5000) break;
    }
    char cap[96]; snprintf(cap,sizeof(cap),"First Scan  [%zu hits%s]", g_scan.count(), g_scan.truncated()?"+":"");
    SetWindowTextA(g_first, cap);
}

static VType cur_type(){ int i=(int)SendMessage(g_type,CB_GETCURSEL,0,0); if(i<0)i=2; return kTypes[i]; }
static Cmp   cur_cmp (){ int i=(int)SendMessage(g_cmp,CB_GETCURSEL,0,0); if(i<0)i=0; return kCmps[i]; }
static std::string edit_text(){ char b[128]; GetWindowTextA(g_value,b,sizeof(b)); return b; }

static void do_first(){
    if(!g_src){ MessageBoxA(g_main,"Attach a process or open a file first.","bitforge",MB_ICONINFORMATION); return; }
    g_scan.set_type(cur_type());
    if(!g_scan.first_scan(edit_text())){ MessageBoxA(g_main,"Bad value/pattern for this type.","bitforge",MB_ICONWARNING); return; }
    populate_results();
}
static void do_next(){
    if(!g_src) return;
    g_scan.next_scan(cur_cmp(), edit_text());
    populate_results();
    char cap[96]; snprintf(cap,sizeof(cap),"Next Scan  [%zu]", g_scan.count()); SetWindowTextA(g_next,cap);
}
static void do_freeze(){
    int idx=(int)SendMessage(g_results,LB_GETCURSEL,0,0);
    if(idx<0||idx>=(int)g_shown.size()) return;
    Hit h=g_shown[idx]; uint64_t v; if(!g_scan.read_value(h,v)) return;
    int w=Scanner::width_of(g_scan.type()); if(w<=0) return;   // numeric only
    g_frozen.push_back(Frozen{h.addr,w,v});
    char cap[64]; snprintf(cap,sizeof(cap),"Freeze+ [%zu]",g_frozen.size()); SetWindowTextA(g_freeze,cap);
}
static void apply_frozen(){
    if(!g_src) return;
    for(auto&f:g_frozen){ uint8_t b[8]; for(int i=0;i<f.width;++i) b[i]=(uint8_t)(f.val>>(8*i)); g_src->write(f.addr,b,f.width); }
}

// ---- grid rendering ----
static void draw(HWND h, HDC hdc){
    RECT rc; GetClientRect(h,&rc);
    // parent background (WS_CLIPCHILDREN keeps controls safe)
    FillRect(hdc, &rc, g_bBg);
    SelectObject(hdc, g_font); SetBkMode(hdc, TRANSPARENT); SetTextColor(hdc, RGB(200,200,205));
    { const char* s="Processes  (dbl-click = attach)"; TextOutA(hdc, 6, 4,   s,(int)strlen(s)); }
    { const char* s="Regions  (click = jump)";         TextOutA(hdc, 6, 236, s,(int)strlen(s)); }

    Geo g = geo(h);
    int gw=g.area.right-g.area.left, gh=g.area.bottom-g.area.top;
    if (gw<10||gh<10) return;

    // offscreen buffer for the whole grid area (no flicker)
    HDC mem=CreateCompatibleDC(hdc); HBITMAP bmp=CreateCompatibleBitmap(hdc,gw,gh);
    HBITMAP old=(HBITMAP)SelectObject(mem,bmp);
    RECT full={0,0,gw,gh}; FillRect(mem,&full,g_bBg);
    SelectObject(mem,g_font); SetBkMode(mem,TRANSPARENT);

    if (g_hunt){
        hunt_render(mem, gw, gh);
    } else if (g_map){
        map_render(mem, gw, gh);
    } else if (!g_src){
        SetTextColor(mem,RGB(150,150,155));
        { const char* s="Attach a process (left) or Open File, then click bits to toggle them."; TextOutA(mem, 12, 12, s,(int)strlen(s)); }
    } else {
        int nbytes = (g.rows*g.cols + 7)/8;   // works for any cols (e.g. 23 for Arecibo)
        g_buf.assign(nbytes,0);
        size_t got = g_src->read(g_view, g_buf.data(), nbytes);

        // live change-heat: bits that flipped since the last frame glow, then cool off
        static std::vector<uint8_t> prevBuf; static std::vector<uint16_t> heat;
        static uint64_t prevView=~0ull; static int prevCols=0;
        int totalBits = g.rows*g.cols;
        if(prevView!=g_view || prevCols!=g.cols || (int)heat.size()!=totalBits){
            heat.assign(totalBits,0); prevBuf=g_buf; prevView=g_view; prevCols=g.cols;
        }
        for(int i=0;i<totalBits;++i){
            int cur=((size_t)(i>>3)<got)? get_bit(g_buf.data(),(uint64_t)i):0;
            int pv =((size_t)(i>>3)<prevBuf.size())? get_bit(prevBuf.data(),(uint64_t)i):0;
            if(cur!=pv) heat[i]=28; else if(heat[i]>0) heat[i]--;
        }
        prevBuf=g_buf;

        for (int row=0; row<g.rows; ++row){
            for (int col=0; col<g.cols; ++col){
                int bitInView = row*g.cols + col;
                int byteOff = bitInView>>3;
                if ((size_t)byteOff >= got){
                    // unknown / unreadable slice
                    RECT r={col*g.step, row*g.step, col*g.step+g_cell, row*g.step+g_cell};
                    FillRect(mem,&r,g_bUnk); continue;
                }
                int val = get_bit(g_buf.data(), (uint64_t)bitInView);
                uint64_t addr = g_view + byteOff;
                int bib = bitInView & 7;
                bool selByte = (addr==g_selAddr);
                uint16_t ht = (bitInView<(int)heat.size())? heat[bitInView] : 0;
                HBRUSH br = ht ? (ht>19?g_heat3 : ht>9?g_heat2 : g_heat1)
                              : (val ? (selByte?g_b1s:g_b1) : (selByte?g_b0s:g_b0));
                RECT r={col*g.step, row*g.step, col*g.step+g_cell, row*g.step+g_cell};
                FillRect(mem,&r,br);
                if (selByte && bib==g_selBit){
                    HPEN pen=CreatePen(PS_SOLID,2,RGB(255,235,120)); HGDIOBJ op=SelectObject(mem,pen);
                    HGDIOBJ ob=SelectObject(mem,GetStockObject(NULL_BRUSH));
                    Rectangle(mem,r.left-1,r.top-1,r.right+1,r.bottom+1);
                    SelectObject(mem,ob); SelectObject(mem,op); DeleteObject(pen);
                }
            }
        }
        // status strip inside the buffer
        uint8_t sb=0; bool have = g_src->read(g_selAddr,&sb,1)==1;
        char bits[9]; for(int k=0;k<8;++k) bits[k]= (sb>>(7-k))&1 ? '1':'0'; bits[8]=0;
        char st1[220], st2[220];
        snprintf(st1,sizeof(st1),"addr %011llX  bit %d = %d   byte 0x%02X  %s  '%c'",
                 (unsigned long long)g_selAddr, g_selBit, have?((sb>>(7-g_selBit))&1):0,
                 sb, bits, (sb>=32&&sb<127)?sb:'.');
        snprintf(st2,sizeof(st2),"%s   view %011llX   cell %dpx  cols %d   frozen %zu   [%s]",
                 g_src->writable()?"RW  click=TOGGLE":"RO  read-only",
                 (unsigned long long)g_view, g_cell, g_cols, g_frozen.size(), g_src->kind());
        SetTextColor(mem,RGB(120,200,255)); TextOutA(mem, 2, gh-STATUSH+2, st1,(int)strlen(st1));
        SetTextColor(mem,RGB(160,160,170)); TextOutA(mem, 2, gh-STATUSH+20, st2,(int)strlen(st2));
    }

    BitBlt(hdc, g.area.left, g.area.top, gw, gh, mem, 0, 0, SRCCOPY);
    SelectObject(mem,old); DeleteObject(bmp); DeleteDC(mem);
}

// map a client point to a bit; returns false if outside the cell grid
static bool hit_cell(HWND h, int mx, int my, uint64_t& addr, int& bit){
    Geo g=geo(h);
    int lx=mx-g.area.left, ly=my-g.area.top;
    if (lx<0||ly<0||my>g.cellsBottom) return false;
    int col=lx/g.step, row=ly/g.step;
    if (col>=g.cols) return false;
    int bitInView=row*g.cols+col;
    addr=g_view+(bitInView>>3); bit=bitInView&7;
    return true;
}

static void scroll_rows(HWND h, int rows){
    Geo g=geo(h); int64_t d=(int64_t)rows*g.bytesPerRow;
    if (d<0 && (uint64_t)(-d)>g_view) g_view=0; else g_view=(uint64_t)((int64_t)g_view+d);
    InvalidateRect(h,nullptr,FALSE);
}

LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp){
    switch(msg){
    case WM_CREATE: {
        g_font=CreateFontA(14,0,0,0,FW_NORMAL,0,0,0,DEFAULT_CHARSET,OUT_DEFAULT_PRECIS,
                           CLIP_DEFAULT_PRECIS,DEFAULT_QUALITY,FIXED_PITCH|FF_MODERN,"Consolas");
        g_bBg =CreateSolidBrush(RGB(18,18,22));
        g_b0  =CreateSolidBrush(RGB(34,34,40));   g_b1 =CreateSolidBrush(RGB(40,130,220));
        g_b0s =CreateSolidBrush(RGB(80,66,30));   g_b1s=CreateSolidBrush(RGB(245,195,45));
        g_bUnk=CreateSolidBrush(RGB(48,40,40));
        g_heat1=CreateSolidBrush(RGB(120,72,22)); g_heat2=CreateSolidBrush(RGB(205,120,32)); g_heat3=CreateSolidBrush(RGB(255,196,64));
        HINSTANCE hi=((LPCREATESTRUCT)lp)->hInstance;
        g_procs   =CreateWindowExA(WS_EX_CLIENTEDGE,"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY|LBS_USETABSTOPS,0,0,10,10,h,(HMENU)ID_PROCS,hi,nullptr);
        g_refresh =CreateWindowExA(0,"BUTTON","Refresh",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_REFRESH,hi,nullptr);
        g_rw      =CreateWindowExA(0,"BUTTON","writable",WS_CHILD|WS_VISIBLE|BS_AUTOCHECKBOX,0,0,10,10,h,(HMENU)ID_RW,hi,nullptr);
        g_openfile=CreateWindowExA(0,"BUTTON","Open File",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_OPENFILE,hi,nullptr);
        g_regions =CreateWindowExA(WS_EX_CLIENTEDGE,"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY,0,0,10,10,h,(HMENU)ID_REGIONS,hi,nullptr);
        g_type    =CreateWindowExA(0,"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,10,220,h,(HMENU)ID_TYPE,hi,nullptr);
        g_value   =CreateWindowExA(WS_EX_CLIENTEDGE,"EDIT","100",WS_CHILD|WS_VISIBLE|ES_AUTOHSCROLL,0,0,10,10,h,(HMENU)ID_VALUE,hi,nullptr);
        g_first   =CreateWindowExA(0,"BUTTON","First Scan",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_FIRST,hi,nullptr);
        g_cmp     =CreateWindowExA(0,"COMBOBOX",nullptr,WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST|WS_VSCROLL,0,0,10,160,h,(HMENU)ID_CMP,hi,nullptr);
        g_next    =CreateWindowExA(0,"BUTTON","Next Scan",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_NEXT,hi,nullptr);
        g_freeze  =CreateWindowExA(0,"BUTTON","Freeze+",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_FREEZE,hi,nullptr);
        g_clearfrz=CreateWindowExA(0,"BUTTON","Clear frz",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_CLEARFRZ,hi,nullptr);
        g_arecibo =CreateWindowExA(0,"BUTTON","Arecibo",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_ARECIBO,hi,nullptr);
        g_seti    =CreateWindowExA(0,"BUTTON","SETI",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_SETI,hi,nullptr);
        g_huntbtn =CreateWindowExA(0,"BUTTON","Hunt",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_HUNT,hi,nullptr);
        g_mapbtn  =CreateWindowExA(0,"BUTTON","Map",WS_CHILD|WS_VISIBLE,0,0,10,10,h,(HMENU)ID_MAP,hi,nullptr);
        g_results =CreateWindowExA(WS_EX_CLIENTEDGE,"LISTBOX",nullptr,WS_CHILD|WS_VISIBLE|WS_VSCROLL|LBS_NOTIFY,0,0,10,10,h,(HMENU)ID_RESULTS,hi,nullptr);
        HWND all[]={g_procs,g_refresh,g_rw,g_openfile,g_regions,g_type,g_value,g_first,g_cmp,g_next,g_freeze,g_clearfrz,g_arecibo,g_seti,g_huntbtn,g_mapbtn,g_results};
        for(HWND c:all) set_font(c);
        for(auto n:{"u8","u16","u32","u64","i8","i16","i32","i64","f32","f64","bits"}) SendMessageA(g_type,CB_ADDSTRING,0,(LPARAM)n);
        SendMessage(g_type,CB_SETCURSEL,2,0); // u32
        for(auto n:{"Exact","Unchanged","Changed","Increased","Decreased"}) SendMessageA(g_cmp,CB_ADDSTRING,0,(LPARAM)n);
        SendMessage(g_cmp,CB_SETCURSEL,0,0);
        refresh_procs();
        SetTimer(h,ID_TIMER,33,nullptr);
        return 0;
    }
    case WM_SIZE: {
        RECT rc; GetClientRect(h,&rc); int W=rc.right,H=rc.bottom;
        MoveWindow(g_procs,   4,20,  LEFTW-8,180,TRUE);
        MoveWindow(g_refresh, 4,204, 80,24,TRUE);
        MoveWindow(g_rw,      90,206, 80,20,TRUE);
        MoveWindow(g_openfile,176,204,LEFTW-180,24,TRUE);
        MoveWindow(g_regions, 4,252, LEFTW-8, H-256,TRUE);
        int y1=H-BOTH+6;
        MoveWindow(g_type,  RX+4,  y1, 64,220,TRUE);
        MoveWindow(g_value, RX+72, y1, 150,24,TRUE);
        MoveWindow(g_first, RX+228,y1, 96,24,TRUE);
        MoveWindow(g_cmp,   RX+330,y1, 104,160,TRUE);
        MoveWindow(g_next,  RX+440,y1, 90,24,TRUE);
        MoveWindow(g_freeze,RX+536,y1, 84,24,TRUE);
        MoveWindow(g_clearfrz,RX+624,y1,84,24,TRUE);
        MoveWindow(g_arecibo, RX+716,y1,92,24,TRUE);
        MoveWindow(g_seti,    RX+812,y1,72,24,TRUE);
        MoveWindow(g_huntbtn, RX+888,y1,72,24,TRUE);
        MoveWindow(g_mapbtn,  RX+964,y1,72,24,TRUE);
        MoveWindow(g_results, RX+4, y1+30, W-RX-8, BOTH-40,TRUE);
        InvalidateRect(h,nullptr,FALSE);
        return 0;
    }
    case WM_COMMAND: {
        int id=LOWORD(wp), code=HIWORD(wp);
        if(id==ID_REFRESH && code==BN_CLICKED) refresh_procs();
        else if(id==ID_PROCS && code==LBN_DBLCLK){
            int i=(int)SendMessage(g_procs,LB_GETCURSEL,0,0);
            if(i>=0&&i<(int)g_procList.size()) attach_process(g_procList[i].pid, SendMessage(g_rw,BM_GETCHECK,0,0)==BST_CHECKED);
        }
        else if(id==ID_REGIONS && code==LBN_SELCHANGE){
            int i=(int)SendMessage(g_regions,LB_GETCURSEL,0,0);
            if(i>=0&&i<(int)g_regionList.size()) jump_to(g_regionList[i].base);
        }
        else if(id==ID_OPENFILE && code==BN_CLICKED){
            char path[MAX_PATH]=""; OPENFILENAMEA ofn{}; ofn.lStructSize=sizeof(ofn); ofn.hwndOwner=h;
            ofn.lpstrFilter="All files\0*.*\0"; ofn.lpstrFile=path; ofn.nMaxFile=MAX_PATH;
            ofn.Flags=OFN_FILEMUSTEXIST|OFN_PATHMUSTEXIST;
            if(GetOpenFileNameA(&ofn)) attach_file(path, SendMessage(g_rw,BM_GETCHECK,0,0)==BST_CHECKED);
        }
        else if(id==ID_FIRST && code==BN_CLICKED) do_first();
        else if(id==ID_NEXT  && code==BN_CLICKED) do_next();
        else if(id==ID_FREEZE&& code==BN_CLICKED) do_freeze();
        else if(id==ID_CLEARFRZ&&code==BN_CLICKED){ g_frozen.clear(); SetWindowTextA(g_freeze,"Freeze+"); }
        else if(id==ID_ARECIBO && code==BN_CLICKED) do_arecibo(h,true);
        else if(id==ID_SETI    && code==BN_CLICKED) do_seti(h,true);
        else if(id==ID_HUNT    && code==BN_CLICKED){ if(g_hunt) exit_hunt(); else enter_hunt(); }
        else if(id==ID_MAP     && code==BN_CLICKED){ if(g_map) exit_map(); else enter_map(); }
        else if(id==ID_RESULTS && code==LBN_DBLCLK){
            int i=(int)SendMessage(g_results,LB_GETCURSEL,0,0);
            if(i>=0&&i<(int)g_shown.size()){ jump_to(g_shown[i].addr); g_selBit=g_shown[i].bit; }
        }
        return 0;
    }
    case WM_LBUTTONDOWN: case WM_RBUTTONDOWN: {
        if(g_map && msg==WM_LBUTTONDOWN){
            Geo g=geo(h); int gw=g.area.right-g.area.left, gh=g.area.bottom-g.area.top; int ox,oy,cell; map_geo(gw,gh,ox,oy,cell);
            int cx=(GET_X_LPARAM(lp)-g.area.left-ox)/cell, cy=(GET_Y_LPARAM(lp)-g.area.top-oy)/cell;
            if(cx>=0&&cx<MAP_N&&cy>=0&&cy<MAP_N){ uint32_t d=g_mapHilbert?hilbert_xy2d(MAP_N,cx,cy):(uint32_t)(cy*MAP_N+cx);
                uint64_t a=g_mapBase+(uint64_t)d*g_mapBlock; g_map=false; g_cols=64; g_view=(a/8)*8; g_selAddr=a; g_selBit=0;
                if(g_src) refresh_regions(); InvalidateRect(h,nullptr,FALSE); }
            return 0;
        }
        uint64_t addr; int bit;
        if(hit_cell(h,GET_X_LPARAM(lp),GET_Y_LPARAM(lp),addr,bit)){
            g_selAddr=addr; g_selBit=bit;
            if(msg==WM_LBUTTONDOWN && g_src && g_src->writable())
                toggle_bit_src(*g_src,addr,bit);
            InvalidateRect(h,nullptr,FALSE);
        }
        return 0;
    }
    case WM_MOUSEWHEEL: {
        int d=GET_WHEEL_DELTA_WPARAM(wp)/WHEEL_DELTA;
        scroll_rows(h,-d*3);
        return 0;
    }
    case WM_KEYDOWN: {
        if(g_map){ if(wp=='H'){ g_mapHilbert=!g_mapHilbert; InvalidateRect(h,0,FALSE); } else if(wp==VK_ESCAPE) exit_map(); return 0; }
        switch(wp){
            case VK_PRIOR: scroll_rows(h,-8); break;
            case VK_NEXT:  scroll_rows(h, 8); break;
            case VK_LEFT:  if(g_selBit>0)g_selBit--; else if(g_selAddr>0){g_selAddr--;g_selBit=7;} InvalidateRect(h,0,FALSE); break;
            case VK_RIGHT: if(g_selBit<7)g_selBit++; else {g_selAddr++;g_selBit=0;} InvalidateRect(h,0,FALSE); break;
            case VK_UP:    { Geo g=geo(h); uint64_t d=g.bytesPerRow; if(g_selAddr>=d)g_selAddr-=d; InvalidateRect(h,0,FALSE);} break;
            case VK_DOWN:  { Geo g=geo(h); g_selAddr+=g.bytesPerRow; InvalidateRect(h,0,FALSE);} break;
            case VK_SPACE: if(g_src&&g_src->writable()){toggle_bit_src(*g_src,g_selAddr,g_selBit);InvalidateRect(h,0,FALSE);} break;
            case VK_OEM_PLUS: case VK_ADD:  if(g_cell<28)g_cell++; InvalidateRect(h,0,FALSE); break;
            case VK_OEM_MINUS:case VK_SUBTRACT: if(g_cell>3)g_cell--; InvalidateRect(h,0,FALSE); break;
        }
        return 0;
    }
    case WM_TIMER:
        if(wp==ID_TIMER){
            if(g_hunt) hunt_tick(); else apply_frozen();
            if(g_hunt || (g_src && !g_map)){ RECT rc;GetClientRect(h,&rc); RECT gr={RX,4,rc.right,rc.bottom-BOTH-6}; InvalidateRect(h,&gr,FALSE);} }
        return 0;
    case WM_ERASEBKGND: return 1;   // we paint everything (WS_CLIPCHILDREN)
    case WM_PAINT: { PAINTSTRUCT ps; HDC hdc=BeginPaint(h,&ps); draw(h,hdc); EndPaint(h,&ps); return 0; }
    case WM_DESTROY:
        KillTimer(h,ID_TIMER);
        DeleteObject(g_font); DeleteObject(g_bBg); DeleteObject(g_b0); DeleteObject(g_b1);
        DeleteObject(g_b0s); DeleteObject(g_b1s); DeleteObject(g_bUnk);
        DeleteObject(g_heat1); DeleteObject(g_heat2); DeleteObject(g_heat3);
        PostQuitMessage(0); return 0;
    }
    return DefWindowProc(h,msg,wp,lp);
}

int WINAPI WinMain(HINSTANCE hInst, HINSTANCE, LPSTR, int nShow){
    WNDCLASSA wc{}; wc.lpfnWndProc=WndProc; wc.hInstance=hInst; wc.lpszClassName="bitforge_wnd";
    wc.hCursor=LoadCursor(nullptr,IDC_ARROW); wc.style=CS_HREDRAW|CS_VREDRAW;
    wc.hbrBackground=(HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassA(&wc);
    g_main=CreateWindowExA(0,"bitforge_wnd","bitforge  -  bit-level viewer/editor",
        WS_OVERLAPPEDWINDOW|WS_CLIPCHILDREN, CW_USEDEFAULT,CW_USEDEFAULT,1400,760,
        nullptr,nullptr,hInst,nullptr);
    ShowWindow(g_main,nShow); UpdateWindow(g_main);
    // Optional launch args:  <pid> | <file> | --arecibo | --seti
    if (__argc>1){
        const char* a=__argv[1];
        if      (strcmp(a,"--arecibo")==0) do_arecibo(g_main,false);
        else if (strcmp(a,"--seti")==0){ do_arecibo(g_main,false); do_seti(g_main,false); }
        else if (strcmp(a,"--hunt")==0) enter_hunt();
        else if (strcmp(a,"--map")==0){ attach_process((uint32_t)GetCurrentProcessId(),false); uint64_t bb=0,bsz=0; if(g_src) for(auto&r:g_src->regions()){ if(r.size>bsz){ bsz=r.size; bb=r.base; } } g_view=bb; enter_map(); }
        else {
            bool numeric = *a && strspn(a,"0123456789")==strlen(a);
            if (numeric) attach_process((uint32_t)atoi(a), false);
            else         attach_file(a, false);
        }
    }
    MSG m; while(GetMessage(&m,nullptr,0,0)){ TranslateMessage(&m); DispatchMessage(&m); }
    return 0;
}
