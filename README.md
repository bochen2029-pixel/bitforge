# bitforge

**A bit-level viewer / editor / scanner for Windows.** Every cell in the grid is one
bit — blue = `1`, dark = `0` — and you click it to flip it straight into the live
source. It attaches to a running **process** (via `ReadProcessMemory` /
`WriteProcessMemory`), a **file**, or its own **sandbox**, all behind a single
`IByteSource` interface, so a raw disk / VHDX / physical-memory / DMA source drops in
later without touching the UI, the scanner, or the renderer.

Zero external dependencies — pure Win32 + GDI. Built with MSVC.

---

## 🛰️ The Arecibo easter egg (transmit → render → detect)

![Arecibo easter egg + SETI detection](docs/arecibo-seti.png)

Hit the **Arecibo** button and bitforge writes the 1974 Arecibo message — **1679 bits,
73 × 23** — as literal bits into a fresh `VirtualAlloc` sandbox, then sets the grid to
**23 columns** so *the raw memory bits are the picture*: you can see the DNA double
helix, the human figure, and the radio-telescope dish, all rendered straight out of
RAM. Click any cell to edit humanity's message to the stars.

Two twists:

- **Updated for 2026.** The message's population field originally encoded
  `4,292,853,750` (Earth's population in 1974). bitforge rewrites those 36 bits to
  today's world population, `8,303,169,803` (~8.3 billion) — located and re-encoded in
  the message's exact 2-D bit orientation, verified by round-trip decode. See
  [`arecibo/gen_arecibo.py`](arecibo/gen_arecibo.py).
- **A memory SETI scanner.** The **SETI** button closes the loop: it bit-searches the
  attached source for a distinctive 64-bit Arecibo signature (at *any* bit alignment),
  then verifies each candidate is the full 1679-bit message before declaring
  `>>> SIGNAL`. Transmit it, then listen for it — in your own RAM.

### 📡 SETI hunt — a screensaver that searches your own RAM

![SETI hunt](docs/seti-hunt.png)

The **Hunt** button (or `--hunt`) turns bitforge into a SETI@home-style screensaver
pointed *inward*: a scrolling waterfall spectrogram sweeps the attached source's memory,
flagging structured "candidate signals" (Gaussian / Pulse / Triplet) as it goes. When it
finds and verifies a real Arecibo message it locks on with a **CONTACT** banner, then
transmits bitforge's own procedurally-generated, bilaterally-symmetric **alien reply**
back into memory ([`core/alien.h`](core/alien.h)). Detect Earth's message in your RAM,
reply with an alien one. `--hunt` seeds a 2 MB noise field with a planted message so you
can watch the sweep lock on.

---

## The everyday tool

![bit grid over live process memory](docs/bitforge-memory.png)

- **Attach** a process (double-click, tick *writable* to edit) or open a file.
- **Bit grid** — click a cell to toggle that bit; arrow keys move the cursor, **Space**
  toggles, **PgUp/PgDn** scroll, **+/-** zoom, wheel scrolls. Recently-flipped bits
  **glow and cool off**, so you can watch memory change live.
- **Region map** — the whole address space (image / mapped / private, `rwx`), click to
  jump.
- **Scanner** — pick a type (`u8..u64`, `i8..i64`, `f32`, `f64`, or `bits` for an
  unaligned bit-pattern like `1011?01`), **First Scan**, then narrow with **Next Scan**
  (Exact / Unchanged / Changed / Increased / Decreased). Double-click a result to jump;
  **Freeze+** holds a value.

## Build

**One shot (MSVC):**
```
build.bat
```
Finds VS 2022 and compiles everything to `build\`. No external dependencies.

**CMake:**
```
cmake -B build
cmake --build build --config Release
```

## Use

```
build\bitforge_gui.exe            # then double-click a process, or Open File
build\bitforge_gui.exe <pid>      # attach on launch (read-only)
build\bitforge_gui.exe <file>     # open a file on launch
build\bitforge_gui.exe --arecibo  # transmit the Arecibo message into a sandbox
build\bitforge_gui.exe --seti     # transmit, then run the SETI detector
build\bitforge_gui.exe --hunt     # SETI@home-style memory-sweep screensaver + alien reply
```

Scriptable proof via the CLI:
```
build\target_toy.exe --hold                       # prints its PID + addresses
build\bitforge_cli.exe scan <pid> u32 100         # find a value
build\bitforge_cli.exe set  <pid> <addr> u32 1337 # write it
build\bitforge_cli.exe poke <pid> <addr> 7 0      # clear one bit
build\bitforge_cli.exe fscan <file> bits 01100100 # unaligned bit search in a file
```

## Architecture — one app, pluggable byte sources

Everything talks only to `IByteSource`, so the disk editor and the memory editor are
the *same* application over a different source:

```
IByteSource
  ├─ FileSource      a plain file (zero risk)
  ├─ ProcessSource   a live process (OpenProcess / VirtualQueryEx / RPM / WPM)
  ├─ BufferSource    a self-owned VirtualAlloc sandbox (the Arecibo scratch space)
  └─ (roadmap)       PhysicalDrive · VHDX · kernel PhysicalMemory · PCILeech/DMA
```

The `bit_span` (get/set/toggle/popcount/**diff**), the address translator, the scanner,
and the GDI renderer are all shared.

```
core/   byte_source.h  bit_span.h  address.h  source_ops.h
        file_source.h  process_source.*  buffer_source.h  scanner.*  arecibo.h
cli/    bitforge_cli.cpp     console harness (proves the engine end-to-end)
gui/    bitforge_gui.cpp     Win32 + GDI bit-grid viewer/editor
target/ target_toy.cpp       a safe process to practice on
arecibo/ gen_arecibo.py + the 1974 and 2026 message grids
```

## Scope

Legitimate for your own processes, debugging, reverse engineering, CTFs, and
forensics on targets you own — not for defeating anti-cheat or DRM.

## Roadmap

- **GPU stage** — upload a region snapshot to VRAM, compute popcount-density / entropy /
  unaligned-bit search in a compute shader, and render a continuous-LOD zoom (whole
  region → single bit). CUDA interop for the analytics island.
- **More sources** — `PhysicalDrive` (sector-aligned disk), VHDX, a kernel-driver
  `PhysicalMemory` source, and a PCILeech/DMA source, all behind `IByteSource`.
- **Space-filling (Hilbert) layout** and a defrag-style region overview map.

## Credits

The Arecibo message was designed in 1974 by Frank Drake, Carl Sagan, and others.
Prior art that inspired the tooling: Cheat Engine, ReClass.NET, binvis, Veles, ImHex.
