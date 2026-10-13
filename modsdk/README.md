# Urban Reign PC port — mod kit

**English** · [Português abaixo](#português)

A mod is **one DLL** with everything it needs inside (patches, options, code). Players drop its folder into
`game\mods` and switch it on in the launcher; its options appear as a tab of the in-game menu (**F2 → MODS / PATCHES**).

```
game\mods\MyMod\
  MyMod.dll     the mod (the folder and the DLL have the same name)
  mod.json      name, version, author, description (shown by the launcher and the menu)
  settings.ini  written by the game: the player's choices for this mod
```

## What a mod can do (`include/urban_mod_api.h`)

| Call | What for |
|---|---|
| `add_patches(name, description, pnach_text, enabled, show)` | Built-in PCSX2 `.pnach` lines. With `show = 1` they become a checkbox in the mod's tab; the player's choice is saved. All CodeBreaker types PCSX2 runs work (writes, increment, serial, copy, pointer, OR/AND/XOR, `D`/`E` conditionals). |
| `set_patches_enabled(id, on)` / `patches_enabled(id)` | Switch a patch group from your own code. |
| `read(address, out, size)` / `write(address, data, size)` | The PS2's main memory (32 MB) by PS2 address. |
| `set_tab(draw, user)` | Your own options under the checkboxes, drawn with the `UrbanModUi` calls (text, headings, checkboxes, buttons, scrolling areas...). Runs on the GS thread. |
| `on_frame(frame, user)` | Code at every frame (vblank) on the game thread, right after the patches. |
| `get_setting` / `set_setting` | Your own saved values (`settings.ini`). |
| `log(text)` | A line in the game's log. |
| `host->language` | `"pt"` or `"en"`, as the launcher. |
| `state_loads()` | (API 2) How many save states were loaded (F9...): when it changes, the game jumped to another moment. Check `host->version >= 2` first. |

The tab's UI (`UrbanModUi`) has text, headings, checkboxes, buttons, scrolling areas and, from version 2, combo boxes,
radio buttons, small buttons, separators with text, indent, item width and disabled blocks.

The DLL exports one function:

```c
#include "urban_mod_api.h"

URBAN_MOD_EXPORT int urban_mod_init(const UrbanModHost *host)
{
    host->add_patches("Infinite health (P1)", "Keeps P1's health full.",
                      "patch=1,EE,105A5FC8,extended,000005DC\n", 0, 1);
    return 1; /* 0 = do not load */
}
```

A full example (patches, its own tab with a live readout and a button, a frame callback): [`example/`](example).

## Building

Any C or C++ compiler for Windows x64 (MSVC, Clang, MinGW). With CMake:

```bat
cmake -S modsdk/example -B build/mymod -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build/mymod
```

Inside this repository the example is built with the game (`URBAN_BUILD_MOD_EXAMPLE`, on by default) into
`build/port/bin/mods/ExampleMod`.

## Rules of thumb

- Keep the menu code light: it runs at every frame while the menu is open.
- `read`/`write` from the tab or the frame callback; do not keep pointers into the game's memory.
- A crash inside a mod stops that mod (its tab says so), not the game.
- A DLL runs with the game's rights: only install mods you trust.
- The game is GPL-3.0: a mod that becomes part of it is expected to be open source too.

---

## Português

Um mod é **uma DLL** com tudo o que precisa dentro (patches, opções, código). Quem joga coloca a pasta dele em
`game\mods` e liga na aba Mods do launcher; as opções aparecem numa aba do menu do jogo (**F2 → MODS / PATCHES**).

- **Patches embutidos:** `add_patches(nome, descrição, texto_pnach, ligado, mostrar)`. Com `mostrar = 1` vira um
  checkbox na aba do mod, e a escolha do jogador fica salva. Funcionam todos os tipos de código que o PCSX2 roda.
- **Memória:** `read`/`write` pelo endereço do PS2.
- **Aba própria:** `set_tab(desenho, user)`, com as funções do `UrbanModUi` (textos, títulos, checkboxes, botões...).
- **A cada quadro:** `on_frame(função, user)`, na thread do jogo, logo depois dos patches.
- **Opções salvas:** `get_setting`/`set_setting` (`settings.ini` do mod).
- **Idioma:** `host->language` é `"pt"` ou `"en"`, como no launcher.
- **Save states:** `state_loads()` (API 2) conta os estados carregados; quando muda, o jogo pulou para outro momento.

Exemplo completo em [`example/`](example); para compilar, os comandos CMake acima. Um travamento dentro do mod para
só o mod, não o jogo. Uma DLL roda com as permissões do jogo: instale só mods de quem você confia.
