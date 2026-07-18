// bitforge - target_toy.cpp
// A safe, self-owned process to practice scanning/editing against -- the moral
// equivalent of Cheat Engine's tutorial. It prints its PID and the addresses of
// a u32, a u16 bitfield, and a float so you can verify the scanner finds them.
//
//   target_toy           interactive: +/- health, f toggle a flag bit, m mana, q quit
//   target_toy --hold    hold values fixed (100 / 0x0005 / 42.5) and never exit,
//                        so an external scan has a stable target.
#include <cstdio>
#include <cstdint>
#include <cstring>
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <conio.h>

volatile uint32_t g_health = 100;
volatile uint16_t g_flags  = 0x0005;   // 0b0000000000000101
volatile float    g_mana   = 42.5f;

int main(int argc, char** argv) {
    bool hold = (argc > 1 && std::strcmp(argv[1], "--hold") == 0);
    std::printf("target_toy  PID %lu\n", GetCurrentProcessId());
    std::printf("health u32 @ 0x%p = %u\n",     (void*)&g_health, g_health);
    std::printf("flags  u16 @ 0x%p = 0x%04X\n", (void*)&g_flags,  g_flags);
    std::printf("mana   f32 @ 0x%p = %.1f\n",   (void*)&g_mana,   g_mana);
    std::fflush(stdout);

    if (hold) { for (;;) Sleep(1000); }   // values stay fixed for scanning

    std::printf("\nKeys: +/- health, f toggle flag bit, m mana+1, q quit\n");
    for (;;) {
        int ch = _getch();
        if      (ch == '+') g_health++;
        else if (ch == '-') g_health--;
        else if (ch == 'f') g_flags ^= 1u;
        else if (ch == 'm') g_mana  += 1.0f;
        else if (ch == 'q') break;
        std::printf("health=%u flags=0x%04X mana=%.1f\n", g_health, g_flags, g_mana);
        std::fflush(stdout);
    }
    return 0;
}
