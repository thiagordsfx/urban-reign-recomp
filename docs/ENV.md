# Variáveis de ambiente e teclas


| Variável | Uso |
|---|---|
| `URBAN_ISO` | caminho da ISO |
| `COOPTOOL_TRACE=1` | (mod CoopTool) loga as mudanças da cena (`0x9449F8`), missão, `on`/IA do P5 e da fase (`0x5E8C88`, `0x943D98`, `0x4453E4`, `0x652974`) |
| `COOPTOOL_MISSION=<n>` | (mod CoopTool) com uma fase do Free Mode escolhida, carrega o pacote `mission<n>.ppk` em vez do da tabela (para testar outras missões da mesma fase) |
| `URBAN_HW_WATCH_RW=1` | junto de `URBAN_HW_WATCH`, o watchpoint pega leituras também (loga "access", sem o valor) |
| `COOPTOOL_LIVE=1` | (mod CoopTool) liga os bots extras mesmo com a luta rodando (jeito antigo; cena preta / câmera voando) |
| (tecla F8) | grava `ram_<tick>.bin` e um trace do VU1 de 2 fields (`vu1trace_<tick>.bin`) na pasta do executável |
| `URBAN_DEBUG_UI=1` | abre o Runtime Debugger (ImGui) no início; F1 abre/fecha a qualquer momento |
| `PS2X_IOP_HLE_MCSERV=0` | (runtime) usa o MCSERV/MCMAN originais do jogo no IOP emulado em vez do HLE |
| `PS2X_GS_SIGNAL_STALL=0` | (runtime) desliga o stall do SIGNAL no GS |
| `URBAN_TRACE` | `BOOT,LOADER,FILE,EE,IOP,GS,VU,SPU2,INPUT,GAME,HOOK` ou `ALL` |
| `URBAN_MISSING_FUNCTION=continue` | não para quando o jogo pula para um endereço sem função recompilada |
| `URBAN_HANG_DUMP=<s>` | após `<s>` s: pilhas nativas, threads/semáforos do EE, DMA, timers, INTC, GS CSR/IMR e amostras do PC do jogo (`URBAN_HANG_DUMP_REPEAT=<s>`, `URBAN_HANG_DUMP_NATIVE=0`) |
| `URBAN_DUMP_MEM=addr:len,...` | junto do hang dump, hex dump da RAM do jogo |
| `URBAN_TRACE=HOOK` | loga cada chamada HLE com argumentos e retorno |
| `URBAN_HLE_SKIP=nome,...` | deixa essas funções do SDK como código recompilado |
| `URBAN_CALL_PROFILE=1` | conta chamadas por função (mostrado no hang dump) |
| `URBAN_CALL_TRACE=addr,...` | loga a entrada dessas funções (`$a0-$a3`, `$s0-$s3`, `$sp`, `$ra`); `URBAN_WATCH=ptr:off` mostra um byte junto |
| `URBAN_WATCH_POLL=ptr:off,=addr` | thread do host vigia memória e loga mudanças com o PC do jogo (aproximado) |
| `URBAN_HW_WATCH=addr,len` | watchpoint de hardware (DR0) na RAM do jogo: PC exato de cada escrita (`URBAN_HW_WATCH_STACK=1` mostra quem escreveu no host) |
| `PS2X_SCHED_STATS=1` | (runtime) estatísticas por segundo do laço do escalonador do EE |
| `PS2X_GS_SIGNAL_LOG=1` | (runtime) SIGNAL/FINISH do GS, fila e interrupções |
| `URBAN_DUMP_RAM=f` / `URBAN_DUMP_VRAM=f` | junto do hang dump, salva a RAM (32 MB) / VRAM do GS (4 MB) |
| `PS2X_PAD_SCRIPT=t:botões[:dur],...` | (runtime) input scriptado na porta 1, ex. `12:start:0.3,40:right+cross`; tempos em s a partir da 1ª leitura do pad (`tv` = fields) |
| `PS2X_SIO2_RECV1=0/1/2` | (runtime) RECV1 de transferências SIO2 com vários comandos; o `urban.exe` usa 2 (conectado se algum respondeu), sem isso o PADMAN não aceita o controle |
| `PS2X_IOP_VBLANK_HANDLERS=0` | (runtime) desliga a chamada dos handlers do `RegisterVblankHandler` no IOP |
| `PS2X_GS_DOWNLOAD_LOG=1` | (runtime) loga cada download GS → EE (TRXDIR=1) |
| `PS2X_VU1_JR_DELAYED_VI=1` | (runtime) JR/JALR do VU1 voltam a usar o VI atrasado (comportamento antigo) |
| `PS2X_VU0_LOG=<n>` | (runtime) para as `n` primeiras chamadas de cada microprograma do VU0: código e VF1-VF8 antes/depois |
| `PS2X_AUDIO_DUMP=f` | (runtime) grava a saída do SPU2 em PCM cru (s16le, estéreo, 48 kHz) |
| `PS2X_SPU2_LOG=<n>` | (runtime) log do ADMA e resumo do SPU2 a cada 5 s (registradores do mixer, key-on, ADMA, amostras) |
| `PS2X_IOP_THREAD_LOG=<s>` | (runtime) a cada `<s>` s, lista as threads do IOP (estado, prioridade, PC, espera) |
| `PS2X_IOP_IRQ_LOG=<n>` | (runtime) loga as `n` primeiras entregas de interrupção no IOP |
| `PS2X_PAD_FIFO=f` | (runtime) cada linha nova no arquivo (`botões[:s]`) é apertada na hora — usado por `tools/autotest/drive.py` |
| `PS2X_VU1_CAPTURE=dir` / `PS2X_VU1_VERIFY=1` / `PS2X_VU1_RECOMP=0` | (runtime) captura as imagens do VU1 para o AOT / compara AOT × interpretador / desliga o AOT |
| `PS2X_IOP_SEMA_LOG=<id>` | (runtime) loga cada Signal/Wait nesse semáforo do IOP (thread, PC, contador) |
| `PS2X_SPU2_ADMA=0` | (runtime) desliga o AutoDMA do SPU2 (volta a descartar as transferências ADMA) |
| `PS2X_VIF1_DOWNLOAD=0` | (runtime) desliga o download GS → EE pela FIFO do VIF1 (VIF1_STAT.FQC + leitura de 0x10005000) |
| `PS2X_GIF_CONTINUATION=0` | (runtime) desliga a continuação de GIFtag entre transferências (volta a cortar o resto do tag) |
| `PS2X_PAD_TRACE=<n>` | (runtime) loga as `n` primeiras transações de pad na SIO2 (comando/resposta) |
| `PS2X_SIF_DMA_LOG=1/2` | (runtime) loga cada `sceSifSetDma` do IOP→EE (2 = com os primeiros 128 bytes) |
| `PS2X_GIF_SUBMIT_LOG=<n>` | (runtime) loga os `n` primeiros pacotes GIF vindos do scratchpad |
| `PS2X_RPC_LOG=1` | (runtime) loga cada `sceSifCallRpc` (sid, função, buffers) |
| `PS2X_IPU=0` | (runtime) desliga a emulação do IPU (FMVs voltam a ficar pretos; liga de novo o HLE da libmpeg) |
| `URBAN_MPEG_HLE=1` | usa o HLE da libmpeg/libipu (`sceMpeg*`, `sceIpu*`) em vez das funções do jogo |
| `PS2X_IPU_TRACE=<n>` / `PS2X_IPU_STATS=1` | (runtime) os n primeiros acessos ao IPU (registradores, FIFOs, DMA) / comandos por segundo com o tempo de host |
| `PS2X_IPU_CATCHUP=0` | (runtime) não deixa o vblank recuperar atraso enquanto o IPU decodifica um FMV |
| `PS2X_THREAD_RET_ID=0` | (runtime) `SuspendThread`/`ResumeThread`/`WakeupThread` voltam a devolver 0 em vez do id da thread |
| `PS2X_SPR_INTERLEAVE=0` | (runtime) desliga o DMA de scratchpad em modo interleave e o toSPR em chain |
| `PS2X_IOP_CD_LOG=1` | (runtime) loga buscas e leituras de CD do lado do IOP |
| `URBAN_CALL_PROFILE_RANGE=lo-hi` | junto do `URBAN_CALL_PROFILE=1`, lista todas as funções chamadas nessa faixa de endereços (hex) |
| `PS2X_GS_RENDERER=pcsx2/nativo` | (runtime) GS do PCSX2 (`pcsx2gs.dll`, DX11; padrão) ou o nosso; no `config.ini`, `[Video] Renderizador` |
| `PS2X_PCSX2_FXAA`, `_TEXTURE_FILTERING`, `_TRILINEAR`, `_ANISOTROPY`, `_CAS`, `_CAS_SHARPNESS`, `_SHADEBOOST`, `_BRIGHTNESS`, `_CONTRAST`, `_SATURATION`, `_GAMMA`, `_TV_SHADER`, `_PRESENT_FILTER`, `_DITHERING`, `_BLENDING` | (runtime, GS do PCSX2) opções de imagem (valores em `pcsx2gs_api.h`); no `config.ini`: `FXAA`, `TextureFiltering`, `TrilinearFiltering`, `AnisotropicFiltering`, `Sharpening`, `SharpeningStrength`, `ColorCorrection`, `Brightness`, `Contrast`, `Saturation`, `Gamma`, `TVShader`, `ScreenFilter`, `Dithering`, `BlendingAccuracy` |
| `PS2X_PCSX2_VSYNC=0/1/2` | (runtime, GS do PCSX2) apresentação: 0 = sem esperar (pode rasgar), 1 = FIFO (espera o monitor), 2 = mailbox (padrão) |
| `PS2X_PCSX2_TEXTURES=0`, `PS2X_PCSX2_TEXTURE_DIR=dir`, `PS2X_PCSX2_TEXTURE_PRECACHE=1` | (runtime, GS do PCSX2) texturas HD: desliga / pasta do pacote (a que tem `replacements/`; padrão `textures\SLUS-21209` ao lado do exe) / carregar tudo ao abrir; no `config.ini`: `HDTextures`, `HDTexturesFolder`, `PreloadHDTextures` |
| `PS2X_PCSX2_STRETCH=1` (`Stretch=1`) | (runtime, GS do PCSX2) estica a imagem até a janela em vez de 4:3 com barras |
| `PS2X_FRAME_TIMES=f` com o GS do PCSX2 | (runtime) uma linha por apresentação na GSThread; "imagem nova" = field com >= 4 KB de GIF (sem hash da imagem) |
| `PS2X_PCSX2_SNAPSHOT_DIR=dir` | (runtime, GS do PCSX2) grava a imagem do GS (resolução interna) a cada `PS2X_PCSX2_SNAPSHOT_EVERY` fields (300) como `field-<n>.bmp` |
| `PS2X_GIF_DUMP=<s>:<fields>:<arquivo>` | (runtime, GS do PCSX2) grava o fluxo GIF enviado ao GS (mais a memória do GS no início) por `<fields>` fields a partir de `<s>` s; `tools/gif_dump.py` lista os desenhos (posição, textura, cor) e as transferências |
| `PS2X_IPU_PICLOG=<n>` / `PS2X_IPU_PICLOG_ALL=1` | (runtime) n linhas do decodificador: start codes que o FDEC acha (slices, picture, sequence) com os BDECs desde o anterior, cada CSC/BCLR com a posição das DMAs do IPU, e os inícios de DMA do IPU; `_ALL` mostra todo comando com o resultado |
| `PS2X_DMAC_CLEAR_LATCH=0` | (runtime) não limpa a conclusão de DMA guardada (canal desligado) quando o jogo limpa o bit no `D_STAT` (§92: FMVs deslocados) |
| `PS2X_ADMA_HOLD=0` | (runtime) com o bloco de ADMA atrasado, toca silêncio em vez de repetir a última amostra (§92: estalos nos vídeos) |
| `PS2X_ADMA_COPY=ahead/all/0` | (runtime) quando os blocos de ADMA são lidos da RAM do IOP: `ahead` (padrão) um bloco à frente, como o hardware; `all` a transferência inteira no início; `0` na hora de tocar (§94) |
| `PS2X_ADMA_DUMP=<arquivo>` | (runtime) grava cada bloco de ADMA tocado (cabeçalho de 4 bytes `A`, core, 0, 0 + 256 pares esquerdo/direito s16le) |
| `PS2X_MULTITAP=0/1/2/12` | (runtime; `controls.ini` `[Pad] MultitapPort1/2`, padrão porta 2) multitap na porta 1, 2 ou nas duas: até 8 controles, na ordem do PCSX2 (1A, 2A, 1B-1D, 2B-2D) |
| `PS2X_SDL_INPUT=0` | (runtime; `controls.ini` `[InputSources] SDL = false`) controles pelo GLFW como antes (sem vibração; `SDL-<n>` vira o gamepad n do GLFW). Padrão: SDL3 (§95) |
| `PS2X_PAD<n>=1`, `PS2X_PAD<n>_<TECLA>=<ligação>;<ligação>` | (runtime; escritas pelo `urban.exe` a partir do `[Pad<n>]` do `controls.ini`, formato do PCSX2) ligações de cada botão/direção do jogador n; sem `PS2X_PAD<n>` vale o padrão (teclado + `SDL-0` no jogador 1, `SDL-<n-1>` nos outros) |
| `PS2X_LOG_VIBRATION=1` | (testes) mostra no console cada mudança dos motores que o jogo manda para cada jogador |
| `PS2X_VSYNC_PHASE_MS=<ms>` | (runtime) onde o vblank emulado (e o present) cai na atualização do monitor, em ms depois do vblank dele; padrão 7 (antes 1: metade das imagens disputava a coleta do compositor, §96). Só vale com o lock ativo (`PS2X_VSYNC_LOCK`, monitor de ~60 Hz ou múltiplo: 120/180/240 Hz, onde vale a mesma fração da atualização mais curta, §97) |
| `PS2X_VSYNC_TEST_MULTIPLY=<n>` | (testes) finge um monitor n vezes mais rápido (vblanks intercalados) para exercitar o lock de 120/240 Hz num monitor de 60 Hz; a lisura medida nesse modo não vale nada |
| `PS2X_HOST_PRESENT=1` | (runtime) a janela do raylib apresenta quadros mesmo com o GS do PCSX2 (antes do §98: deixava a tela cheia preta) |
| `PS2X_PNACH_RELOAD=0` | (runtime) não recarrega os `.pnach` com o jogo rodando (padrão: recarrega quando `config.ini [Mods] Enabled` ou um arquivo ligado muda, e no F4; §106) |
| `PS2X_GAME_MENU=0` | (runtime) desliga o menu do F2 (§107) |
| `URBAN_MOD_DLLS=<A;B>` | (`config.ini [Mods] Dlls`) mods em DLL ligados: `mods\<Nome>\<Nome>.dll` (§109; kit em `modsdk/`) |
| `URBAN_START_FULLSCREEN=1` | (`config.ini` `[Video] StartFullscreen`) abre em tela cheia sem borda, como o F11 |
| `URBAN_MODS=<a.pnach;b.pnach>` | (`config.ini` `[Mods] Enabled`) os patches da pasta `mods` ligados; viram `PS2X_PNACH` se ele não estiver definido |
| `urban.exe --write-settings` | escreve/completa/converte `config.ini` e `controls.ini` com os comentários e os padrões, e sai |
| `PS2X_PRESENT_STATS=<arquivo>` | (medida) estatísticas do swap chain depois de cada present (GS do PCSX2): o que o monitor mostrou de fato; `python tools/present_stats.py <arquivo> <de_s> <até_s>` conta imagens repetidas e puladas |
| `PS2X_FAKE_PADS=<n>` | (testes) os jogadores 1..n contam como conectados, parados, sem gamepad |

As variáveis `PS2X_*` do runtime também valem (veja a documentação do port do SotC).


## PCSX2 como referência

`tools/pine.py` (do port do SotC) lê a RAM do jogo rodando no PCSX2 pelo PINE (habilite em Configurações >
Avançado, slot 28011). Serve para comparar estruturas do jogo entre o port e o emulador. O PINE só lê RAM; registradores
de hardware voltam 0.

