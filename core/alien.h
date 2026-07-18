// bitforge - alien.h
// Generate bitforge's own "alien reply" to the Arecibo message: a bilaterally
// symmetric 73x23 glyph (aliens love symmetry), packed exactly like AR_MSG
// (1679 bits, row-major, MSB-first) so it renders in the same 23-column grid.
// Deterministic in `seed` so a given contact always replies the same way.
#pragma once
#include "arecibo.h"
#include <cstdint>
#include <cstring>

namespace bf {

inline void generate_alien_reply(unsigned char out[210], uint32_t seed){
    const int R = AR_ROWS, C = AR_COLS, mid = C/2;   // 73 x 23, centre col 11
    unsigned char g[AR_ROWS*AR_COLS];
    for(int i=0;i<R*C;i++) g[i]=0;

    uint32_t s = seed ? seed : 2463534242u;
    auto rnd = [&](){ s^=s<<13; s^=s>>17; s^=s<<5; return s; };

    // top: binary count 1..4 -- echo Arecibo's "here is how we count" opener
    for(int n=1;n<=4;n++){ int col=2+(n-1)*5; for(int b=0;b<3;b++) if((n>>b)&1) g[1*C + col+(2-b)]=1; }

    // head: a symmetric diamond
    for(int r=5;r<=11;r++){ int w=(r<=8)?(r-4):(12-r); if(w<0)w=0;
        for(int c=0;c<=w;c++){ g[r*C+mid-c]=1; g[r*C+mid+c]=1; } }

    // body: a tapering, textured, mirror-symmetric torso with a spine
    for(int r=13;r<=55;r++){
        int taper = 9-(r-13)/6; if(taper<3)taper=3; if(taper>mid)taper=mid;
        g[r*C+mid]=1;
        for(int c=1;c<=taper;c++){ int on=(int)(rnd()%100)<45; g[r*C+mid-c]=on; g[r*C+mid+c]=on; }
    }
    // limbs and a base platform
    for(int r=40;r<=60;r++){ g[r*C+mid-6]=1; g[r*C+mid+6]=1; }
    for(int r=62;r<=66;r++) for(int c=0;c<=8;c++){ g[r*C+mid-c]=1; g[r*C+mid+c]=1; }

    // framing corner ticks
    g[0]=1; g[C-1]=1; g[(R-1)*C]=1; g[(R-1)*C+C-1]=1;

    std::memset(out,0,210);
    for(int i=0;i<R*C;i++) if(g[i]) out[i>>3] |= (unsigned char)(1<<(7-(i&7)));
}

} // namespace bf
