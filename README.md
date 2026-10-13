<div align="center">

# Urban Reign — PC Port

**English** · [Português (Brasil)](README.pt-BR.md)

**Urban Reign (PS2) running natively on Windows: 60 FPS, high resolution, HD textures, up to 8 players.**

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows&logoColor=white)
![Version](https://img.shields.io/badge/version-1.0.0-d9261c)
![Status](https://img.shields.io/badge/status-beta-yellowgreen)
![Based on](https://img.shields.io/badge/based%20on-PS2Recomp-8A2BE2)
![License](https://img.shields.io/badge/license-GPL--3.0-blue)

### [⬇ Download the latest release](../../releases/latest)

</div>

---

> [!IMPORTANT]
> The game is **not included**. You need **your own copy** of Urban Reign for the PS2 (USA, `SLUS-21209`) as a disc
> image (`.iso`).

## What is this?

A PC port of *Urban Reign* made by **static recompilation**: the game's PS2 code was translated into C++ and compiled
as a Windows program, instead of being emulated. The graphics use PCSX2's renderer, so you get high internal
resolutions, texture filters and HD texture packs.

- 60 FPS (progressive scan), including 8-fighter brawls
- 1x to 8x internal resolution, FXAA, sharpening, TV shaders
- HD texture packs (PCSX2 format)
- Xbox, PlayStation and other controllers, with vibration; multitap for up to 8 players
- Memory card and save states (F5 / F9)

## Status

<a href="docs/status/details.en.md"><img src="docs/status/map.en.svg" alt="Port status map: Sony SDK library functions and PS2 hardware components, colored by status" width="100%"></a>

Every Sony SDK library the game links and every PS2 hardware component it uses, by status. Click the map for the
details.

## Download and play

1. Download the ZIP from the [**latest release**](../../releases/latest) and extract it anywhere.
2. Open **`Urban Reign.exe`**.
3. In the **Game** tab, pick your Urban Reign disc image (`.iso`).
4. Press **PLAY**.

**HD textures (optional):** download the
[Urban Reign Enhanced HD Textures Pack](https://gbatemp.net/threads/urban-reign-enhanced-hd-textures-pack-updated.665094/)
and extract it into the `game\textures` folder (it should end up as `game\textures\SLUS-21209\replacements`).

**Mods:** put mods (a folder with a `.dll`) or PCSX2 `.pnach` patches in `game\mods` and turn them on in the
**Mods** tab; their options are in the in-game menu (**F2**). To make your own: the [mod kit](modsdk/README.md).
The **CoopTool** mod comes with the game (`mods/CoopTool`): Multiplayer with up to 8 fighters (up to 4 humans plus
bots, every one for himself or custom teams, Free Mode stages) and Free Mode missions with up to 4 players (number
of enemies, objective, each fighter's character / AI / strength / health, optional partner, enhanced camera with
L3 / R3). Its **How to use** tab explains each part.

### Requirements

- Windows 10 or 11, 64-bit
- A DirectX 11 graphics card
- A processor with SSE4.1 (any PC from the last ~12 years)

## Controls

| PS2 | Keyboard |
|---|---|
| ✕ / ○ / □ / △ | ↓ / → / ← / ↑ (arrow keys) |
| Left stick | W A S D |
| L1 / R1 / L2 / R2 | Q / E / 1 / 3 |
| L3 / R3 | F / G |
| START / SELECT | Enter / Tab |

Controllers work out of the box. Buttons, controllers per player and the multitap are set in the **Controls** tab.

| Key | In game |
|---|---|
| F11 | Fullscreen |
| F3 | FPS counter |
| F5 / F9 | Quick save / load state |
| F2 | In-game menu: MODS / PATCHES |
| F4 | Reload the mods (.pnach) |

**Recording with OBS:** use *Window Capture* with **Capture Method: Windows 10 (1903 and up)** (the default
"Automatic"/BitBlt shows a blank window), or *Display Capture*.

## Build from source

For those who want to change the code. You need Windows, Visual Studio 2022 (C++ with CMake), Python 3.10+ and your
disc image.

```bat
git clone <this repository>
cd <repository folder>
:: copy your Urban Reign (USA) .iso into the game\ folder
python build.py
```

The menu walks you through it: **Step 1** generates the game code from your disc, **Step 2** builds (the first time
takes over an hour), **Step 3** plays.

➡ **Everything else** (repository layout, settings files, how it works, debugging tools, full credits) is in the
[**advanced guide**](docs/ADVANCED.md).

## Credits

- Made by **thiagordsfx** — [GitHub](https://github.com/thiagordsfx) · [YouTube](https://www.youtube.com/@thiagordsfx)
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) (recompiler and runtime), [PCSX2](https://pcsx2.net/) (GS renderer),
  [Shadow of the Colossus PC port](https://github.com/LightVelox/sotc-vibe-pc),
  [God of War PS2 recomp](https://github.com/KIexster/god-of-war-recomp),
  [Ace Combat 5 static recompilation](https://github.com/sal063/Ace-Combat-5-Static-recompilation) and Michael's
  "Force Progressive Scan" patch. Full list in the [advanced guide](docs/ADVANCED.md#credits-and-license).
- *Urban Reign* © Namco / Paradigm Entertainment. This is a fan project, not affiliated with or endorsed by Bandai
  Namco. No game content is distributed.

The code in this repository is released under the **GPL-3.0** license. See [`LICENSE`](LICENSE).
