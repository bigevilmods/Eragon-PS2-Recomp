# Eragon (SLUS-21322) static recompiler

Translates the PS2 Emotion Engine executable `SLUS_213.22` into C, one host function per guest function, and runs it on Windows with a high-level emulation of the rest of the PS2 (kernel, IOP modules, DMA/VIF/GIF, VU0/VU1, GS, audio).

## Current state (2026-11-23)

**Playable:** boot, logo movies, menus and the first level (CaSpine) run. In the level, the world, sky, characters and HUD render; Eragon walks, runs, jumps and fights with keyboard or gamepad; music, ambience, voice and sound effects play.

**Speed:** about 21 vsyncs/s in the CaSpine benchmark scene, roughly 10–11 game frames/s (frame time about 95 ms; the game draws one frame per two vsyncs). The real PS2 runs this at 30 frames/s. The GPU (an AMD Vega 8 iGPU in testing) is the main limit, then the EE thread (VU1 programs and VIF unpacking). The debug overlay shows FPS, frame time and vsync rate live.

**Known problems:**
- Shadows on the ground show dark rectangular bars with the GPU rasterizer (not with `PS2_GPU=0`). Likely cause: draws writing the same memory through different layouts racing inside one GPU batch. A fix (batch conflicts keyed on format, base and width) is written in `gs.c` but not yet built or verified.
- In jump and attack animations, Eragon's head sometimes disappears and orange rectangles appear on his back and sword. This happens with both rasterizers and with the older VU timing, so it is upstream of the GS (VU1 skinning/animation or EE math); not found yet.
- An invisible wall stops Eragon after the tutorial blockers, around x -112.8, y 79.1 (overlay coordinates). Likely another collision-test emulation error; not investigated yet.
- The CPU/GPU verifier still reports differences on a full-screen darkening pass (pre-existing) and 1–2-step bilinear rounding differences on 8-bit textures.
- Not emulated: audio reverb and ADSR envelopes, the `FILE.IRX` synchronous API, kernel event flags and Suspend/Resume, fog, dithering, mipmapping, local→host GS transfers.
- Nothing is in version control yet.

## Building and running

### Requirements
- Windows 10/11, x64.
- **Visual Studio 2026 Community** (installed at `C:\Program Files\Microsoft Visual Studio\18\Community`) with the **Desktop development with C++** workload. Its MSVC compiler, CMake and Ninja are used; nothing needs to be on `PATH`. If Visual Studio is installed elsewhere, change the `VS=` line at the top of `build.cmd`.
- **Python 3** on `PATH` (as `python`). CMake runs `tools/embed_text.py` to embed the GPU shader, and the code generators are Python.
- A Direct3D 11 GPU for the GPU rasterizer (the default). Without one, the game falls back to the software rasterizer.
- Your own dump of the game disc (SLUS-21322) as a folder. This project lives in a `recomp\` folder inside it, next to `SLUS_213.22`, `IOP\`, `AUDIO\`, `wad\` and the other disc folders.

### Build
From the `recomp\` folder, in a normal Command Prompt or PowerShell:

```
build.cmd
```

The first run configures `build\` with CMake (Ninja, RelWithDebInfo, MSVC) and compiles everything. Later runs rebuild only what changed. The result is `build\eragon.exe` (plus `build\eragon.pdb`, which the profiler uses). A message saying `vswhere.exe` was not found is harmless. To rebuild from scratch, delete the `build\` folder.

Compiling the roughly 14 MB of generated C in `out\` takes several minutes on a full build. Changing `runtime\ps2_runtime.h` or `runtime\ps2_ops.h` recompiles all of it.

The generated sources are already in `out\`. Regenerate them only after changing the generators:
- EE code: `python -I tools\ps2recomp.py ..\SLUS_213.22 out extra_entries.txt` (then rebuild). This overwrites the diagnostic hooks described under `PS2_SCRIPTLOG`.
- VU microcode pages: `python -I tools\vurecomp.py vu_pages out\vu_pages.inc` (then rebuild). New pages are collected by running with `PS2_VUDUMP=1`.

On the first start, the GPU shader is compiled (up to about a minute) and cached as `gs_gpu_<hash>_<entry>.cso` in the working folder.

### Run
From `recomp\`:

```
build\eragon.exe ..\SLUS_213.22
```

Memory card saves go to `memcard\mc0\` and `memcard\mc1\` under the working folder. Log output goes to stderr.

Controls: an XInput gamepad, or the keyboard while the game window is in front:

| Key | PS2 | Key | PS2 |
|---|---|---|---|
| W A S D | left stick | Arrow keys | D-pad |
| K | Cross | L | Circle |
| J | Square | I | Triangle |
| Q / E | L1 / R1 | 1 / 3 | L2 / R2 |
| Enter | Start | Backspace | Select |

F8 toggles the debug overlay, F9 saves a diagnostic capture, F10 switches between the GPU and software rasterizers (see "Debug switches").

## Layout

| Path | What |
|---|---|
| `tools/r5900.py` | R5900 decoder (MIPS III + MMI + COP0/1/2) |
| `tools/ps2recomp.py` | Function discovery + C emitter: `python -I tools/ps2recomp.py ../SLUS_213.22 out extra_entries.txt` |
| `tools/elfinfo.py`, `tools/probe.py` | ELF dump / ad-hoc disassembly probes |
| `runtime/ps2_runtime.h` | CPU context, memory map, dispatch API |
| `runtime/ps2_ops.h` | Semantics for every emitted `OP_*` macro |
| `runtime/runtime.c` | Boot, ELF data load, dispatcher, MMIO fallback |
| `runtime/kernel.c` | EE kernel HLE: syscalls, threads (fibers), semaphores, INTC/DMAC handlers, alarms, timers, VBlank, COP0, watchdog |
| `runtime/vu0.c` | VU0 macro-mode interpreter, VU0/VU1 memories |
| `runtime/vu1.c` | VU micro-mode interpreter (VU1 and VU0 microprograms) and dispatcher for the translated VU pages |
| `runtime/hw.c` | DMAC (normal + source chain), GS privileged registers |
| `runtime/vif.c`, `gif.c`, `gs.c` | VIF0/VIF1, GIF and the GS thread, the GS emulation and software rasterizer |
| `runtime/gs_gpu.c`, `gs_gpu.hlsl` | Direct3D 11 compute GS rasterizer (default) |
| `runtime/iop*.c` | IOP high-level emulation: SIF/RPC, memory card, SFIOP hub, FILE, AUDIO, Bink, pad |
| `runtime/window.c` | Game window, keyboard hotkeys and debug overlay |
| `runtime/profiler.c` | Sampling profiler (`PS2_PROFILE`) |
| `tools/vurecomp.py`, `tools/vudis.py` | VU microcode translator and disassembler |
| `tools/bench.ps1`, `movetest.ps1`, `combat.ps1` | Benchmark, movement and combat test scripts |
| `tools/frames2png.py`, `crop.py`, `groundsheet.py`, `drawsin.py`, `segdec.py`, `eragonpos.py` | Frame, capture, audio and RAM-dump helpers |
| `regions.txt` | Extra code ranges outside the main `.text` |
| `out/` | Generated: `recomp_NNN.c`, `funcs.h`, `functable.c`, `report.json`, `vu_pages.inc` |

## Facts about the binary

- ELF, MIPS R5900 little-endian, **stripped** (no symbols, no section names), entry `0x00293B70` (standard crt0).
- One RWX load segment at `0x00100000` (file 0x299500, mem 0x4DAB80), plus 64 KB at `0x005DAB80`.
- `.text` = `0x00100000–0x002E5AF8`; detected automatically (data starts at `0x002E5B00`).
- A second code range, `0x0038D200–0x003923E8`, holds 177 static constructors (table at `0x3923F0`). It is listed in `regions.txt`.
- The SDK copies three kernel patches out of the data area: alarms (`0x80076000`), TLB (`0x80075000`) and a third at `0x80074000`. They are never run; their syscalls (0xFC–0xFF, 0x55–0x59) are implemented natively.
- Sony SDK 3.0.x libraries (`PsIIlibgraph/dma/kernl/cdvd 3000`, `libpad/libmc 3020`); © 2006 Vivendi Universal Games.
- IOP modules in `IOP/`: AUDIO.IRX, FILE.IRX, LIBSD, MCMAN/MCSERV, PADMAN, SIO2MAN, IOPRP300.IMG.

## Recompiler output (current)

- 8,818 functions (both code ranges), 0 undecodable words, about 14 MB of C. Every emitted `OP_*` macro is defined.
- Heavy users: VU0 macro ops (10,676, 41 kinds, including 36 `VCALLMS` microprogram launches), 128-bit `lq/sq` (28K), FPU (~40K).
- See "Building and running" above. `set PS2_TRACE=1` logs every syscall, DMA start and DMAC interrupt.
- A watchdog prints the current thread and loop every 3 s, and writes `ramdump.bin` when a loop looks stuck.

### How a run proceeds
1. crt0 → libkernl kernel patches (skipped, handled natively) → 177 static constructors.
2. Kernel: threads (host fibers), semaphores, sleep/wakeup, alarms and interrupt handlers. VBlank runs at 59.94 Hz and wakes the render thread each frame.
3. Graphics init (sceGsResetGraph, SetGsCrt NTSC interlaced); VIF1/GIF DMA chains run every frame, and GS SIGNAL/FINISH/LABEL interrupts come from the GIF/GS emulation.
4. IOP (high-level emulation, `iop*.c`): SIF/RPC, IOP reset, module loading, memory card, SFIOP hub, file loads, audio, Bink, pad. Details under "IOP" below.
5. Game data loads from the disc folder (`wad/boot.wad`, `wad/us/NTSC/boot.wad`, then `shell.wad`, then the level WADs).
6. Bink movies (logos and in-game `FMV/*.BIK`) stream, decode and play with sound.
7. Legal/autosave screens, title, main menu and the CaSpine level (see "Current state").

### Graphics
| File | What |
|---|---|
| `runtime/gs.c` | 4 MB local memory with real swizzling (CT32/24/16/16S, T8/T4/T8H/T4HL/T4HH, Z32/24/16/16S); host→local and local→local transfers; CLUT (CSM1/2, CLD modes); points/lines/triangles/strips/fans/sprites; texture (nearest/bilinear, wrap modes, TFX, TEXA); alpha test/blend, PABE, FBA, DATE, Z test, scissor, FBMSK, COLCLAMP; display readout from PMODE/DISPFB/DISPLAY; SIGNAL/FINISH/LABEL interrupts |
| `runtime/gs_gpu.c`, `gs_gpu.hlsl` | **GPU rasterizer (Direct3D 11 compute, default).** The 4 MB swizzled VRAM lives in a GPU buffer and a compute shader runs the same pixel pipeline as `gs.c` on it, so render-to-texture, Z buffers read as textures and 16/32-bit views of the same memory behave as on the CPU. Each batch is binned into 16×16 tiles; one thread group walks a tile's primitives in submission order. VRAM is kept coherent with the CPU copy per 8 KB page: pages are marked GPU-written only where a primitive's exact pixel bounds fall; transfers into GPU-newest pages are applied there as masked word writes (scatter) and also recorded as fresh on the CPU copy, so a CLUT uploaded that way loads without a readback. The display is copied to a staging ring and presented when the GPU has finished it (about one vsync later), so in gameplay the GS thread does not wait for the GPU (`PS2_GPU_SYNCDISPLAY=1` reads it back synchronously). Per-dispatch inputs are appended to ring buffers (`MAP_WRITE_NO_OVERWRITE`, base offsets in a constant buffer; `PS2_GPU_DISCARD=1` forces DISCARD maps). `PS2_CLUTLOG=1` logs CLUT loads that still need a readback and which draw marked their page. A 256-entry CT32 CLUT that the GPU drew (render-to-CLUT, used in gameplay) is read by the shader straight from VRAM (`GSF_CLUTVRAM`) instead of being read back; draws that would rewrite it end the batch first (`PS2_NOCLUTVRAM=1` restores the readback). CLUT ranges are block-exact (4 blocks for a 256-entry CT32 CLUT). Self-sampling draws snapshot only the texture pages under their texel bounding box. `PS2_GPUTIME=1` adds sampled GPU timings per category (copies, raster, scatter, uploads) to the `PS2_GFXSTATS` output. The shader stages each tile's primitives and states 32 at a time in groupshared memory (one cooperative load per group instead of one per thread), and every primitive carries its exact covered-pixel box (also used for binning). Diagnostics: `PS2_GPU_DBG=1` coverage only, `=2` no texturing, `=3` sample live VRAM instead of the pre-dispatch copy; `PS2_GPU_COHERCHECK=1` compares every page the tracking calls CPU-current with the GPU copy every 200 vsyncs; `PS2_GPUVERIFY_FULLSYNC=1` makes the verifier start each flush from a full download. Batches never mix two swizzles of the same memory; a draw that samples its own target reads a pre-dispatch snapshot of that texture. Unsupported frame formats fall back to the CPU rasterizer. The compiled shader is cached as `gs_gpu_<hash>.cso`. `PS2_GPU=0` uses the CPU rasterizer; `PS2_GPUVERIFY=1` (+ `PS2_GPUVERIFY_X0=x`) rasterizes each gameplay batch on both and reports differences |
| `runtime/gif.c` | GIF tag parser (PACKED/REGLIST/IMAGE). The GS runs on its **own host thread**, fed by an ordered queue, as the real GS runs in parallel with the EE. Also **PATH3 masking**: MSKPATH3/GIF_MODE stall PATH3, unmask lets one EOP packet through, FLUSHA drains. Bink depends on this |
| `runtime/vif.c` | VIF0/VIF1: STCYCL/OFFSET/BASE/ITOP/STMOD/MARK/MSKPATH3/FLUSHx/MSCALx/STMASK/STROW/STCOL/MPG/DIRECT(HL)/UNPACK (all 13 formats, masks, modes, fill/skip, double-buffered TOPS) |
| `runtime/window.c` | Win32 window (StretchDIBits), hotkeys (F8 overlay, F9 capture, F10 rasterizer switch) and the debug overlay (FPS, frame time, vsync rate, renderer, Eragon's position) |
| `runtime/vu1.c` | VU micro-mode interpreter shared by VU1 and VU0 (VCALLMS), plus the translated VU pages. **Cycle-based timing**: VF results ready 4 cycles after issue (readers stall; ACC reads never stall), MAC/status/clip flags visible after 4 cycles, Q/P after FDIV/EFU latency, WAITQ/WAITP stall. With the I bit set, the upper op reads the *old* I and the lower word loads I afterwards (the character renderer depends on it). XGKICK feeds PATH1 |

Debug switches:
- `PS2_SHOT=N`: dump a BMP every N vsyncs to `frames/` (`tools/frames2png.py` makes PNG contact sheets).
- `PS2_VRAMDUMP=N`: dump all of VRAM at vsync N.
- `PS2_GFXSTATS=1`: display registers, transfer and pixel stats.
- `PS2_GS_SYNC=1`: draw on the EE thread.
- `PS2_NOWINDOW=1`: run without the window.
- `PS2_PADSCRIPT="start@4000:8,lup@5600:300"`: scripted input (`button@vsync:frames`; buttons plus `lup/ldown/lleft/lright/rup/rdown/rleft/rright` for the sticks).
- `PS2_DRAWLOG=N`: log every primitive drawn in vsync N. `PS2_FBLOG=N`: per-vsync display buffer and draw counts per target, for 120 vsyncs. `PS2_COPYLOG=N`: log the final copy into the display buffer.
- `PS2_KICKDUMP=N`, `PS2_KICKFIND=1`: dump XGKICK packets (KICKFIND dumps the first packet with a (1,1) vertex plus `vu1_code/data/regs.bin`). `PS2_VU0DUMP=N`: dump `vu0_code.bin` and a start-address histogram after N VU0 runs.
- `PS2_VIBACKUP=1`: make conditional branches see the pre-write VI value (off by default; Eragon's VU0 collision code needs it off).
- `tools/vudis.py code.bin START [COUNT]`: VU microcode disassembler.
- `[game]` log lines: the game's own debug console (ring buffer of 200×128 chars at `0x3BDB50+64`, written by `f_001b95b0`), echoed by the watchdog every 3 s. The first place to look when something stalls.
- `PS2_FASTTEST=1` (with `PS2_SHOT=N`): only rasterize the 8 vsyncs before each screenshot, which makes long test runs several times faster.
- `PS2_PROFILE=1`: sampling profiler of the EE thread; prints the hottest runtime functions every 10 s (uses the PDB).
- `PS2_RAMDUMP_AT=N,M`: write `ramdump_<vsync>.bin` (32 MB EE RAM) at those vsyncs, for diffing game state. `tools/eragonpos.py ramdump_*.bin` prints Eragon's position from such dumps.
- Debug overlay (top-left of the game window): Eragon's position, the vsync number and the active rasterizer; **F8** hides/shows it, `PS2_OVERLAY=0` starts hidden. `PS2_TELEPORT=vsync,x,y,z` moves Eragon to a position (writes his scene node) for reproducing spots.
- Hotkeys in the game window: **F10** switches between the GPU and software rasterizers while playing; **F9** writes a capture to `capture/NN_*` (EE RAM, GS memory, Eragon's position, every primitive of the next 4 vsyncs with full GS state, and those frames as BMP; `tools/drawsin.py` lists the primitives touching a screen rectangle). `PS2_CAPTURE_AT=vsync` / `PS2_TOGGLE_GPU_AT=vsync` do the same from scripts.
- GPU renderer experiments (wrong or slow output, for narrowing bugs down): `PS2_GPU_ONEBYONE=1` one dispatch per primitive, `PS2_GPU_FULLCOPY=1` snapshot whole textures for self-sampling draws, `PS2_GPU_NOCOPY=1` no snapshots. `PS2_GPUVERIFY_MAX=n` raises the verifier's detailed-report limit (default 25). `tools/combat.ps1 -Out sheet.png [-Env ...]` runs a scripted attack/jump sequence and saves a frame sheet; `tools/groundsheet.py` crops the area around Eragon's feet.
- `PS2_WATCH=addr[,addr2]` (hex, + `PS2_WATCH_FROM=vsync`): log every EE store to those 16-byte blocks with the value and the recompiled call chain (`[wstore]` lines; first 400).
- `tools/movetest.ps1 [-Env ...]`: boots into CaSpine, holds the stick up and reports how much the background changes (near 0 = the player is not moving).
- `PS2_VUTALLY=1`: per-VU1-program starts, instructions, kicks, drawn vs ADC vertices. `PS2_VU1CODEDUMP=0xADDR`: dump VU1 code/data/regs the first time a program starts at ADDR.
- **VU microcode is statically translated too.** Run once with `PS2_VUDUMP=1` to save every 64-instruction page of VU code the game uploads into `vu_pages/`, then `python -I tools/vurecomp.py vu_pages out/vu_pages.inc` and rebuild. Pages are matched by content hash when code memory changes; unknown pages fall back to the interpreter. `PS2_VUINTERP=1` forces the interpreter; `PS2_VUVERIFY=1` runs both on every VU1 program and reports any difference (0 mismatches in 2.1M runs). The translator marks an FMAC's MAC result dead when a later FMAC on the same branch-free path replaces it with no FMEQ/FMAND/FMOR before, or within 3 instructions after, the replacement (flags appear 4 cycles after issue); dead results are not computed when the loaded VU1 code has no FS* instruction (sticky status unused; checked whenever code memory changes). About 70% of FMACs qualify. `PS2_VUFLAGS=1` (and `PS2_VUVERIFY`) keep every flag. Reading ACC never stalls (the accumulate path is forwarded, so ADDA/MULA → MADD issue back to back). The VU0 ray/triangle test used for character collision reads MAC with FMAND exactly 4 cycles after the MADD it checks; with an ACC stall it read another instruction's flags, accepted triangles far from the ray, and Eragon could not walk (`PS2_VU_ACCSTALL=1` restores the old stall in the interpreter for comparison). VIF UNPACK has a fast path for unmasked, non-offset, non-filling unpacks (one copy for contiguous V4-32), and command payloads are taken from the DMA stream in bulk.
- `tools/bench.ps1 [-Seconds N] [-Env "K=V,..."]`: in-level speed benchmark (vsyncs/s over the last 60 s). `PS2_GS_THREADS=n` sets raster workers (default: logical CPUs − 1). `PS2_NOTEXCACHE=1` disables the texture cache.
- `PS2_PROFILE=1|lines` (+ `PS2_PROFILE_GS=1` for the GS thread), `PS2_AREALOG=1` (pixels drawn per render target in one frame), `PS2_VUIMAGES=1` (distinct VU code images).
- `PS2_SCRIPTLOG=1`: log level-script API calls (`sau*`) by name. Needs the diagnostic hook at the top of `f_00247e60`/`f_00249b20` in `out/recomp_014.c` (lost when the code is regenerated).

Performance: boot, menus and movies run at full speed. In the level the game reaches about 21 vsyncs/s (benchmark scene, `tools\bench.ps1`), limited mainly by the GPU rasterizer and then by the EE thread (VU1 programs, VIF unpacking). The software rasterizer (`PS2_GPU=0`, multithreaded) is several times slower in 3D.

### IOP (high-level emulation)
The real IOP modules are not run. Their RPC/SIF protocols are reimplemented natively.

| Server / channel | Module | Status |
|---|---|---|
| SIF cmd 0x80000000–0x8000000C | sifcmd/sifrpc | done: init, reset, bind, call, other-data |
| 0x80000003 / 0x80000006 | IOP heap / loadfile | done: fake heap; loads "succeed"; fn 255 version "3000"; fn 6 buffer loads |
| 0x80000400 | MCSERV | done, host folder `recomp\memcard\mc0\` (GetInfo, open/read/write/seek, mkdir, getdir, delete, chdir) |
| 0xFF04 + user cmd 0/1 | SFIOP.IRX (Stormfront) | done: batch hub, 64-byte commands/responses |
| 0xFF01 + SFIOP type 2 | FILE.IRX | async loads done (subcmds 1/2/3/12/15). Sync RPC API (fn 1–21, `Config.txt`) still stubbed |
| 0xFF03 + SFIOP type 1 | AUDIO.IRX (`iop_audio.c`) | done: cues, SPU RAM banks, sound effects (PS2 ADPCM with SPU loop/end flags), streamed `.SEG` music/ambience/speech (16 KB header; 16 KB chunks split 8 KB left / 8 KB right), 48 kHz mixer with volume, pause, mute and key-off release → WinMM. `PS2_NOSOUND=1` mutes, `PS2_AUDIOLOG=1` logs commands, `PS2_SFXDUMP=1` writes the mix to `sfx_audio.raw`. Not done: reverb, ADSR envelopes (only a fixed release), pitch effects beyond SET_CUE_SAMPLE_RATE. `tools/segdec.py FILE.SEG out.wav 0x2000` decodes a segment |
| user cmd = load arg (2) | Bink RAD_IOP_SIDE (`iop_bink.c`) | done: open/read/close (virtual LSNs over the disc folder), audio positions paced by host time, PCM → WinMM. Set `PS2_NOSOUND=1` to mute and `PS2_AUDIODUMP=1` to capture |
| 0x80000100/1 | PADMAN (`iop_pad.c`) | done: keyboard (while the window is in front), XInput gamepad, and scripted input (`PS2_PADSCRIPT`) |
| 0x80000592/3, 0x80000001 | cdvdfsv, fileio | zero-reply stubs |

Tools: `tools/irxdis.py` disassembles IRX modules with import names (`syms`/`fn`/`str`).

### How control flow is translated
- Delay slots are folded into the branch: the condition is evaluated first, then the delay slot runs, then the jump. Branch-likely forms run the delay slot only when the branch is taken.
- `jal` → direct C call; `jr ra` → `return`; `jalr`/unknown `jr` → `ps2_call`/`ps2_jump` via the sorted function table.
- Jumps to another function → tail call (`f_x(c); return;`). Splitting a function wrongly is harmless, because all state lives in the context struct and guest RAM.
- If the runtime is entered mid-function, it stops with `no function starts at 0x...`. Add that address to `extra_entries.txt` and regenerate.

## Roadmap

Done: toolchain; EE recompilation; kernel HLE for everything the game uses so far; SIF/IOP HLE (memory card, files, audio, Bink, pad); VIF/GIF/GS with software and GPU rasterizers; VU0 macro and micro mode, VU1, and static translation of VU microcode; Bink movies; game audio; the first level is playable.

Next:
1. **Rendering bugs**: GPU shadow bars (fix written, to verify), Eragon's head and the orange rectangles in jump/attack animations, the verifier's darkening-pass difference.
2. **Collision**: the invisible wall after the tutorial blockers.
3. **Speed** (goal: the game's 30 frames/s): cheaper per-pixel GPU work, fewer and larger GPU batches, then the EE side (VU1 programs, VIF).
4. **Audio**: reverb, ADSR envelopes.
5. **Kernel/IOP**: event flags, Suspend/Resume, the `FILE.IRX` synchronous API.
6. **GS features**: fog, dithering, mipmapping, local→host transfers.
7. **Accuracy**: PS2 FPU clamping (`PS2_FPU_CLAMP`), COP0 Count timing, exact triangle edge rules.
8. **Beyond CaSpine**: play through further levels and fix what they need.
