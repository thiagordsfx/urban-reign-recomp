# Port do Urban Reign para PC — guia avançado

[English](ADVANCED.md) · **Português (Brasil)** · [← voltar ao README](../README.pt-BR.md)


---

> [!IMPORTANT]
> Este repositório **não contém nenhum arquivo do jogo** (ISO, executável, módulos IRX, arquivos `.AFS`) nem o
> código C++ gerado a partir deles. Você precisa da **sua própria cópia legal** de Urban Reign para PS2.

## Sumário

- [O que é isto?](#o-que-é-isto)
- [Status atual](#status-atual)
- [Estrutura do repositório](#estrutura-do-repositório)
- [Requisitos](#requisitos)
- [Compilando e rodando](#compilando-e-rodando)
- [Controles e configurações](#controles-e-configurações)
- [Como funciona](#como-funciona)
- [Ferramentas de depuração](#ferramentas-de-depuração)
- [Documentação](#documentação)
- [Créditos e licença](#créditos-e-licença)

## O que é isto?

Em vez de emular o PlayStation 2 instrução por instrução, este projeto **traduz o executável original do jogo
(`SLUS_212.09`) para C++** com o [PS2Recomp](https://github.com/ran-j/PS2Recomp) e o compila como um programa
nativo do Windows, ligado a um runtime que reimplementa o hardware e o sistema do PS2 (kernel do EE, DMA, VU0/VU1,
IPU, CD/DVD…). O processador de E/S (IOP) roda os **módulos IRX originais do jogo** (driver de som `NUSOUND`,
`CRI_ADXI`, `PADMAN`, `MCMAN`/`MCSERV`…) num interpretador R3000A. A imagem é desenhada pelo **renderizador de
hardware do GS do PCSX2** (DX11): resolução interna, filtros e pacotes de texturas HD funcionam como no PCSX2.

## Status atual

<a href="status/details.pt-BR.md"><img src="status/map.pt-BR.svg" alt="Mapa de status do port: funções das bibliotecas do SDK da Sony e componentes do hardware do PS2, coloridos por status" width="100%"></a>

Detalhe por biblioteca e por componente: [`docs/status/details.pt-BR.md`](status/details.pt-BR.md).
Gerado a partir de [`docs/status/data.toml`](status/data.toml) por `python tools/status/generate.py`.

| Marco | Status |
|---|:---:|
| Recompilar `SLUS_212.09` para C++ (9.878 funções, com o mapa do Ghidra) | ✅ |
| Boot, menus, memory card (`Mcd001.ps2` compatível com o PCSX2), controle | ✅ |
| Lutas e modo história | ✅ |
| Áudio: SFX, música e vozes (SPU2 + CRI ADX) | ✅ |
| Desempenho: luta de 8 a 60 FPS estáveis, de 1x a 8x de resolução interna (GTX 1660 SUPER) | ✅ |
| Renderizador do PCSX2: FXAA, filtro de textura, nitidez CAS, shaders de TV, texturas HD | ✅ |
| Vídeos Sofdec (IPU emulado) | ✅ |
| Widescreen de verdade (o modo 16:9 do próprio jogo, ligado por padrão: mais da cena, não o 4:3 esticado) | ✅ |
| Multitap (até 8 controles; porta 2 por padrão, como o jogo espera) | ✅ |
| Controles pelo SDL3 (mapeamento no estilo do PCSX2, vibração) | ✅ |
| Versão pronta para jogar (1.4.1) | ✅ |
| Mods: menu no jogo (F2), .pnach ao vivo, mods em DLL + kit, CoopTool (multiplayer até 8, humanos também; Free Mode até 4) | ✅ |

O histórico completo da investigação está em [`docs/PROGRESS.md`](PROGRESS.md), e o plano em
[`docs/ROADMAP.md`](ROADMAP.md).

## Estrutura do repositório

| Caminho | Conteúdo |
|---|---|
| `build.py` | menu de build e execução (ver abaixo) |
| `src/` | o port: `main.cpp` (boot, `config.ini`/`controls.ini`), verificação do disco, ligações HLE do SDK, hooks de função, perfis |
| `recomp/` | configuração do recompilador (`urban.toml`) e o mapa de funções do Ghidra (`urban_functions.csv`) |
| `runtime/` | runtime, IOP, recompilador e ferramentas offline do PS2Recomp, com as mudanças deste port ([`runtime/README.md`](../runtime/README.md)) |
| `thirdparty/` | o GS do PCSX2 (só os arquivos usados), o nosso wrapper `pcsx2gs.dll`, DirectX-Headers ([`thirdparty/README.md`](../thirdparty/README.md)) |
| `tools/` | scripts de geração de código, análise, benchmark, depuração e release |
| `docs/` | diário de progresso, roadmap, variáveis de ambiente, mapa de status, licenças |
| `game/` | a **sua** imagem do disco fica aqui (não versionada) |
| `textures/` | pacotes de texturas HD, `textures/SLUS-21209/replacements` (não versionados) |
| `bench/`, `build/`, `generated/` | savestates e resultados de benchmark, o build, o C++ gerado — **não versionados** |

## Requisitos

- Windows 10/11 x64, Visual Studio 2022 (C++ para desktop, com CMake e Ninja), Python 3.10+
- GPU com DirectX 11
- Sua imagem de **Urban Reign (USA)** em `game/`; o `SLUS_212.09` extraído é conferido por hash
  (`SHA-256 60399090fc69ffdb4c39afd30977ab50890b0e33ed6bd9374e4d9497f4246081`)
- Opcional: [Ghidra](https://ghidra-sre.org/) 12.x + JDK 21 + a extensão
  [ghidra-emotionengine-reloaded](https://github.com/chaoticgd/ghidra-emotionengine-reloaded), para regenerar o
  mapa de funções (`python tools/ghidra_functions.py`); o mapa já está em `recomp/`

## Compilando e rodando

```bat
git clone <este repositório>
cd UrbanRecomp
:: coloque a imagem do seu disco em game\
python build.py
```

`python build.py` abre um menu numerado:

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

Siga os passos em ordem: **1** (extrai o ELF do disco e gera o `generated/`), **2** e **3**. O primeiro build completo leva
mais de uma hora (≈10 mil arquivos gerados); os seguintes só recompilam o que mudou. Os mesmos passos sem o menu:
`python build.py generate`, `python build.py build`, `python build.py play`.

- 8 jobs em paralelo por padrão (`URBAN_JOBS` muda). `URBAN_DEPS` pode apontar para uma pasta `_deps` do
  FetchContent já baixada (raylib, fmt...), para não baixar de novo.
- Os arquivos gerados acima de 900 KB vão para `urban_game_code_huge` e são compilados no máximo 2 por vez
  (`URBAN_HUGE_JOBS`), para o MSVC não ficar sem memória.
- **Microcódigo do VU1 compilado** (~4x mais rápido no 3D): capture o microcódigo com `PS2X_VU1_CAPTURE=<pasta>`,
  depois `python build.py configure -DURBAN_BUILD_TOOLS=ON`, `python build.py build vu1_recomp` e
  `build\port\runtime\ps2xTest\vu1_recomp.exe <pasta> generated\vu1`; o próximo build inclui `generated\vu1`.
- Rebuild só do host: `python build.py configure -DURBAN_GAME_CODE_LIBRARY=build/port/src/urban_game_code.lib`.

### Release

`python build.py release` monta o que vai num release do GitHub: `build/release/UrbanReignPC-1.2.0.zip` com `game/`
(`urban.exe`, só as DLLs que ele carrega, `pcsx2gs.dll` + resources, `mods/`, `textures/`), README, LICENSE e
THIRD-PARTY. Não vai nada do disco nem nada que o jogo grava ao rodar. Rodar de novo atualiza a
`build/release/UrbanReignPC-1.2.0` no lugar: configurações, memory cards, states, mods e texturas dela ficam.

## Controles e configurações

| PS2 | Teclado |
|---|---|
| ✕ / ○ / □ / △ | ↓ / → / ← / ↑ (setas) |
| Analógico esquerdo | W A S D |
| L1 / R1 / L2 / R2 | Q / E / 1 / 3 |
| L3 / R3 | F / G |
| START / SELECT | Enter / Tab |

Os controles passam pelo SDL3 (Xbox, PlayStation e outros, com vibração). O `controls.ini` ao lado do `urban.exe`
usa o formato do PCSX2: `[Pad1]`...`[Pad8]` com uma linha `Botão = Fonte/Elemento` por ligação (`Cross = Keyboard/Down`,
`Cross = SDL-0/FaceSouth`, `LargeMotor = SDL-0/LargeMotor`), então dá para copiar as seções do `PCSX2.ini`; `SDL-<n>`
é o número do controle (aparece no console quando ele conecta). Multiplayer: há um multitap na porta 2 (`[Pad]
MultitapPort1/MultitapPort2`, até 8 controles); jogadores: Pad1 = porta 1, Pad2 = porta 2 slot A, depois
os slots B-D da porta 2 e os B-D da porta 1 (a porta 2 primeiro, ao contrário do PCSX2: o jogo usa ela para o
P3/P4, então o multitap da porta 1 só acrescenta os jogadores 6-8; P5-P8 humanos com o CoopTool). Um `controls.ini` de versão antiga é convertido na primeira execução (o
`controls.ini.old` guarda o antigo). **F1** abre o depurador do runtime (ImGui), **F3** o contador de FPS, **F5/F9**
salvam/carregam o estado rápido, **F6** inicia/para uma captura de desempenho (**F7** marca um momento nela), **F8**
grava uma captura de depuração (RAM + trace do VU1), **F11** alterna a tela cheia.

O jogo abre em **progressive scan (480p)**, como o patch "Force Progressive Scan" do PCSX2: até 60 FPS em vez de 30
(`URBAN_PROGRESSIVE=0` no `config.ini` `[Runtime]` volta ao modo entrelaçado).

O vídeo é configurado no `config.ini` ao lado do `urban.exe` (criado na primeira execução; um arquivo antigo ganha as
opções novas com o valor padrão):

- `InternalResolution=1..8` (1 = nativa, 2 = 1280×896, 3 = 1920×1344...), `StartMaximized`, `Stretch`.
- `AspectRatio=16:9` (padrão: widescreen de verdade, o modo 16:9 do próprio jogo com a proporção exata), `4:3` (a
  imagem original) ou `auto` (como no menu Options do jogo).
- `Renderer=pcsx2` (padrão: o GS do PCSX2 com as correções do GameIndex para o Urban Reign) ou `native` (o GS
  próprio do port).
- Filtros do PCSX2: `FXAA` (ligado por padrão), `TextureFiltering`, `TrilinearFiltering`, `AnisotropicFiltering`,
  `Sharpening` (CAS), `ColorCorrection` (brilho/contraste/saturação/gama), `TVShader` (scanlines, CRT...),
  `ScreenFilter`, `Dithering`, `BlendingAccuracy`.
- Texturas HD: pacotes de texturas do PCSX2 (PNG/DDS numa pasta `replacements`) em `textures\SLUS-21209` ao lado do
  `urban.exe` ou no repositório, ou na pasta de `HDTexturesFolder`; `HDTextures=0` desliga.

## Como funciona

1. **Recompilação**: o `ps2_recomp` traduz cada função MIPS do `SLUS_212.09` para uma função C++. As funções são
   descobertas pelo próprio recompilador e pelo mapa do Ghidra; as duas listas são somadas, porque funções
   chamadas só por ponteiro (como métodos de vtable) ficariam sem entrada.
2. **SDK**: funções conhecidas do SDK da Sony (SIF, CD, E/S de arquivo…) são localizadas por assinatura e trocadas
   por implementações HLE do runtime na inicialização.
3. **Runtime**: o escalonador do EE executa as threads do jogo; DMA, VIF, VU0/VU1 (compilados antes) e IPU são
   emulados; os pacotes GIF vão para o GS do PCSX2 numa thread própria; o IOP interpreta os IRX originais, inclusive
   o driver de som e o PADMAN.
4. **Mudanças**: tudo que precisou mudar no PS2Recomp para este jogo está em `runtime/`, explicado no
   [`docs/PROGRESS.md`](PROGRESS.md).

## Ferramentas de depuração

| Ferramenta | Uso |
|---|---|
| `tools/autotest/drive.py` | teste automático: abre o jogo, reconhece as telas e navega até a luta (`--route challenge/story`, `--mash` para teste de estabilidade) |
| `tools/autotest/bench_state.py` | quadros/s num savestate de `bench/states` (opção 6 do menu) |
| `tools/vu_disasm.py` | desmontador de microcódigo do VU (pares upper/lower) |
| `tools/vu1_trace.py` | lê o trace do VU1 (F8 ou `PS2X_VU1_TRACE`): microprogramas, registradores e memória de dados |
| `tools/vif_chain.py` | decodifica chains de DMA, códigos VIF e pacotes GIF |
| `tools/gs_vram.py` | converte a VRAM do GS em PNG |
| `tools/frame_times.py`, `tools/present_pacing.py` | ritmo dos quadros a partir do `PS2X_FRAME_TIMES` ou de uma captura F6 |
| `tools/ghidra_functions.py` | importa/analisa o ELF no Ghidra (headless) e exporta o mapa de funções |
| `tools/status/generate.py` | gera o mapa de status (SVG) e as tabelas de detalhe |
| `tools/pine.py` | lê a RAM do jogo no PCSX2 via PINE, para comparar com o port |

As variáveis de ambiente de depuração (`URBAN_*` e `PS2X_*`) estão em [`docs/ENV.md`](ENV.md).

## Documentação

- [`docs/PROGRESS.md`](PROGRESS.md) — diário da investigação, com endereços, logs e o porquê de cada mudança
- [`docs/ROADMAP.md`](ROADMAP.md) — plano, critérios de desempenho e bugs conhecidos
- [`docs/ENV.md`](ENV.md) — variáveis de ambiente, teclas e uso do PCSX2 como referência
- [`docs/GS_PCSX2.md`](GS_PCSX2.md) — como o GS do PCSX2 é compilado e ligado
- [`docs/GAME.md`](GAME.md) — referência técnica do jogo (SDK, middleware, IRX, microcódigo)
- [`docs/status/`](status/) — mapa de status e detalhes

## Créditos e licença

- **Michael** — patch "Force Progressive Scan" do Urban Reign (60 FPS), reproduzido no port.
- [**PS2Recomp**](https://github.com/ran-j/PS2Recomp), de ran-j e colaboradores — recompilador e runtime (GPL-3.0).
- [**Port de Shadow of the Colossus**](https://github.com/LightVelox/sotc-vibe-pc) (sotc-vibe-pc), de LightVelox — o fork do runtime usado aqui (IOP emulado, GS em OpenGL,
  SPU2, SIO2, depurador ImGui, controles) e o padrão de ligação do SDK por assinatura (GPL-3.0).
- [**God of War PS2 recomp**](https://github.com/KIexster/god-of-war-recomp), de KIexster — correções de VU1
  (EATAN, JR/JALR) e de VIF UNPACK, e o gerador do mapa de status, adaptado em `tools/status/generate.py` (GPL-3.0).
- [**PCSX2**](https://pcsx2.net/) (GPL-3.0) — o GS de hardware (`pcsx2gs.dll`, com a troca de texturas e os shaders),
  referência de comportamento do hardware (via PINE) e da semântica das instruções (MMI, SIO2, SPU2).
- [**Recompilação estática do Ace Combat 5**](https://github.com/sal063/Ace-Combat-5-Static-recompilation) (Apache-2.0) — a emulação do IPU (decodificador MPEG dos FMVs),
  em `runtime/ps2xRuntime/src/lib/ipu/` (licença em `docs/licenses/AC5-Recomp-APACHE-2.0.txt`).
- [**Ghidra**](https://ghidra-sre.org/) (NSA, Apache-2.0) e
  [**ghidra-emotionengine-reloaded**](https://github.com/chaoticgd/ghidra-emotionengine-reloaded), de chaoticgd —
  análise do ELF e mapa de funções.
- [**raylib**](https://www.raylib.com/) (zlib), [**Dear ImGui**](https://github.com/ocornut/imgui) (MIT),
  [**DirectX-Headers**](https://github.com/microsoft/DirectX-Headers) (MIT) e [**FFmpeg**](https://ffmpeg.org/)
  (LGPL) — bibliotecas usadas pelo runtime.
- *Urban Reign* © Namco / Paradigm Entertainment. Este projeto não é afiliado nem endossado pela Bandai Namco.
  Nenhum conteúdo do jogo é distribuído.

O código deste repositório é distribuído sob a licença **GPL-3.0**, como o PS2Recomp. Veja [`LICENSE`](../LICENSE).
