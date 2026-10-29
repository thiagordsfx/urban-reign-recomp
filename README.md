<div align="center">

# Urban Reign — PC Port

**English** · [Português (Brasil)](README.pt-BR.md)

**Urban Reign (PS2) running natively on Windows and Linux: 60 FPS, high resolution, HD textures, up to 8 players.**

![Platform](https://img.shields.io/badge/platform-Windows%20x64-0078D6?logo=windows&logoColor=white)
![Platform](https://img.shields.io/badge/platform-Linux%20x86--64-FCC624?logo=linux&logoColor=black)
![Version](https://img.shields.io/badge/version-1.4.1-d9261c)
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
as a Windows and Linux program, instead of being emulated. The graphics use PCSX2's renderer (DirectX 11 on Windows,
Vulkan on Linux), so you get high internal resolutions, texture filters and HD texture packs.

- 60 FPS (progressive scan), including 8-fighter brawls
- Real widescreen (16:9, the camera shows more of the scene) or the original 4:3
- 1x to 8x internal resolution, FXAA, sharpening, TV shaders
- HD texture packs (PCSX2 format)
- Xbox, PlayStation and other controllers, with vibration; multitap for up to 8 players
- Memory card and save states (F5 / F9)

## Status

<a href="docs/status/details.en.md"><img src="docs/status/map.en.svg" alt="Port status map: Sony SDK library functions and PS2 hardware components, colored by status" width="100%"></a>

Every Sony SDK library the game links and every PS2 hardware component it uses, by status. Click the map for the
details.

## Download and play

### Windows

1. Download the ZIP from the [**latest release**](../../releases/latest) and extract it anywhere.
2. Open **`Urban Reign.exe`**.
3. In the **Game** tab, pick your Urban Reign disc image (`.iso`).
4. Press **PLAY**.

**HD textures (optional):** download the
[Urban Reign Enhanced HD Textures Pack](https://gbatemp.net/threads/urban-reign-enhanced-hd-textures-pack-updated.665094/)
and extract it into the `game\textures` folder (it should end up as `game\textures\SLUS-21209\replacements`).

**Mods:** put mods (a folder with a `.dll`) or PCSX2 `.pnach` patches in `game\mods` and turn them on in the
**Mods** tab; their options are in the in-game menu (**F2**). To make your own: the [mod kit](modsdk/README.md).
The **CoopTool** mod comes with the game (`mods/CoopTool`): Multiplayer with up to 8 fighters (up to 8 humans, the
rest bots, every one for himself or custom teams, Free Mode stages) and Free Mode missions with up to 4 players (number
of enemies, objective, each fighter's character / AI / strength / health, optional partner, enhanced camera with
L3 / R3). Its **How to use** tab explains each part.

### Linux

From the [**latest release**](../../releases/latest), pick one:

- **`.deb`** (Debian, Ubuntu, Mint...): `sudo apt install ./urban-reign_1.4.1_amd64.deb` and open **Urban Reign** from
  the applications menu. Settings, saves and your own mods are in `~/.local/share/urban-reign` (HD textures go in
  `~/.local/share/urban-reign/textures/SLUS-21209`).
- **`.tar.gz`** (any distribution): extract it and run **`urban-reign`**; everything stays in that folder, as with the
  Windows ZIP (`./install-shortcut.sh` adds it to the menu).

Both open the same launcher as on Windows (pick the `.iso` in the **Game** tab and press **PLAY**). The launcher uses
the system's GTK with WebKit; when it is missing it says what to install (`sudo apt install python3-gi
gir1.2-webkit2-4.1`), and `urban-reign --game` starts the game directly. Details in the [Linux guide](docs/LINUX.md).

### Requirements

- **Windows:** Windows 10 or 11, 64-bit, and a DirectX 11 graphics card
- **Linux:** 64-bit with glibc 2.30 or newer (Ubuntu 20.04+, Debian 11+, Fedora, Arch...) and a Vulkan graphics driver;
  the window is X11 (XWayland on Wayland desktops)
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

**On Linux** (Clang; the game code compiles in a few minutes):

```sh
sh tools/linux/install_deps.sh   # apt, dnf or pacman (uses sudo)
./build.py
```

`./build.py release` makes the `.tar.gz` and `.deb` packages (needs podman or docker). Everything about Linux:
[`docs/LINUX.md`](docs/LINUX.md).

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
