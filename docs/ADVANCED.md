# Urban Reign PC port — advanced guide

**English** · [Português (Brasil)](ADVANCED.pt-BR.md) · [← back to the README](../README.md)


---

> [!IMPORTANT]
> This repository **does not contain any game files** (ISO, executable, IRX modules, `.AFS` archives), nor the C++
> code generated from them. You need **your own legal copy** of Urban Reign for the PS2.

## Contents

- [What is this?](#what-is-this)
- [Current status](#current-status)
- [Repository layout](#repository-layout)
- [Requirements](#requirements)
- [Building and running](#building-and-running)
- [Controls and settings](#controls-and-settings)
- [How it works](#how-it-works)
- [Debugging tools](#debugging-tools)
- [Documentation](#documentation)
- [Credits and license](#credits-and-license)

## What is this?

Instead of emulating the PlayStation 2 one instruction at a time, this project **translates the game's original
executable (`SLUS_212.09`) into C++** with [PS2Recomp](https://github.com/ran-j/PS2Recomp) and compiles it as a
native Windows program, linked against a runtime that reimplements the PS2 hardware and operating system (EE kernel,
DMA, VU0/VU1, IPU, CD/DVD…). The I/O processor (IOP) runs the game's **original IRX modules** (the `NUSOUND` sound
driver, `CRI_ADXI`, `PADMAN`, `MCMAN`/`MCSERV`…) on an R3000A interpreter. The picture is drawn by
**PCSX2's hardware GS renderer** (DX11), so internal resolution, filters and HD texture packs work as in PCSX2.

## Current status

<a href="status/details.en.md"><img src="status/map.en.svg" alt="Port status map: Sony SDK library functions and PS2 hardware components, colored by status" width="100%"></a>

Per-library and per-component breakdown: [`docs/status/details.en.md`](status/details.en.md), generated from
[`docs/status/data.toml`](status/data.toml) by `python tools/status/generate.py`.

| Milestone | Status |
|---|:---:|
| Recompiling `SLUS_212.09` to C++ (9,878 functions, with the Ghidra map) | ✅ |
| Boot, menus, memory card (PCSX2-compatible `Mcd001.ps2`), controller | ✅ |
| Fights and story mode | ✅ |
| Audio: SFX, music and voices (SPU2 + CRI ADX) | ✅ |
| Performance: 8-fighter brawl at a steady 60 FPS, 1x to 8x internal resolution (GTX 1660 SUPER) | ✅ |
| PCSX2 GS renderer: FXAA, texture filtering, CAS sharpening, TV shaders, HD texture packs | ✅ |
| Sofdec videos (emulated IPU) | ✅ |
| Real widescreen (the game's own 16:9 mode, on by default: more of the scene, not a stretched 4:3) | ✅ |
| Multitap (up to 8 controllers; port 2 by default, as the game expects) | ✅ |
| Controllers through SDL3 (PCSX2-style mapping, vibration) | ✅ |
| Ready-to-play release (1.4.1) | ✅ |
| Mods: in-game menu (F2), live .pnach, DLL mods + mod kit, CoopTool (multiplayer up to 8, humans too; Free Mode up to 4) | ✅ |

The full investigation log (in Portuguese) is in [`docs/PROGRESS.md`](PROGRESS.md), and the plan in
[`docs/ROADMAP.md`](ROADMAP.md).

## Repository layout

| Path | Contents |
|---|---|
| `build.py` | build and run menu (see below) |
| `src/` | the port: `main.cpp` (boot, `config.ini`/`controls.ini`), disc check, SDK HLE bindings, function hooks, profilers |
| `recomp/` | recompiler configuration (`urban.toml`) and the Ghidra function map (`urban_functions.csv`) |
| `runtime/` | PS2Recomp's runtime, IOP, recompiler and offline tools, with this port's changes ([`runtime/README.md`](../runtime/README.md)) |
| `thirdparty/` | PCSX2's GS (only the files used), our `pcsx2gs.dll` wrapper, DirectX-Headers ([`thirdparty/README.md`](../thirdparty/README.md)) |
| `tools/` | code generation, analysis, benchmark, debugging and release scripts |
| `docs/` | progress log, roadmap, environment variables, status map, licenses |
| `game/` | **your** disc image goes here (not versioned) |
| `textures/` | HD texture packs, `textures/SLUS-21209/replacements` (not versioned) |
| `bench/`, `build/`, `generated/` | benchmark savestates and runs, the build, the generated C++ — **not versioned** |

## Requirements

- Windows 10/11 x64, Visual Studio 2022 (Desktop C++, with CMake and Ninja), Python 3.10+
- A DirectX 11 GPU
- Your **Urban Reign (USA)** disc image in `game/`; the extracted `SLUS_212.09` is checked by hash
  (`SHA-256 60399090fc69ffdb4c39afd30977ab50890b0e33ed6bd9374e4d9497f4246081`)
- Optional: [Ghidra](https://ghidra-sre.org/) 12.x + JDK 21 + the
  [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded) extension, to regenerate
  the function map (`python tools/ghidra_functions.py`); the map is already in `recomp/`

## Building and running

```bat
git clone <this repository>
cd UrbanRecomp
:: put your disc image in game\
python build.py
```

`python build.py` opens a numbered menu:

```
=== Urban Reign PC: build ===
  Step 1) Generate the game code from your disc image (put the .iso in game/ first)
  Step 2) Build urban.exe (the first time takes over an hour)
  Step 3) Play

  Utilities
  4) Build and play
  5) Configure again (CMake)
  6) Build only the PCSX2 GS (pcsx2gs.dll)
  7) Benchmark a savestate (bench/states)
  8) Open the game folder (build/port/bin)
  9) Make a release: folder + ZIP (build/release)

  0) Exit
```

Follow the steps in order: **1** (extracts the ELF from your disc and generates `generated/`), **2**, **3**. The first full build takes
over an hour (≈10k generated files); later builds only recompile what changed. The same steps without the menu:
`python build.py generate`, `python build.py build`, `python build.py play`.

- 8 parallel jobs by default (`URBAN_JOBS` changes it). `URBAN_DEPS` can point to an already downloaded FetchContent
  `_deps` folder (raylib, fmt...) to skip the downloads.
- Generated files over 900 KB go to `urban_game_code_huge` and are compiled at most 2 at a time (`URBAN_HUGE_JOBS`),
  so MSVC does not run out of memory.
- **VU1 microcode compiled ahead of time** (~4x faster 3D): capture the microcode with `PS2X_VU1_CAPTURE=<dir>`,
  then `python build.py configure -DURBAN_BUILD_TOOLS=ON`, `python build.py build vu1_recomp` and
  `build\port\runtime\ps2xTest\vu1_recomp.exe <dir> generated\vu1`; the next build picks up `generated\vu1`.
- Host-only rebuilds: `python build.py configure -DURBAN_GAME_CODE_LIBRARY=build/port/src/urban_game_code.lib`.

### Release

`python build.py release` puts together what goes in a GitHub release: `build/release/UrbanReignPC-1.2.0.zip` with
`game/` (`urban.exe`, only the DLLs it loads, `pcsx2gs.dll` + resources, `mods/`, `textures/`), README, LICENSE and
THIRD-PARTY notes. No disc data and nothing the game writes at run time goes in it. Running it again updates
`build/release/UrbanReignPC-1.2.0` in place: settings, memory cards, states, mods and textures there are kept.

## Controls and settings

| PS2 | Keyboard |
|---|---|
| ✕ / ○ / □ / △ | ↓ / → / ← / ↑ (arrow keys) |
| Left stick | W A S D |
| L1 / R1 / L2 / R2 | Q / E / 1 / 3 |
| L3 / R3 | F / G |
| START / SELECT | Enter / Tab |

Controllers go through SDL3 (Xbox, PlayStation and others, with vibration). `controls.ini` next to `urban.exe` uses
PCSX2's format: `[Pad1]`...`[Pad8]` with one `Button = Source/Element` line per binding (`Cross = Keyboard/Down`,
`Cross = SDL-0/FaceSouth`, `LargeMotor = SDL-0/LargeMotor`), so the sections of `PCSX2.ini` can be copied over;
`SDL-<n>` is the controller number (printed in the console when it connects). Multiplayer: a multitap is plugged into
port 2 (`[Pad] MultitapPort1/MultitapPort2`, up to 8 controllers); players: Pad1 = port 1, Pad2 = port 2 slot A,
then port 2's slots B-D and port 1's B-D (port 2 first, unlike PCSX2: the game uses it for P3/P4, so port 1's
multitap only adds players 6-8; P5-P8 as humans with CoopTool). A `controls.ini` from an older version is converted on the first run
(`controls.ini.old` keeps it). **F1** opens the runtime debugger (ImGui), **F3** the FPS counter, **F5/F9** quick save/load
state, **F6** starts/stops a performance capture (**F7** marks a moment in it), **F8** writes a debug capture
(RAM + VU1 trace), **F11** toggles fullscreen.

The game starts in **progressive scan (480p)**, like PCSX2's "Force Progressive Scan" patch: up to 60 FPS instead of
30 (`URBAN_PROGRESSIVE=0` in `config.ini` `[Runtime]` goes back to interlaced).

Video settings are in `config.ini` next to `urban.exe` (created on the first run; older files get new options added
with their defaults):

- `InternalResolution=1..8` (1 = native, 2 = 1280×896, 3 = 1920×1344...), `StartMaximized`, `Stretch`.
- `AspectRatio=16:9` (default: real widescreen, the game's own 16:9 mode with the exact proportion), `4:3` (the
  original picture) or `auto` (as the game's Options menu).
- `Renderer=pcsx2` (default: PCSX2's GS with the GameIndex fixes for Urban Reign) or `native` (the port's own GS).
- PCSX2's filters: `FXAA` (on by default), `TextureFiltering`, `TrilinearFiltering`, `AnisotropicFiltering`,
  `Sharpening` (CAS), `ColorCorrection` (brightness/contrast/saturation/gamma), `TVShader` (scanlines, CRT...),
  `ScreenFilter`, `Dithering`, `BlendingAccuracy`.
- HD textures: PCSX2 texture packs (PNG/DDS in a `replacements` folder) in `textures\SLUS-21209` next to `urban.exe` or
  in the repository, or in `HDTexturesFolder`; `HDTextures=0` turns them off.

## How it works

1. **Recompilation**: `ps2_recomp` translates every MIPS function of `SLUS_212.09` into a C++ function. Functions are
   found by the recompiler itself and by the Ghidra map; both lists are merged, because functions only called through
   pointers (such as vtable methods) would otherwise have no entry.
2. **SDK**: known Sony SDK functions (SIF, CD, file I/O...) are located by signature and replaced at startup by the
   runtime's HLE implementations.
3. **Runtime**: the EE scheduler runs the game's threads; DMA, VIF, VU0/VU1 (compiled ahead of time) and the IPU are
   emulated; GIF packets go to PCSX2's GS on its own thread; the IOP interprets the original IRX modules, including
   the sound driver and PADMAN.
4. **Changes**: everything that had to change in PS2Recomp for this game is in `runtime/`, explained in
   [`docs/PROGRESS.md`](PROGRESS.md).

## Debugging tools

| Tool | Use |
|---|---|
| `tools/autotest/drive.py` | automatic test: starts the game, recognizes screens and navigates to a fight (`--route challenge/story`, `--mash` for stability) |
| `tools/autotest/bench_state.py` | frames/s on a savestate from `bench/states` (menu option 6) |
| `tools/vu_disasm.py` | VU microcode disassembler (upper/lower pairs) |
| `tools/vu1_trace.py` | reads the VU1 trace (F8 or `PS2X_VU1_TRACE`): microprograms, registers, data memory |
| `tools/vif_chain.py` | decodes DMA chains, VIF codes and GIF packets |
| `tools/gs_vram.py` | converts GS VRAM to PNG |
| `tools/frame_times.py`, `tools/present_pacing.py` | frame pacing from `PS2X_FRAME_TIMES` or an F6 capture |
| `tools/ghidra_functions.py` | imports/analyzes the ELF in Ghidra (headless) and exports the function map |
| `tools/status/generate.py` | generates the status map (SVG) and detail tables |
| `tools/pine.py` | reads game RAM from PCSX2 through PINE, to compare with the port |

The debugging environment variables (`URBAN_*` and `PS2X_*`) are in [`docs/ENV.md`](ENV.md).

## Documentation

- [`docs/PROGRESS.md`](PROGRESS.md) — investigation log, with addresses, logs and the reason for every change
- [`docs/ROADMAP.md`](ROADMAP.md) — plan, performance criteria and known bugs
- [`docs/ENV.md`](ENV.md) — environment variables, keys and using PCSX2 as a reference
- [`docs/GS_PCSX2.md`](GS_PCSX2.md) — how PCSX2's GS is built and wired in
- [`docs/GAME.md`](GAME.md) — technical reference of the game (SDK, middleware, IRX, microcode)
- [`docs/status/`](status/) — status map and details

The documentation is written in Portuguese (Brazil).

## Credits and license

- **Michael** — Urban Reign "Force Progressive Scan" patch (60 FPS), reproduced in the port.
- [**PS2Recomp**](https://github.com/ran-j/PS2Recomp) by ran-j and contributors — recompiler and runtime (GPL-3.0).
- [**Shadow of the Colossus port**](https://github.com/LightVelox/sotc-vibe-pc) (sotc-vibe-pc) by LightVelox — the runtime fork used here (emulated IOP, OpenGL GS, SPU2, SIO2,
  ImGui debugger, controls) and the signature-based SDK binding approach (GPL-3.0).
- [**God of War PS2 recomp**](https://github.com/KIexster/god-of-war-recomp) by KIexster — VU1 (EATAN, JR/JALR) and VIF
  UNPACK fixes, and the status map generator adapted in `tools/status/generate.py` (GPL-3.0).
- [**PCSX2**](https://pcsx2.net/) (GPL-3.0) — the hardware GS (`pcsx2gs.dll`, with texture replacement and shaders),
  hardware behavior reference (through PINE) and instruction semantics (MMI, SIO2, SPU2).
- [**Ace Combat 5 static recompilation**](https://github.com/sal063/Ace-Combat-5-Static-recompilation) (Apache-2.0) — the IPU emulation (the FMVs' MPEG decoder), in
  `runtime/ps2xRuntime/src/lib/ipu/` (license in `docs/licenses/AC5-Recomp-APACHE-2.0.txt`).
- [**Ghidra**](https://ghidra-sre.org/) (NSA, Apache-2.0) and
  [**ghidra-emotionengine-reloaded**](https://github.com/chaoticgd/ghidra-emotionengine-reloaded) by chaoticgd — ELF
  analysis and function map.
- [**raylib**](https://www.raylib.com/) (zlib), [**Dear ImGui**](https://github.com/ocornut/imgui) (MIT),
  [**DirectX-Headers**](https://github.com/microsoft/DirectX-Headers) (MIT) and [**FFmpeg**](https://ffmpeg.org/)
  (LGPL) — libraries used by the runtime.
- *Urban Reign* © Namco / Paradigm Entertainment. This project is not affiliated with or endorsed by Bandai Namco.
  No game content is distributed.

The code in this repository is released under the **GPL-3.0** license, consistent with PS2Recomp. See [`LICENSE`](../LICENSE).
