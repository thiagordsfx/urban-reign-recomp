"""The launcher's exe and the release ZIP (python build.py launcher / python build.py release).

The launcher (launcher/, Urban Reign.exe) is thiagordsfx's own closed-source program and is not in the repository
(.gitignore): `release` puts its exe in when it is there (built from launcher/, or URBAN_LAUNCHER_EXE=<exe>), and
otherwise makes the release without it (urban.exe runs on its own).

Release layout (what goes in the GitHub release):
  UrbanReignPC-<version>/
    Urban Reign.exe          the launcher (one file)
    game/                    urban.exe and only the DLLs it loads, PCSX2's GS with its resources and shader cache,
                             mods/ and textures/ (empty, with a note)
    README.txt, LICENSE.txt, THIRD-PARTY.txt
Nothing the game writes at run time (config.ini, controls.ini, memcards, states, game_data) and nothing from the disc.
"""
import os
import shutil
import struct
import subprocess
import sys
import venv
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BIN = ROOT / "build" / "port" / "bin"
LAUNCHER = ROOT / "launcher"
WORK = ROOT / "build" / "launcher"
VENV = WORK / "venv"
LAUNCHER_EXE = WORK / "dist" / "Urban Reign.exe"
RELEASE = ROOT / "build" / "release"
VERSION = "1.0.0"

GAME_ROOTS = ["urban.exe", "pcsx2gs.dll"]  # pcsx2gs.dll is loaded with LoadLibrary, not imported
GAME_FOLDERS = ["resources", "shadercache_pcsx2"]

README = f"""URBAN REIGN - PC PORT {VERSION}
======================================
Created by / Criado por thiagordsfx
GitHub:  https://github.com/thiagordsfx
YouTube: https://www.youtube.com/@thiagordsfx

EN
- Open "Urban Reign.exe". In the Game tab pick your own Urban Reign disc image (USA, SLUS-21209, .iso).
  The game is not included: you need your own copy.
- Graphics, controllers (PCSX2 style, multitap, vibration) and mods are set in the launcher.
- Mods: each mod is a folder in game\\mods; turn them on in the Mods tab, their options are in the in-game menu (F2).
  CoopTool comes with it: Multiplayer with up to 8 fighters and Free Mode missions with up to 4 players.
- HD textures: put a PCSX2 texture pack in game\\textures\\SLUS-21209 (the folder with "replacements").
- In game: F11 fullscreen, F3 FPS counter, F5/F9 quick save/load state.
- Needs Windows 10/11 64-bit, a DirectX 11 graphics card and a CPU with SSE4.1.

PT
- Abra o "Urban Reign.exe". Na aba Jogo escolha a sua imagem do Urban Reign (EUA, SLUS-21209, .iso).
  O jogo não vem junto: você precisa da sua própria cópia.
- Gráficos, controles (estilo PCSX2, multitap, vibração) e mods são configurados no launcher.
- Mods: cada mod é uma pasta em game\\mods; ligue na aba Mods, as opções ficam no menu do jogo (F2).
  O CoopTool já vem junto: Multiplayer com até 8 lutadores e missões do Free Mode com até 4 jogadores.
- Texturas HD: coloque um pacote do PCSX2 em game\\textures\\SLUS-21209 (a pasta com "replacements").
- No jogo: F11 tela cheia, F3 contador de FPS, F5/F9 salvar/carregar estado rápido.
- Precisa de Windows 10/11 64 bits, placa de vídeo DirectX 11 e processador com SSE4.1.

Fan project, not affiliated with Bandai Namco. / Projeto de fãs, sem ligação com a Bandai Namco.
"""

THIRD_PARTY = """Third-party software in this release
- PCSX2 GS renderer (pcsx2gs.dll): GPL-3.0-or-later, https://pcsx2.net
- PS2Recomp runtime: see LICENSE.txt, https://github.com/ran-j/PS2Recomp
- SDL3 (SDL3.dll): zlib license, https://libsdl.org
- FFmpeg (avcodec, avutil, swresample, swscale and their libraries): LGPL-2.1-or-later, https://ffmpeg.org
- raylib: zlib license; Dear ImGui: MIT
- Launcher: Python (PSF license), pywebview (BSD-3-Clause), pythonnet (MIT), packaged with PyInstaller
"""

MODS_NOTE = "Mods go here, one folder each (the mod's .dll and mod.json). Switch them on in the launcher's Mods tab;\n" \
            "their options are in the in-game menu (F2). How to make one: modsdk/ in the source repository.\n" \
            "Cada mod fica numa pasta aqui (a .dll e o mod.json). Ligue na aba Mods do launcher; as opções ficam no menu do\n" \
            "jogo (F2). Como criar: modsdk/ no repositório do código.\n"
TEXTURES_NOTE = "HD texture packs go in SLUS-21209\\ (PCSX2 format: SLUS-21209\\replacements\\...).\n" \
                "Pacotes de texturas HD vão em SLUS-21209\\ (formato do PCSX2: SLUS-21209\\replacements\\...).\n"


def run(cmd, **kwargs):
    print("+", " ".join(str(c) for c in cmd), flush=True)
    return subprocess.run([str(c) for c in cmd], **kwargs).returncode


def pe_imports(path: Path) -> list[str]:
    """DLL names a PE file imports (normal and delay-load import tables)."""
    data = path.read_bytes()
    pe = struct.unpack_from("<I", data, 0x3C)[0]
    sections = struct.unpack_from("<H", data, pe + 6)[0]
    optional = pe + 24
    optional_size = struct.unpack_from("<H", data, pe + 20)[0]
    directories = optional + (112 if struct.unpack_from("<H", data, optional)[0] == 0x20B else 96)
    table = []
    for i in range(sections):
        header = optional + optional_size + i * 40
        vsize, va, raw_size, raw_ptr = struct.unpack_from("<IIII", data, header + 8)
        table.append((va, max(vsize, raw_size), raw_ptr))

    def offset(rva):
        return next((rva - va + raw for va, size, raw in table if va <= rva < va + size), None)

    def text(at):
        return data[at:data.index(b"\0", at)].decode("ascii", "replace")

    names = []
    for index, entry_size, name_field in ((1, 20, 12), (13, 32, 4)):
        rva = struct.unpack_from("<I", data, directories + index * 8)[0]
        at = offset(rva) if rva else None
        while at is not None:
            name_rva = struct.unpack_from("<I", data, at + name_field)[0]
            if not name_rva:
                break
            names.append(text(offset(name_rva)))
            at += entry_size
    return names


def game_files() -> list[Path]:
    """urban.exe, pcsx2gs.dll and every DLL of the game folder they load, directly or not."""
    local = {p.name.lower(): p for p in BIN.glob("*.dll")}
    pending, found = list(GAME_ROOTS), {}
    while pending:
        name = pending.pop()
        if name.lower() in found:
            continue
        path = BIN / name if (BIN / name).exists() else local.get(name.lower())
        if path is None:
            continue
        found[name.lower()] = path
        pending += [d for d in pe_imports(path) if d.lower() in local]
    return sorted(found.values(), key=lambda p: p.name.lower())


def build_launcher() -> int:
    if not (VENV / "Scripts" / "python.exe").exists():
        print("Creating the launcher's Python environment (build/launcher/venv).")
        venv.create(VENV, with_pip=True)
    python = VENV / "Scripts" / "python.exe"
    if run([python, "-m", "pip", "install", "--disable-pip-version-check", "-q", "-r", LAUNCHER / "requirements.txt"]):
        return 1
    # One file: the page and the Python code stay inside the exe.
    return run([python, "-m", "PyInstaller", "--noconfirm", "--clean", "--windowed", "--onefile", "--name", "Urban Reign",
                "--icon", LAUNCHER / "ui" / "img" / "icon.ico",
                "--add-data", f"{LAUNCHER / 'ui'};ui", "--distpath", WORK / "dist", "--workpath", WORK / "work",
                "--specpath", WORK, LAUNCHER / "urban_launcher.py"])


def make_release() -> int:
    if not (BIN / "urban.exe").exists():
        print("urban.exe is not built: run the Build option first.")
        return 1
    launcher_exe = Path(os.environ.get("URBAN_LAUNCHER_EXE") or LAUNCHER_EXE)
    if not launcher_exe.exists() and (LAUNCHER / "urban_launcher.py").exists() and build_launcher():
        return 1
    name = f"UrbanReignPC-{VERSION}"
    # Put together in a staging folder (the ZIP comes from it, always clean); the playable folder next to the ZIP is
    # then only updated, so the settings, memory cards, states, mods and textures someone has there are kept.
    staging_root = RELEASE / "staging"
    out = staging_root / name
    if staging_root.exists():
        shutil.rmtree(staging_root)
    out.mkdir(parents=True)
    if launcher_exe.exists():
        shutil.copy2(launcher_exe, out / "Urban Reign.exe")
    else:
        print("No launcher (launcher/ is not here and URBAN_LAUNCHER_EXE is not set): release without it.")
    game = out / "game"
    game.mkdir()
    for path in game_files():
        shutil.copy2(path, game / path.name)
        print("  game/" + path.name)
    for folder in GAME_FOLDERS:
        shutil.copytree(BIN / folder, game / folder)
        print(f"  game/{folder}/")
    (game / "mods").mkdir()
    (game / "mods" / "README.txt").write_text(MODS_NOTE, encoding="utf-8")
    # The mods built with the game (mods/ and the mod kit's example): off until the player switches them on.
    for folder in sorted((BIN / "mods").glob("*/")) if (BIN / "mods").is_dir() else []:
        dll = folder / f"{folder.name}.dll"
        if not dll.exists() or not (folder / "mod.json").exists():
            continue
        (game / "mods" / folder.name).mkdir()
        for file_name in (dll.name, "mod.json"):
            shutil.copy2(folder / file_name, game / "mods" / folder.name / file_name)
        print(f"  game/mods/{folder.name}/")
    (game / "textures").mkdir()
    (game / "textures" / "README.txt").write_text(TEXTURES_NOTE, encoding="utf-8")
    (out / "README.txt").write_text(README, encoding="utf-8")
    (out / "THIRD-PARTY.txt").write_text(THIRD_PARTY, encoding="utf-8")
    shutil.copy2(ROOT / "LICENSE", out / "LICENSE.txt")
    archive = shutil.make_archive(str(RELEASE / name), "zip", staging_root, name)
    size = Path(archive).stat().st_size / (1024 * 1024)
    played = update_folder(out, RELEASE / name)
    shutil.rmtree(staging_root)
    print(f"\nZIP: {archive} ({size:.1f} MB)\nRelease folder (updated, your settings kept): {played}")
    return 0


MANIFEST = ".release-files"


def update_folder(staged: Path, target: Path) -> Path:
    """Copies the staged release over `target`. Only files the release ships are written; files a previous release
    shipped and this one does not are removed (listed in .release-files); anything else (config.ini, controls.ini,
    memcards, states, logs, game_data, mods, textures) is left alone."""
    shipped = sorted(p.relative_to(staged).as_posix() for p in staged.rglob("*") if p.is_file())
    previous = []
    if (target / MANIFEST).exists():
        previous = (target / MANIFEST).read_text(encoding="utf-8").splitlines()
    for rel in shipped:
        destination = target / rel
        if rel.endswith("README.txt") and rel.split("/")[-2:-1] in (["mods"], ["textures"]) and destination.exists():
            continue
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(staged / rel, destination)
    for rel in set(previous) - set(shipped):
        stale = target / rel
        if stale.is_file():
            stale.unlink()
            print("  removed old " + rel)
            folder = stale.parent
            while folder != target and folder.is_dir() and not any(folder.iterdir()):
                folder.rmdir()
                folder = folder.parent
    (target / MANIFEST).write_text("\n".join(shipped) + "\n", encoding="utf-8")
    return target


if __name__ == "__main__":
    sys.exit(build_launcher() if sys.argv[1:] == ["launcher"] else make_release())
