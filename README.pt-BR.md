<div align="center">

# Urban Reign — Port para PC

[English](README.md) · **Português (Brasil)**

**Urban Reign (PS2) rodando nativo no Windows: 60 FPS, alta resolução, texturas HD, até 8 jogadores.**

![Plataforma](https://img.shields.io/badge/plataforma-Windows%20x64-0078D6?logo=windows&logoColor=white)
![Versão](https://img.shields.io/badge/vers%C3%A3o-1.0.0-d9261c)
![Status](https://img.shields.io/badge/status-beta-yellowgreen)
![Baseado em](https://img.shields.io/badge/baseado%20em-PS2Recomp-8A2BE2)
![Licença](https://img.shields.io/badge/licen%C3%A7a-GPL--3.0-blue)

### [⬇ Baixar a versão mais recente](../../releases/latest)

</div>

---

> [!IMPORTANT]
> O jogo **não vem junto**. Você precisa da **sua própria cópia** do Urban Reign de PS2 (EUA, `SLUS-21209`) como
> imagem de disco (`.iso`).

## O que é isso?

Um port do *Urban Reign* para PC feito por **recompilação estática**: o código do PS2 foi traduzido para C++ e
compilado como um programa de Windows, em vez de ser emulado. Os gráficos usam o renderizador do PCSX2, então dá para
usar resolução interna alta, filtros de textura e pacotes de texturas HD.

- 60 FPS (progressive scan), inclusive nas lutas de 8
- Resolução interna de 1x a 8x, FXAA, nitidez, shaders de TV
- Pacotes de texturas HD (formato do PCSX2)
- Controles de Xbox, PlayStation e outros, com vibração; multitap para até 8 jogadores
- Memory card e save states (F5 / F9)

## Status

<a href="docs/status/details.pt-BR.md"><img src="docs/status/map.pt-BR.svg" alt="Mapa de status do port: funções das bibliotecas do SDK da Sony e componentes de hardware do PS2, coloridos por status" width="100%"></a>

Cada biblioteca do SDK da Sony que o jogo usa e cada componente de hardware do PS2, por status. Clique no mapa para
ver os detalhes.

## Baixar e jogar

1. Baixe o ZIP da [**versão mais recente**](../../releases/latest) e extraia em qualquer pasta.
2. Abra o **`Urban Reign.exe`**.
3. Na aba **Jogo**, escolha a sua imagem do Urban Reign (`.iso`).
4. Aperte **JOGAR**.

**Texturas HD (opcional):** baixe o
[Urban Reign Enhanced HD Textures Pack](https://gbatemp.net/threads/urban-reign-enhanced-hd-textures-pack-updated.665094/)
e extraia na pasta `game\textures` (tem que ficar `game\textures\SLUS-21209\replacements`).

**Mods:** coloque mods (uma pasta com a `.dll`) ou patches `.pnach` do PCSX2 em `game\mods` e ligue na aba
**Mods**; as opções ficam no menu do jogo (**F2**). Para criar o seu: o [kit de mods](modsdk/README.md#português).
O mod **CoopTool** já vem com o jogo (`mods/CoopTool`): Multiplayer com até 8 lutadores (até 4 humanos mais bots,
cada um por si ou times personalizados, fases do Free Mode) e missões do Free Mode com até 4 jogadores (quantidade
de inimigos, objetivo, personagem / IA / força / vida de cada lutador, parceiro opcional, enhanced camera com
L3 / R3). A aba **Como usar** dele explica cada parte.

### Requisitos

- Windows 10 ou 11, 64 bits
- Placa de vídeo com DirectX 11
- Processador com SSE4.1 (qualquer PC dos últimos ~12 anos)

## Controles

| PS2 | Teclado |
|---|---|
| ✕ / ○ / □ / △ | ↓ / → / ← / ↑ (setas) |
| Analógico esquerdo | W A S D |
| L1 / R1 / L2 / R2 | Q / E / 1 / 3 |
| L3 / R3 | F / G |
| START / SELECT | Enter / Tab |

Controles funcionam direto. Botões, qual controle é de cada jogador e o multitap ficam na aba **Controles**.

| Tecla | No jogo |
|---|---|
| F11 | Tela cheia |
| F3 | Contador de FPS |
| F5 / F9 | Salvar / carregar estado rápido |
| F2 | Menu do jogo: MODS / PATCHES |
| F4 | Recarregar os mods (.pnach) |

**Gravar com o OBS:** use *Captura de janela* com **Método de captura: Windows 10 (1903 e posterior)** (o padrão
"Automático"/BitBlt mostra a janela vazia), ou *Captura de tela*.

## Compilar do zero

Para quem quer mexer no código. Precisa de Windows, Visual Studio 2022 (C++ com CMake), Python 3.10+ e a sua imagem
do disco.

```bat
git clone <este repositório>
cd <pasta do repositório>
:: copie a sua .iso do Urban Reign (EUA) para a pasta game\
python build.py
```

O menu guia você: o **Passo 1** gera o código do jogo a partir do seu disco, o **Passo 2** compila (na primeira vez
leva mais de uma hora) e o **Passo 3** abre o jogo.

➡ **Todo o resto** (estrutura do repositório, arquivos de configuração, como funciona, ferramentas de depuração,
créditos completos) está no [**guia avançado**](docs/ADVANCED.pt-BR.md).

## Créditos

- Feito por **thiagordsfx** — [GitHub](https://github.com/thiagordsfx) · [YouTube](https://www.youtube.com/@thiagordsfx)
- [PS2Recomp](https://github.com/ran-j/PS2Recomp) (recompilador e runtime), [PCSX2](https://pcsx2.net/)
  (renderizador GS), [port do Shadow of the Colossus](https://github.com/LightVelox/sotc-vibe-pc),
  [recomp do God of War](https://github.com/KIexster/god-of-war-recomp),
  [recompilação do Ace Combat 5](https://github.com/sal063/Ace-Combat-5-Static-recompilation) e o patch "Force
  Progressive Scan" do Michael. Lista completa no [guia avançado](docs/ADVANCED.pt-BR.md#créditos-e-licença).
- *Urban Reign* © Namco / Paradigm Entertainment. Projeto de fãs, sem ligação com a Bandai Namco. Nenhum conteúdo do
  jogo é distribuído.

O código deste repositório é distribuído sob a licença **GPL-3.0**. Veja a [`LICENSE`](LICENSE).
