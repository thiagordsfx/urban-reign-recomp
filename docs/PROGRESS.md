# Diário de progresso

Registro do que foi investigado, com endereços, para não ter que redescobrir. Endereços são do `SLUS_212.09`
(Urban Reign USA, SHA-256 `60399090…6081`).

Nada foi commitado ainda. As mudanças no runtime estão na árvore de trabalho de `runtime` (branch local
`urban-port`, sobre o commit `ac9efa0` do fork do SotC) e também salvas em `docs/runtime-urban-port.patch`
(`git -C runtime apply ../../docs/runtime-urban-port.patch` para reaplicar).

## 2026-10-05 — Passo 1: build

- O `tools/ps2_recomp.exe` é idêntico (MD5) ao que o port do SotC compilou, então o código gerado bate com o runtime do
  fork `TaylorNAlbarnaz/PS2Recomp` branch `sotc-port`.
- Projeto CMake no modelo do SotC. O build completo leva cerca de 45 min.
- `sub_001A7AE8` (~11 mil instruções) ficou mais de 40 min no otimizador do MSVC com `/d2OptimizeHugeFunctions`. Os
  arquivos gerados acima de 900 KB vão para `urban_game_code_huge`, sem essa flag.
- Na primeira execução o driver compila os shaders do GS e a janela parece travada. O cache do SotC
  (`game_data/gs_shaders`) serve aqui também.

## 2026-10-05 — Passo 2: SDK e boot

### Identificação de funções (`tools/find_sdk_functions.py`)

O ELF não tem símbolos. As funções com nome do SotC servem de assinatura (SDK 3.0 lá, 2.8 aqui):

| Passada | Critério | Achadas |
|---|---|---|
| syscall | wrappers `addiu $v1,N; syscall; jr $ra` (SIF DMA/reg) | 8 |
| exact | sequência normalizada (alvos de `j`/`jal` e imediatos mascarados) única | 105 |
| near | perto do endereço previsto pelos vizinhos já achados, similaridade ≥ 80%, mesmo prólogo | 66 |
| order | sequência entre duas âncoras do SDK com a mesma contagem de funções | 10 |
| rpc | libcdvd pelo comando enviado ao servidor N (`0x80000592`) ou S (`0x80000593`) | 4 |

Lições:
- Funções curtas só podem casar de forma exata. A busca aproximada colocou o `sceSifGetReg` em `0x333A60`, mas o certo é
  o wrapper em `0x334270`.
- Só nomes do SDK são aproveitados: funções curtas de jogo do SotC geram falsos positivos.
- Vários endereços do SDK não aparecem como função separada no código gerado (ficaram dentro da função anterior) e o
  slot deles na tabela de dispatch é **nulo**. Ligar o HLE preenche o slot.
- Os números de comando da libcdvd seguem o PS2SDK: N1 Read, N5 Seek, S1 ReadClock, S3 GetDiskType, S4 GetError,
  S22 Break. O Urban não usa streaming (`sceCdSt*`).

### Ligações HLE (`src/urban/hle_bindings.cpp`)

- A lista é a mesma que o SotC liga por nome (`sotc.toml`), mais o restante da libcdvd. Só entram funções que foram
  localizadas.
- O trampolim replica o stub do recompilador: `ctx->pc = $ra` e depois o handler.
- Alias: `sceCdLayerSearchFile` → handler do `sceCdSearchFile` (o disco tem uma camada só).
- `recomp/urban.toml` usa `direct_tail_calls = false`; sem isso, saltos de cauda chamariam a função original direto,
  sem passar pela tabela.

### Sequência de boot observada

1. `sceSifInitRpc`, `sceCdInit`, `sceSifRebootIop("…IOPRP280.IMG")`, `sceSifSyncIop`, `sceSifLoadFileReset`, `sceFsReset`
2. `sceCdGetDiskType` (`0x31a5a8`) → `sceCdMmode(DVD)`
3. Módulos: SIO2MAN, MTAPMAN, PADMAN, MCMAN, MCSERV, LIBSD, SDRDRV, NUSOUND e CRI_ADXI, todos no IOP emulado
4. `GsSetCrt` (entrelaçado, NTSC)
5. Busca de `\FCACHE.DVD`, `\DVD000.AFS` e `\DVD001.AFS` via `sceCdLayerSearchFile`

### Bugs do runtime encontrados (corrigidos no patch)

**1. Host pacing travando o EE** (`EeScheduler::hostCatchUp`)
- Sintoma: `PS2X_SCHED_STATS=1` mostra `ckptSkip` em toda iteração, `calls=0`, `cycle == nextDeadline` e
  invocações pendentes crescendo sem parar.
- Causa: o catch-up pula até um field de ciclos, e isso roda o IOP pelo field inteiro. Com o IOP ocupado (CRI ADX e
  nuSound), pular um field custa um field de tempo real; o atraso nunca diminui e o código do jogo nunca executa.
- Correção: se o catch-up anterior não foi seguido de nenhum dispatch do jogo, o pacing desiste até o próximo VBlank
  reancorar a referência. Os contadores são estáticos no `.cpp` para não mexer no header (que está no PCH).

**2. `INTC_STAT` era um registrador de armazenamento comum** (`ps2_memory.cpp`)
- A espera de VSync da libkernel (`0x334380`) faz `SetVSyncFlag(&flag, &tick)` com as duas variáveis **na pilha**,
  escreve 4 em `INTC_STAT` para limpar o bit de VBlank e espera o bit 2 ligar **ou** o flag.
- Como o registrador lia de volta o 4 recém-escrito, a espera terminava na hora. No VBlank seguinte o runtime gravava
  o tick de VSync (8 bytes) num frame de pilha já reutilizado: estragava o `$s3` salvo por `0x122ca0`, o que fazia
  `cfg[232]` ficar 3, que indexava uma entrada nula em `0x3CC3D8` e crashava num `jalr` para zero (`0x13b4b8`).
- Correção: latch com write-1-to-clear, e VBON/VBOF ligados nos eventos de VBlank (`ps2xRaiseIntcStat`).
- Encontrado com `URBAN_HW_WATCH` + `URBAN_HW_WATCH_STACK`: a escrita vinha de `EeScheduler::processEvent`.

**3. Timer 3 parado**
- O handler de VSync (`0x128290`) e os limites de tempo (`0x140d40`, loop em `0x129ee0`) leem `T3_COUNT`.
- No PCSX2 o T3 conta ~31.500 Hz (525 ticks por field). O kernel da BIOS o deixa rodando para os alarmes; o runtime
  emula alarmes de forma nativa e nunca o ligava.
- Correção: `resetEeTimers` liga o T3 com `CUE | CLKS=H-BLANK`, e o clock H-BLANK passou para 31468 Hz.

## 2026-10-05 — Passo 3: GS, primeira imagem

Resolvido o impasse da tela preta. Cada item abaixo destravou o seguinte (todos no patch do runtime):

**4. SIGNAL do GS sem "stall"** (`ps2_memory.cpp`, `gs_frontend.cpp`)
- O jogo renderiza em pedaços: a cada par de SIGNALs (labels 1/2 e, no fim, 6/5) o handler de GS `0x10bc38` dispara o
  próximo pedaço (`0x10a5a0` VIF1, `0x10a790` GIF). No hardware, um SIGNAL que chega com `CSR.SIGNAL` ainda ligado
  trava o GS até o EE limpar o bit, então cada SIGNAL vira uma interrupção. O runtime processava o pacote inteiro e
  gerava uma interrupção só.
- Correção: fila de SIGNAL. Um SIGNAL que chega com o bit ligado (ou com fila não vazia) espera; quando o EE limpa
  `CSR.SIGNAL`, o próximo é aplicado. `ps2xGsSignalSerial` faz o escalonador ver cada entrega como nova borda.
- O FINISH também entra na fila quando há SIGNAL pendente. A lista final, disparada de dentro do handler, rodava na
  hora e o FINISH era apagado pelo `CSR = CSR | 1` (read-modify-write W1C) no fim do próprio handler.

**5. Interrupções mascaradas eram descartadas** (`EeScheduler::dispatchIrq`)
- O handler de GS roda com `iDisableIntc(GS)`; o SIGNAL liberado durante o handler era jogado fora. Agora fica
  pendente e é entregue no `iEnableIntc` (como o latch do `INTC_STAT`).

**6. `CreateThread` com prioridade 0 rejeitada** (`EeScheduler::createThread/changePriority`)
- O CRI ADX cria as threads de servidor com prioridade 0 e baixa logo depois. Sem elas, nenhuma leitura de AFS.

**7. `ChangeThreadPriority` devolvia `KE_OK`** (`Syscalls/Thread.cpp`)
- O kernel devolve a prioridade anterior. O lock/unlock do ADXM (`0x2de4d8` / `0x2de570`) restaura com esse valor;
  com 0, a thread principal ficava na prioridade 0 em espera ativa, e as threads da CRI passavam fome.

**8. `TEST.ZTE` ignorado** (CPU, raster e shader da GPU)
- A cópia final (FB 0, CT32 → FB `0x8c`, CT24, 224 linhas por campo) usa `TEST=0`, ou seja, `ZTE=0` e `ZTST=0`. O
  runtime tratava como "NEVER" e não escrevia nada: o buffer exibido ficava preto mesmo com a cena desenhada.
  Correção: `ZTE=0` significa teste de profundidade desligado (sempre passa), como no PCSX2.

Resultado: **primeira imagem** a ~30 s, a tela de acesso ao memory card ("Accessing. Please do not remove memory card
(PS2) from MEMORY CARD slot 1, reset or turn off the console."), com fonte e moldura corretas. O port roda a 60
apresentações por segundo e lê o disco normalmente.

Pipeline de renderização do jogo (para referência):
- Cena desenhada no FB 0 (CT32, 640×448), com sprites de texturas PSMT8 paletizadas.
- Cópia por campo para o FB `0x8c` (CT24, 640×224), com blend (`ALPHA=0x44`, TA0 de `TEXA`).
- Exibição pelo circuito 2 (`PMODE=0x62`, `DISPFB2=0x948c`, `SMODE2=3`: entrelaçado, modo campo).
- Ordering table de 8.192 slots (tags `NEXT`) na lista final; 0x910ec0/0x8f0e80 (buffer duplo).

## Estado atual: preso na tela do memory card

- A tela "Accessing..." não sai, com ou sem cartão (`PS2X_MEMCARD=0` dá o mesmo resultado).
- A `libmc` (2.8) faz `sceSifCallRpc` no `MCSERV` (sid `0x80000400`): `0xFE` (init) e depois `0x01` (GetInfo,
  NOWAIT, callback `0x323140`, que roda). Depois disso não há mais RPC de memory card.
- O `MCSERV` é HLE no IOP do runtime (`ps2xIOP/src/modules/mcserv.cpp`) e decodifica os números de função no layout
  do XMCSERV novo (`Flavor::NewXmcserv`, saída de GetInfo com 192 bytes, formato em +144). O Urban usa o MCSERV
  antigo (a string "libmc: too old release of mcserv.irx" está no ELF). Próximo passo: comparar o layout que a libmc
  2.8 espera (parâmetros e onde tipo/livre/formatado são escritos) com o que o HLE escreve.
- Os vídeos Sofdec (abertura depois do logo da Namco) vão sair pretos: o runtime foi compilado sem FFmpeg
  (`PS2X_ENABLE_FFMPEG=OFF`).

### Imagem intermitente: a cópia final roda fora de ordem

- O jogo chega **sempre** à tela "Accessing...". Com `PS2X_GS_GPU=0` + `URBAN_DUMP_VRAM`, o FB 0 (cena) mostra a
  janela com o texto completo. Às vezes (rodadas 72/73) ela aparece na tela, às vezes não.
- Nas rodadas pretas, o FB `0x8c` (exibido) tem exatamente 1 byte não zero por pixel: a cópia FB 0 → FB `0x8c` roda,
  mas copia preto (só alpha). Não é o cache de shaders (`PS2X_GS_SHADER_CACHE=0`), nem a GPU (`PS2X_GS_GPU=0`), nem o
  GIF/VIF assíncrono (`PS2X_MTVU=0` também falha).
- Causa provável: a fila de SIGNAL (correção 4) só segura o próprio SIGNAL. No hardware o GS para no SIGNAL e **todo**
  o restante do fluxo daquele path espera o EE limpar `CSR.SIGNAL`. Sem isso, os desenhos que vêm depois do SIGNAL no
  pacote (a cópia final) podem rodar antes dos pedaços que o handler `0x10bc38` ainda vai disparar (cena), conforme o
  tempo de cada thread.
- Próximo passo: emular o stall completo no front-end do GS. Quando um SIGNAL fica pendente, guardar o restante do
  pacote e os pacotes seguintes do mesmo path, e reprocessar em ordem quando o EE limpar o bit (até o próximo SIGNAL).
- Ao capturar a janela, use `tools/win/capture_window.ps1 -TitleEquals "Urban Reign"`: com `-TitleContains` a captura
  pegou outra janela com "Urban Reign" no título. Uma janela OpenGL coberta também pode sair preta no `PrintWindow`;
  o dump de VRAM é a verificação confiável.

Observação sobre o PCSX2 de referência: ele aplica 1 patch de jogo e 248 cheats (`SLUS-21209_BDD9BAAD.pnach` e os
`.pnach` de co-op/4 jogadores). Para comparações de memória confiáveis, rodar sem cheats. O arquivo `Documents/PCSX2/cheats/SLUS-21209_BDD9BAAD.pnach`
(gerado por um programa do usuário) foi apagado a pedido dele.

## 2026-10-05 — Referência: recomp de God of War (KIexster/god-of-war-recomp)

Clonado em `H:\JOGOS\GoW-Recomp-ref` (só leitura). Usa o PS2Recomp original (`c5a9d02`) com patches em `patches/`;
o nosso é o fork do SotC, outra base, então nada se aplica direto: cada item foi conferido no nosso código.

Aplicado no nosso runtime (conferido sem regressão):
- **VU1 EATAN**: coeficiente de t⁷ `-0.139085337519646` (valor do PCSX2; o fork tinha a errata `-0.130853…`).
- **VIF UNPACK V4-5**: RGB expandido para os bits 3..7 e alpha para o bit 7 (antes vinha cru, 0..31).
- **VIF UNPACK V2**: grava XYXY, como o hardware e o PCSX2 (antes preservava Z/W do destino). Nos quatro caminhos:
  SIMD, escalar rápido, escalar e genérico.

Anotado para quando chegarmos lá:
- **VU1 JR/JALR**: o GoW passou a ler o VI *atual*; o fork usa o valor atrasado (`readBranchVi`), que o PCSX2 só usa
  em desvios condicionais. Não mudei, porque o SotC funciona com a regra atual. Testar quando o 3D do Urban rodar.
- **XGKICK com snapshot do pacote** (opcional no GoW): se aparecer geometria corrompida no PATH1.
- **MPEG `sceMpegCbNodata`** (`ps2recomp-mpeg-nodata.patch`): o Urban liga `sceMpeg*` ao HLE; vale olhar quando os
  vídeos Sofdec entrarem (precisa de FFmpeg).
- **RPC do IOP antes das threads** (ESTADO.md §6): no IOP real, o servidor acorda threads de maior prioridade antes de
  responder; o emulador deles respondia antes e os dados iam para o buffer da requisição seguinte. Possível causa de
  problemas no streaming do ADX/CRI.
- Memory card: o GoW usa `MC2_D.IRX` (libmc2) sobre SIO2, que o nosso fork já emula. O Urban usa `MCMAN`/`MCSERV`
  (libmc antiga), que o nosso fork troca por HLE. Ideia para testar: tirar `mcman`/`mcserv` da lista de builtins do
  IOP (`iop_module_manager.cpp`) e rodar os IRX originais sobre o SIO2 emulado.

## 2026-10-05 — Memory card: o callback que nunca terminava

Cadeia completa, do sintoma à causa:
1. A tela "Accessing..." é a máquina de estados `0x21d338` (chamada todo quadro): estado 0 → `sceMcGetInfo`
   (`0x323198`); estado 1 → `sceMcSync(1)` (`0x323078`, via `0x21cbe8`) até voltar 1.
2. `sceMcSync` só conclui se `PollSema(sema 5)` tiver sucesso. Quem dá o `iSignalSema(5)` é o callback de fim de
   RPC da libmc (`0x323140`), que termina com `j 0x333e90` (iSignalSema).
3. O callback executa (é invocado pelo runtime), mas **o `iSignalSema` nunca roda**.
4. Causa: o recompilador criou uma função em `0x323150`, que é o **delay slot** do `beqz` em `0x32314c`, porque uma
   palavra na `.data` (`0x6BB1C0`) vale `0x323150` e foi tratada como ponteiro de função. `sub_00323140` passou a ter
   só 4 instruções; o caminho "não tomado" sai com `ctx->pc = 0x323154`, endereço sem entrada na tabela de dispatch.
   Dentro de uma invocação, o escalonador trata isso zerando o PC em silêncio, e o callback é abortado.
5. Correção no recompilador (`elf_parser.cpp`, `LooksLikeCallableEntry`): um início de função nunca fica num delay
   slot. De 31 funções do Urban que começavam em delay slot, sobrou 1 (`0x336688`, o `sceSifInitRpc` de verdade,
   descoberto por `jal`). Requer regenerar o código (`tools/generate.py`, com o `ps2_recomp.exe` compilado do fonte:
   `configure.bat -DURBAN_BUILD_RECOMP=ON` + `build.bat ps2_recomp`) e um build completo.

Também nesta etapa:
- **`PS2X_IOP_HLE_MCSERV=0`** (runtime, `iop_subsystem.cpp`): desliga o HLE do MCSERV, que intercepta o sid
  `0x80000400` mesmo com o `MCSERV.IRX` original carregado. Com isso, o MCSERV/MCMAN do próprio jogo rodam no IOP
  emulado sobre o SIO2 e o `Mcd001.ps2`. Com o HLE, o `GetInfo` responde "não formatado", porque o HLE lê a pasta
  `memcards/mc0` (vazia); com o MCSERV original e um cartão formatado: tipo 2, 4847 livres, formatado, retorno -1
  (correto).
- O `Mcd001.ps2` do port era um cartão apagado (todo `0xFF`). Foi trocado por uma cópia do cartão formatado do
  PCSX2 do usuário; o vazio ficou em `memcards/Mcd001.empty-backup.ps2`.
- Valores de vídeo no PCSX2 **sem cheats**: `cfg[234]` = 2 já aos 6 s e `cfg[233]` = 1 aos 12 s. No port eles ficam
  em 1/0 porque o jogo não passa do memory card (as funções `0x1373b0`/`0x137498`, que mudam o modo, nunca rodam).
- A cópia final FB 0 → `0x8c` cobre uma coluna de 64 px por quadro, com ou sem o stall: é o jeito como essa tela é
  desenhada nesse modo, não o bug.
- **Stall completo do SIGNAL no GS** implementado (`gs_frontend.cpp`; `PS2X_GS_SIGNAL_STALL=0` desliga). Mais fiel ao
  hardware, sem regressão observada.
- **Runtime Debugger em ImGui** (já existia no PS2Recomp, `ps2_debug_panel.cpp`) ligado no `urban.exe`
  (`URBAN_DEBUG_UI` no CMake, ligado por padrão): F1 abre/fecha; `URBAN_DEBUG_UI=1` abre no início.

## 2026-10-05 — Passou do memory card; vídeos

- Com o recompilador corrigido (delay slot) e o código regenerado, o callback da libmc completa (`iSignalSema(5)`
  roda) e o jogo **passa da tela do memory card**. O usuário viu o logo da Namco e depois tela preta; nas minhas
  capturas a cada 5 s o logo não aparece sempre, então ainda há uma parte não determinística a investigar.
- Depois do memory card a exibição muda para modo quadro (`SMODE2=0x1`) mostrando o FB 0 (`DISPFB=0x9400`), como no
  jogo real (`cfg[234]=2`).
- A tela preta seguinte é o vídeo de abertura (Sofdec): muitas leituras de CD, `sceSifSetDma`/RPC de áudio e
  `sceMpegAddCallback`. Ligar o FFmpeg (feito: `URBAN_FFMPEG`, DLLs copiadas para `bin/`) não basta: o M2V da CRI
  programa o **IPU** direto (não usa `sceMpegGetPicture`), e o runtime quase não emula o IPU.
- `urban.exe` agora usa por padrão o MCSERV/MCMAN originais (`PS2X_IOP_HLE_MCSERV=0` definido no `main.cpp` se não
  vier do ambiente).
- O FFmpeg passou a ser ligado de forma PRIVATE no `ps2_runtime`, para não mexer na linha de compilação do código do
  jogo (ligar/desligar só recompila o runtime).
- Runtime Debugger (ImGui) testado: funciona; F1 abre/fecha.

Próximo passo sugerido (feito na seção seguinte): pular os vídeos com START.

## Controle, tela inteira e menu (2026-10-05, noite)

Objetivo: pular os vídeos com START (o usuário confirmou no emulador que todos aceitam START) e chegar ao menu.

1. **O START nunca chegava ao jogo.** O PADMAN.IRX importa só `RegisterVblankHandler`/`ReleaseVblankHandler`
   (vblank 8/9) e lê os pads a partir desse handler; o IOP emulado ignorava o registro. Agora `iop_vblank.cpp`
   guarda os handlers (por borda, prioridade, retorno 0 remove, como o vblank.irx) e os chama em cada borda de
   VBLANK pelo laço do IOP (`serviceDue`/`nextEventCycle`, mesmo modelo do timrman). Com isso o PADMAN passa a mandar
   pacotes por `sceSifSetDma` para os padAreas do EE (0x35a480… porta/slot, 0x80 bytes por quadro).
2. **O PADMAN rejeitava o pad.** Ele lê as duas portas numa transferência SIO2 só; a porta 2 vazia deixava RECV1 em
   "sem resposta" (0x1D100) para a transferência inteira, e o PADMAN ficava para sempre mandando só 0x42 (estado
   EXECCMD → ERROR → DISCONN, visto no padArea em +0x70). Com `PS2X_SIO2_RECV1=2` (RECV1 conectado se algum comando
   respondeu; padrão no `main.cpp`) ele configura o pad (0x43/0x4C/0x46/0x47) e entra em modo DualShock (0x73).
   O START (0xF7 no byte 3) chega e os vídeos são pulados: **o jogo chega à tela PROFILE (New/Load Profile)**.
3. **Só uma coluna de 64 px da tela atualizava** (igual nos backends GPU e CPU). O desenho vai para o FB 0 completo;
   a cópia FB 0 → buffer exibido (FBP 0x8c, CT24) é uma lista REGLIST de sprites de 64 px montada no scratchpad em
   blocos (0x70000080/0x70002040) e enviada em várias DMAs do GIF, e **só o primeiro bloco tem o GIFtag**. O GS
   tratava cada bloco como pacote novo e jogava fora o resto. Agora o `processGIFPacket` guarda o tag pendente
   (PACKED/REGLIST/IMAGE: loops restantes, registrador, paridade do REGLIST) e continua no pacote seguinte, como o
   PATH3 do hardware (`PS2X_GIF_CONTINUATION=0` desliga). Resultado: tela de título "PRESS START BUTTON" e menus de
   perfil completos.
4. **Crash ao chegar no menu/carregar save** (o usuário também viu): `JALR` para 0x1d5cb8, que não era entrada de
   função. Está numa vtable do GCC 2.x em 0x455c7c (pares `{0xFFFF0000, ptr}`: 0x1d5ca0, 0x1d5cb8, 0x1d5d28,
   0x1d5d38) e é um método folha com o `jr ra` na 26ª instrução, fora da janela da heurística. O recompilador ganhou
   o formato `hasGcc2VtableShape` no scan de tabelas de ponteiros (prefixo 0xFFFFdddd e vizinho conservador a ±8
   bytes com o mesmo prefixo). Código regenerado: 6 arquivos mudaram.

5. **Ghidra.** Instalado em `H:\Tools` (Ghidra 12.1.4, JDK 21 Temurin só para ele via `support/launch.properties`,
   extensão `ghidra-emotionengine-reloaded` v2.1.38 para o R5900). `tools/ghidra_functions.py` importa e analisa o
   ELF em modo headless (projeto em `build/ghidra`, ~2 min), exporta com o `ExportPS2Functions.java` do PS2Recomp e
   filtra para `build/ghidra/urban_functions.csv` (só `FUN_*`; o export também lista ~33 mil labels). O Ghidra acha
   **1557 funções** que o recompilador emendava na anterior (1556 começam logo após `jr ra`/`j`), cada uma um crash
   em potencial se chamada por ponteiro; o recompilador acha 818 que o Ghidra não acha. O `urban.toml` passa a usar o
   mapa (`ghidra_output`, une com a descoberta própria). O 0x1d5cb8 o Ghidra também não achou; esse fica com a
   correção de vtable do recompilador.

6. **Crash ao renderizar 3D** (teste do usuário, logs em `build/port/bin/urban_err.log`): `JALR` para 0x106290, de
   0x1131b0, numa tabela `{0, fn}` em 0x6a6ac0 com quatro métodos folha longos seguidos (0x1061b0, 0x1061e8,
   0x106258, 0x106290; `jr ra` na 12ª instrução). A âncora `hasAlternatingPointerIdShape` exigia um vizinho
   conservador a ±8 bytes; agora percorre a sequência de pares (até 16 de cada lado). O Ghidra também não achava
   essas funções.
7. **Mapa do Ghidra somado, não substituído.** Carregar `ghidra_output` fazia o recompilador descartar as funções
   próprias que não estão no mapa (perderia as 818, inclusive 0x1d5cb8). Nova opção `ghidra_map_merge = true`
   (padrão false no PS2Recomp) mantém as duas; o CSV usa nomes `sub_XXXXXXXX` para os arquivos gerados não mudarem de
   nome. Resultado: **9878 funções** (antes 8296).

8. **Build sem memória (C1002) depois do mapa.** O `End` exportado pelo Ghidra é o maior endereço do corpo; funções
   com um pedaço de código compartilhado longe (ex.: 0x2ABAD8, 32 bytes, End 0x2C7C00) viravam faixas de ~118 KB
   repetidas em 9 arquivos de ~9,5 MB. `tools/ghidra_functions.py` agora usa `End = min(End, Start + Size)`.
   Efeito colateral bom: o maior arquivo gerado passou de 30 MB (0x2bcee0, o de 40 min de compilação) para 2 MB, e o
   `register_functions.cpp` de 16 MB para 7,3 MB. Também ficou um job pool do Ninja (`URBAN_HUGE_JOBS`, padrão 2)
   para os arquivos acima de 900 KB.

9. **Boot quebrado pelo corte do passo 8** (salto direto para 0x3317a4 sem destino): o `Size` do Ghidra pula buracos
   dentro do corpo (0x3313d8 tem 972 bytes, mas o código vai até 0x3317b8), e o `End` dele para no último
   endereço alcançado, deixando sem dono o código só alcançado por saltos (632 buracos). Correção no recompilador:
   no modo `ghidra_map_merge`, toda função vai até o próximo início da união das duas listas (teto de 128 KB). O
   script também corta no próximo início do Ghidra. Verificado com script: 9878 funções, **0 buracos com código e
   0 sobreposições**; maior arquivo gerado 2 MB.

10. **Teste do usuário com o mapa do Ghidra: o 3D renderiza** (personagens deitados/achatados, ainda a investigar no
    VU1) e há SFX na entrada da luta, mas o jogo congela quando a luta começa, com um áudio em loop. Thread principal
    presa em 0x315ae8 (libgraph, rotina de store-image): espera `VIF1_STAT.FQC != 0` e lê com `LQ` da FIFO
    0x10005000. O backend do GS já gerava os dados do download (TRXDIR=1, `localToHostTransfer`), mas só o HLE
    `sceGsExecStoreImage` os consumia. Agora `ps2_memory.cpp` tem uma FIFO de 16 qwords reabastecida do GS: o
    VIF1_STAT mostra FQC/FDR e o `read128` de 0x10005000 consome os dados (`PS2X_VIF1_DOWNLOAD=0` desliga).

11. **Download da VRAM deixava tudo lento e mudava o fluxo** (seleção de personagens lenta; ao confirmar, voltava
    à seleção). Com `PS2X_VIF1_DOWNLOAD=0` a luta funciona e dá para andar. No jogo inteiro só houve um download
    (`PS2X_GS_DOWNLOAD_LOG=1`: tick 2038, 320x224 CT24 de 0x3020). A causa provável: cada leitura do VIF1_STAT
    consultava o GS e travava o mutex dele. Agora o GS incrementa `g_gsDownloadSerial` a cada TRXDIR=1 e a memória só
    consulta o GS com download pendente (sobra de download antigo é descartada).
    Ainda lento depois disso. Voltar à seleção ao confirmar é **normal** (animação de chamada para a luta, segundo o
    usuário). Causa real: a libgraph do Urban grava FDR=1 no VIF1_STAT (0x315954: `sw 0x00800000`) antes do
    download e FDR=0 depois; o runtime ignorava essa escrita e mostrava sobras na FIFO para sempre. Agora o FDR
    escrito pelo EE é guardado, os dados só aparecem com FDR=1, e voltar a FDR=0 descarta a sobra.
12. **Personagens deitados/achatados na luta** (prints do usuário: cenário e HUD certos, corpos girados ~90° e
    achatados no chão). Suspeita: matrizes dos ossos. O jogo usa 30 `vcallms` (microprogramas do VU0) e muitas
    instruções COP2. `PS2X_VU0_LOG=<n>` registra, por endereço chamado, o código e VF1-VF8 antes e depois.

13. **Objetos do cenário também achatados** (print do usuário): não é só o skinning, é tudo que tem matriz de objeto;
    chão/paredes estáticos estão certos. Conferido e correto no código gerado: VOPMULA/VOPMSUB (produto vetorial),
    VRSQRT/VWAITQ/VMULQ, broadcasts de VMULA/VMADDA, vf0 nunca escrito, SQRT.S/RSQRT.S do FPU (o fork já lê FT).
    Portado do GoW o `ps2recomp-vu-jump.patch`: JR/JALR do VU1 usam o VI atual, como o microVU do PCSX2; o VI
    atrasado vale só para ramos condicionais (`PS2X_VU1_JR_DELAYED_VI=1` volta ao antigo). Microprogramas de objeto
    costumam fazer `IADDIU vi` + `JALR` para sub-rotinas de matriz.

14. **Log do VU0 durante a luta** (`PS2X_VU0_LOG=3`) e desmontador novo `tools/vu_disasm.py` (microcódigo VU, pares
    upper/lower; o microcódigo do VU0 do jogo é o MPG em 0x34aefc, 172 instruções, dados no offset 0x24bf00 do ELF).
    Rotinas: 0x000/0x038 seno (I=π/2 e 2π; resultados certos), 0x0F0 atan2 (estouro com entrada (0,0) igual ao do
    hardware), 0x360 transformação de vértice com perspectiva/clip em pipeline de software entre chamadas, 0x3C8/0x438
    matriz × ponto. O VU0 em modo micro parece correto. `PS2X_VIF_SIMD_UNPACK=0` + `PS2X_VU1_SIMD_CLIP=0` não
    mudam nada. O usuário notou: **objetos fixos do mapa certos; objetos dinâmicos e personagens achatados em Z**,
    às vezes partes do corpo certas, mas deitadas.
    Nova captura: **F8** na janela grava `ram_<tick>.bin` (RAM do EE) e um trace do VU1 de 2 fields
    (`vu1trace_<tick>.bin`, formato do `PS2X_VU1_TRACE`, com a imagem do microcódigo em `vu1_<hash>.bin`).

15. **Causa dos personagens deitados/achatados: permutações MMI erradas.** Caminho da investigação:
    - F8 na luta → `tools/vu1_trace.py` (leitor novo do trace do VU1): o microprograma de skinning é o 0x1D98
      (matriz do osso endereçada pelo w do peso). As matrizes de osso já chegavam ao VU1 com a coluna Y zerada
      (personagem A) ou giradas (B).
    - No dump da RAM, a hierarquia do personagem A em 0x59f240 (raiz certa, filhos com Y=0).
    - `URBAN_HW_WATCH=0x59f2c4,4`: 0x1017c8/0x1019c0 escrevem o valor certo e depois 0x101100 (grava vf1-vf4,
      chamada pela multiplicação 0x1010A0 a partir de 0x20bf84) escreve 0. A matriz multiplicada vem de
      `func_1018E8` → `func_101628` (Euler → matriz com o seno do VU0 e instruções MMI).
    - A função usa `pexew`; a macro `PS2_PEXEW` trocava pares vizinhos em vez de w0<->w2.
    Conferidas contra o manual do EE Core e o `MMI.cpp` do PCSX2, **seis permutações estavam erradas**: PEXEW,
    PROT3W, PEXCW, PEXEH, PEXCH, PREVH (só PCPYH certa). O jogo usa 5 pexcw, 3 pexew, 3 prot3w. Corrigidas nas
    macros (`ps2_runtime_macros.h`) e nas traduções (`mmi_translation_helpers.cpp`).

16. **Confirmado em teste automático (2026-10-06 madrugada): personagens em pé.** Após o build completo com as
    permutações MMI corrigidas, a navegação scriptada chegou a uma luta (Challenge: Brad vs Ty): closes da
    introdução e a luta com os dois personagens em pé, com sombra no chão (`build/runs/mp144`). Caminho de menus
    que funciona com `PS2X_PAD_SCRIPT` (toques de 0,4 s; toques de 0,15 s se perdem porque o jogo roda abaixo da
    velocidade real): START×10 (vídeos/título) → direita + ✕ (Load Profile) → ✕, esquerda, ✕ (perfil "brad") →
    baixo×N no MAIN MENU (CONTINUE, FREE, CHALLENGE, MULTIPLAYER…) → ✕ repetido.
    Observado: a seleção de lutador do modo história (Continue → missão 100) trava com a lógica rodando; com
    `PS2X_SPU2_ADMA=0` também trava, então não é o ADMA novo. A investigar.
17. **Áudio: ADMA do SPU2 implementado.** O `IopSpu2` descartava todo DMA com AutoDMA ligado (`if (autoDma) return`),
    que é como o CRI ADX manda música/vozes decodificadas no IOP. Agora `admaStart` toca blocos de 0x400 bytes
    (256 amostras L + 256 R) direto da RAM do IOP, a 48 kHz, como entrada de som do core (MMIX bits 0x80/0x40,
    volume BVOL), e o fim do DMA é agendado para quando o último bloco toca (`transferWords × 768` ciclos). O mixer
    também usava os bits de entrada (InpDry) para somar a saída do core0 no core1; pelo PCSX2 são os bits Ext
    (0x08/0x04). `PS2X_SPU2_ADMA=0` volta ao comportamento antigo.

18. **Áudio continua mudo — diagnóstico (sem solução ainda).** Ferramentas novas: `PS2X_AUDIO_DUMP=<arquivo>` (PCM
    s16le estéreo 48 kHz de tudo que o SPU2 gera), `PS2X_SPU2_LOG=<n>` (log do ADMA + resumo a cada 5 s: MMIX/ATTR/
    volumes e contadores de key-on, ADMA, amostras de voz e de saída), `PS2X_IOP_THREAD_LOG=<s>` (threads do IOP
    com estado/prioridade/PC/objeto de espera) e `PS2X_IOP_IRQ_LOG=<n>` (entregas de interrupção no IOP).
    Resultados numa run até a luta (Challenge):
    - saída 100% zerada; nos primeiros 5 s 96 key-on e 111 ADMA (core 0, blocos de 2048 bytes no ritmo certo:
      ~393 mil ciclos = 512 amostras), **depois disso nenhum key-on nem ADMA** pelo resto do jogo;
    - as vozes geram amostra zero já na origem (antes do mixer), então os gates do MMIX não são a causa;
    - registradores: core0 MMIX=0x0fc0, core1 MMIX=0x0fcc (Ext+Inp+Snd), MVOL=0x3fff, ATTR com bit de não-mudo,
      AVOL do core1=0x7fff, BVOL=0 nos dois cores;
    - IOP: o worker do NUSOUND (thread com entrada 0x51614) fica em `WaitSema(4)`; o NUSOUND só importa
      `SignalSema` (sem `iSignalSema`), então quem libera é a thread de RPC dele (0x502f8, esperando comando).
      Durante a luta o EE só manda RPC 0x90000200 funções 0x408/0x409; nenhuma que dispare som.
    - a IRQ 0x24 (DMA do SPU2 core 0) é entregue ao handler da libsd (0x49cb8) normalmente.
    O usuário ouviu alguns SFX numa build anterior (antes de VIF1 download, MMI, ADMA). Com `PS2X_SPU2_ADMA=0` o
    resultado é o mesmo, então não é o ADMA. Próximo passo: descobrir no lado do EE (biblioteca de som do jogo) por
    que nenhum comando de tocar é enviado; comparar com o PCSX2.

19. **Testes automáticos em malha fechada.** `PS2X_PAD_FIFO=<arquivo>` (runtime): cada linha nova
    (`botões[:segundos]`, mesmo formato do `PS2X_PAD_SCRIPT`) é apertada na hora. `tools/autotest/drive.py` abre o
    `urban.exe`, captura a janela a cada segundo, reconhece a tela por miniatura em tons de cinza contra
    referências em `build/autotest/refs/*.png` (capturas do jogo, fora do repositório) e aperta o botão certo até
    chegar à luta (Challenge, perfil 1), inclusive se recuperando de desvios (modo FREE por engano, "Quit Free
    Mode?"). Chega à luta em ~105 s de forma repetível. Os scripts por tempo fixo falhavam porque o tempo de
    carregamento varia entre runs (o título às vezes aparece depois do último START e o jogo entra na demo).
20. **Desempenho: microcódigo do VU1 compilado antecipadamente (AOT), como no SotC.** Medição na luta antes: ~6-7
    FPS, EE a 40-49 M ciclos/s (15% do tempo real), GPU ociosa (espera ~10 ms/frame) → gargalo no VU1 interpretado
    (~520 microprogramas por frame). O jogo inteiro usa **uma única imagem** de VU1 (`ddcd7ee32b8482b8`, 9 entradas).
    Fluxo: `PS2X_VU1_CAPTURE=build/vu1_capture` (driver até a luta) → `vu1_recomp build/vu1_capture generated/vu1`
    (ferramenta do ps2xTest; configure com `-DURBAN_BUILD_TOOLS=ON`) → biblioteca `urban_vu1_code` +
    `registerGeneratedVu1Programs()` no `main.cpp`. Verificação: `PS2X_VU1_VERIFY=1` até a luta, **688.128 execuções,
    0 divergências** contra o interpretador. Resultado: **~30 FPS na luta**, EE a ~165 M ciclos/s (4-5x mais rápido).
    `PS2X_VU1_RECOMP=0` volta ao interpretador.

21. **Áudio: funciona nas lutas; falta o streaming ADX (música/vozes dos menus).** Com o driver automático e o
    resumo do SPU2 (agora com o número de meias-palavras não nulas da RAM do SPU2):
    - a RAM do SPU2 fica vazia (1 meia-palavra) até perto da luta; ao carregar a luta os bancos de som sobem
      (141.530 meias-palavras), há key-on e a saída fica não nula — o RMS gravado com `PS2X_AUDIO_DUMP` mostra um
      fundo ~277 com picos de 1.700-2.900 (golpes). Amostra de 30 s: `build/autotest/fight_audio_sample.wav`.
      (As runs anteriores "mudas" paravam antes da luta.)
    - o worker do NUSOUND funciona: cada comando RPC (servidor em 0x519f0, ra 0x50c8c) faz `SignalSema(4)` e o
      worker (0x51614) processa (`PS2X_IOP_SEMA_LOG=4`).
    - o que falta é o ADX da CRI: 114 blocos de ADMA no boot (~1,2 s, provavelmente o jingle do logo) e depois
      nunca mais — sem música no título/menus. A investigar: por que a CRI não reinicia o ADMA (lado do EE:
      leitura ADXF do AFS e envio por SIF ao `CRI_ADXI`; lado do IOP: `sceSdBlockTransStatus` lê a posição pelo
      MADR do DMA, que o ADMA emulado não avança).

22. **Teste de estabilidade** (`drive.py --mash --hold 300`): chega à luta e aperta botões aleatórios. Em 10 min
    o jogo passou por luta, derrota (Challenge), volta ao MAIN MENU e entrada no MULTIPLAYER SETUP **sem nenhum
    crash, "missing-target" ou travamento**. O driver agora considera luta qualquer tela desconhecida depois de
    "Now Loading" (os cenários variam) e lista no fim as linhas de problema do `urban.err`.
    Pendências conhecidas: seleção de lutador do modo história (Continue → missão) congelou em testes anteriores;
    VU0 em modo micro ainda interpretado (~1/3 do tempo da thread do jogo na luta) — próximo alvo de desempenho
    (adaptar o `vu1_recomp` às máscaras de 4 KB do VU0 e criar uma verificação para ele).

23. **Modo história: congela na seleção de lutador (reproduzível).** `drive.py --route story` (Continue → dados 1 →
    missão → lutador solo) chega à tela "Solo Mission: No Partner - Please select Confirm" e fica nela (3 min de ✕).
    Estado no congelamento: thread 4 da CRI (entrada 0x2de7f0) em RUN com **prioridade 1**, presa num laço em
    0x2fa2a0 que percorre uma lista chamando um método virtual (`jalr vtbl[0x18]`, ra 0x2fa35c) — padrão do sistema
    de streams/arquivos da CRI; a thread principal (24) e a thread 3 da CRI (8) ficam READY e famintas; a thread do
    host fica quase sempre ociosa no escalonador. Com `PS2X_SPU2_ADMA=0` também congela. Provável causa comum com o
    ADX que não toca nos menus (a CRI não consegue completar uma operação de stream). Próximo passo: engenharia
    reversa da biblioteca CRI no Ghidra (projeto em `build/ghidra`) a partir de 0x2fa2a0 e do RPC 0x81000201.
    Ghidra (novo `tools/ghidra/DecompileAt.java`, descompila no modo headless; saídas em `build/ghidra/cri_*.c`):
    a thread 4 (0x2de7f0) é o servidor ADXM — laço que suspende a outra thread da CRI (`FUN_002ded18`: Refer + Suspend),
    roda os servidores de ADX/streams (`FUN_002fb290`, `FUN_002fa2a0`: percorre 32 entradas de 0x14 bytes em
    0x69efe8 chamando `vtbl[0x18]`, até 0x80 chunks) e termina em `SleepThread` (acordada pelo VSync). Hipótese de
    acúmulo de wakeups pelo catch-up do host: testada com `PS2X_EE_HOST_PACING=0` e **também congela**, então não é.

Ferramentas novas: `PS2X_PAD_TRACE`, `PS2X_SIF_DMA_LOG`, `PS2X_GIF_SUBMIT_LOG`, e no `PS2X_GS_TRACE` cada desenho
agora começa com `P<path> src=<endereço do pacote>` (path 3 = DMA do GIF; `PS2X_MTVU=0` para o path ficar certo).

## 2026-10-07 (madrugada, trabalho autônomo)

Correção sobre o §20: o usuário mede **~10-15 FPS** na luta, e não os ~30 FPS anotados. O desempenho voltou para a
lista de pendências.

24. **Modo história destravado: o DMA do VIF1 ignorava a direção.** A hipótese da CRI (§23) estava errada:
    - As threads da CRI só rodam ~12 mil ciclos por VSync (`PS2X_TRACE_SCHED=6000:6003:arquivo`, com
      `MSYS_NO_PATHCONV=1` no Git Bash: sem isso o caminho é convertido e o arquivo não é criado). A thread
      principal roda o resto do quadro.
    - O `pc-sample` do `URBAN_HANG_DUMP` mostra a thread principal no laço 0x1298a0. Esse laço espera o byte
      0x35a27a (base 0x3597c0 + 0xaba), "quadro terminado", que só o handler do GS (0x10bc38) liga.
    - Com o novo `PS2X_GS_SIGNAL_LOG=2` (log sem limite de cada SIGNAL aplicado, enfileirado, travado e
      reprocessado, e de cada escrita do EE em CSR/SIGLBLID), o último quadro termina normalmente (6/5, FINISH).
      O quadro seguinte é disparado, mas nenhum SIGNAL chega mais ao GS.
    - Entre esses dois quadros há um FINISH extra e uma escrita `CSR=2`. É a rotina de store-image da libgraph
      (0x3157fc–0x315c7c), que faz o único download de VRAM do jogo (320x224 CT24 de 0x3020, o fundo desfocado
      da seleção de lutador). Ela mascara o FINISH no IMR, manda o pacote por VIF1, espera o FINISH, liga
      `VIF1_STAT.FDR` e `BUSDIR` e lê os 215.040 bytes por **DMA do VIF1 com CHCR=0x100 (DIR=0: VIF1 → memória)**.
    - O runtime ignorava o bit DIR. Tratava o buffer de destino (0x103bc00) como códigos VIF e o enviava ao VIF1.
      O lixo deixava o VIF1 num estado que engolia as chains de todos os quadros seguintes.
    - Correção (`ps2_memory.cpp`, no início de DMA): VIF1 com DIR=0 em modo normal copia `QWC` qwords da FIFO de
      download (a mesma do §10, reabastecida pelo GS) para a memória, avança MADR, zera QWC e completa o canal 1.
      `PS2X_VIF1_DOWNLOAD=0` desliga junto com a FIFO. `PS2X_GS_DOWNLOAD_LOG=1` mostra
      `VIF1 DMA GS->MEM ... 215040 of 215040 bytes`.
    - Resultado: `drive.py --route story` passa pela seleção de lutador e pelo diálogo "Proceed with these
      settings?" (nova referência `story_proceed_dialog`, sobre o fundo desfocado correto). Depois do
      "Now Loading" roda a cutscene da missão com legendas.

25. **Música e vozes (ADX) voltaram: o heap terminava cedo demais.** Cadeia completa:
    - O driver CRI do IOP (`CRI_ADXI.IRX`, base 0x54100) só toca por **ADMA** ("PSM"). O EE decodifica o ADX
      (ADXRT) e manda PCM por SIF para o "ioprna" do IOP. Pelo buffer de comandos DTX, o EE liga o PSM (comando
      0x10 → 0x5566c → 0x59b68: `sceSdBlockTrans(WRITE|LOOP)` + BVOL=0x7fff) e desliga (comando 0x11 → 0x59d0c:
      STOP, zera a área de entrada do core com 5 `sceSdVoiceTrans`, BVOL=0). O laço por VBlank (0x593e8) usa
      `sceSdBlockTransStatus >> 24` (metade atual) para encher o anel.
    - O ADMA parava aos 1,2 s porque o EE mandava o 0x11 e nunca o 0x10. Quem decide é o gerenciador de som do
      jogo (`ch_snd.cpp`, 0x2889f8/0x288ac0): ele pausa o PSM enquanto um banco do nuSound sobe (`ss00`, `at00`)
      e só religa quando a carga chega ao estado 3. As duas cargas falhavam com "nuSound: Error: Bank Push
      Failed! BdAddr in EE Memory is NULL": o buffer do banco vem do `malloc` da libc do jogo (0x140bf8 →
      0x144780 → 0x3295e8), e o `malloc` devolvia NULL.
    - Causa: o crt0 do Urban faz `SetupThread(stack=0x1FBFF00, size=0x40000)` e `SetupHeap(0xAC0A10, -1)`. No
      kernel do PS2, o heap com tamanho -1 vai até a base da pilha (0x1FBFF00). O runtime fixava o fim em
      0x1F00000. Os blocos grandes do boot (~19 MB) terminam em ~0x1EEB280, e os 213 KB do banco não cabiam.
    - Correção (`System.cpp`, `ps2_runtime.cpp`): o SetupThread guarda a base da pilha principal. O SetupHeap
      com tamanho -1/0 usa essa base como limite (`PS2X_HEAP_TO_STACK=0` volta ao 0x1F00000). O
      `clampGuestHeapLimit` aceita limites explícitos até 32 MB (sem limite continua 0x1F00000).
    - Resultado: os dois bancos sobem (SPU2 RAM com 165 mil meias-palavras já no boot) e o PSM é religado. O
      ADMA roda contínuo (~470 blocos a cada 5 s), com música no título e nos menus (`build/autotest/
      title_music.wav`), vozes/música na cutscene do modo história e música contínua na luta.
    - Falsas pistas eliminadas no caminho (as correções ficaram, por serem mais fiéis ao hardware):
      - Progresso do DMA do SPU2 no IOP (`iop_memory.cpp`): CHCR.START fica ligado até o fim e MADR/BCR avançam
        bloco a bloco (`PS2X_SPU2_DMA_PROGRESS=0` desliga).
      - `PS2X_IOP_HLE_LIBSD=0`: entrega o sid 0x80000701 (sceSdRemote) ao SDRDRV.IRX nativo em vez do HLE. Não
        mudou nada aqui e **não** é o padrão.
    - Ferramentas novas desta etapa:
      - `PS2X_SPU2_DMA_LOG=<n>` (inícios de DMA do SPU2 e escritas no ADMAS, com pc/ra do IOP).
      - `PS2X_SPU2_STACK_LOG=<n>` (pilha do IOP nesses pontos).
      - `PS2X_IOP_TRACE_PC=a,b,...` (registradores sempre que o IOP passa por esses pcs).
      - `PS2X_CD_READ_LOG=1` (cada `sceCdRead`).
      - `PS2X_EE_SIFDMA_LOG=1` (cada descritor de `sceSifSetDma` do EE).
      - `PS2X_GS_SIGNAL_LOG=2`.
      - `tools/iso_extract.py` (lista/extrai arquivos da ISO).
      - `tools/irx_info.py` (imports de um IRX e disassembly com as relocações aplicadas). Para o Ghidra:
        `python -c "from irx_info import load"` gera a imagem relocada e o projeto `build/ghidra_iop` (BinaryLoader,
        MIPS:LE:32, base 0x54100) descompila o driver CRI.
    - Driver de testes:
      - Direções com toques de 0,2 s: 0,4 s ativa a repetição automática do menu, agora que o jogo roda perto da
        velocidade real.
      - Só age em telas com casamento forte (diff < 6).
      - Novas referências `challenge_proceed_dialog`/`story_proceed_dialog`.
      - Volta com triângulo se cair no Continue.
    - Observação: o áudio gerado fica abaixo do tempo real na luta (67 s de áudio em ~114 s de jogo), porque o
      IOP/SPU2 acompanha o ritmo do EE. Vai junto com a pendência de desempenho.

26. **Desempenho na luta: medição, profiler e VU0 compilado.**
    - Medição:
      - `PS2X_FRAME_TIMES=<csv>` + `PS2X_FRAME_HASH=1` e o novo `tools/autotest/frame_stats.py <csv> --from <s>`
        dão fields/s (velocidade da emulação, 60 = real) e o FPS visível (imagens que mudam).
      - Antes: **26,6 fields/s e ~12,8 FPS visíveis** na luta, o que confirma os 10-15 FPS do usuário.
    - Profiler novo: `URBAN_NATIVE_PROFILE=<início>:<duração>[:<ms>]` (`src/urban/native_profile.cpp`).
      Amostra as threads nomeadas (GameThread, VUThread, GIFThread, GSThread) com pilha e imprime o tempo próprio
      e inclusivo por função, recompiladas (`sub_XXXXXXXX`) e do runtime.
    - Primeiro perfil da GameThread na luta:
      - 33% esperando em `asyncvif::sync`: cada leitura/escrita de registrador privilegiado do GS ou do VIF espera o
        VU1/GIF assíncrono esvaziar. O handler de SIGNAL 0x10bc38 roda muitas vezes por quadro.
      - 21% no VU0 interpretado.
      - 18% no IOP.
      - O resto é código do jogo.
      - O VUThread fica ~80% ocioso: EE e VU1 andam praticamente em série.
    - **VU0 AOT** (`ps2_vu1_core.cpp`, `ps2_memory.cpp`, `ps2_runtime.cpp`):
      - A memória do VU0 (4 KB) passou a ter 16 KB por trás, e o `vcallms` roda com os tamanhos de 16 KB. Assim a
        imagem do VU0 passa pelo mesmo caminho compilado do VU1.
      - A imagem é capturada com `PS2X_VU0_CAPTURE=<dir>`, recompilada pelo `vu1_recomp` sem mudança e registrada
        pelo hash.
      - A imagem do jogo é `e2a875f42172c34a` (174 pares, 2 blocos), junto com a do VU1 em `generated/vu1`.
      - Bug achado na verificação: o cache de registradores normalizados (`vu1NormShadow`) era único e só era
        invalidado para o VU1. O VU0 recarrega os registradores do contexto do EE a cada chamada e reaproveitava o
        cache velho. Agora existe um `vu0NormShadow` próprio, invalidado a cada `reset`.
      - `PS2X_VU0_VERIFY=1` (compilado contra interpretador, com estado de verificação `thread_local`): **786.432
        execuções, 0 divergências** até a luta.
      - `PS2X_VU0_RECOMP=0` desliga e `PS2X_VU0_BACKING=0` volta aos tamanhos de 4 KB.
      - Resultado: VU0 de 21% para 4,5% da GameThread; **34,1 fields/s** (+28%).
    - Leituras de CSR/SIGLBLID/IMR do GS não esperam mais os workers (`PS2X_GS_READ_SYNC=1` volta). Sem ganho
      sozinho: a espera passou para a escrita do CSR no mesmo handler.
    - Escritas em CSR/SIGLBLID/IMR sem sync (`PS2X_GS_WRITE_SYNC=0`, com replay do stall exclusivo entre threads)
      **travam o boot numa tela azul**, então ficou desligado por padrão. A próxima tentativa deve entender o que
      o boot espera dessa ordem.
    - Downloads de VRAM:
      - Na luta há centenas de leituras de 8x8 pixels por segundo (provável teste de oclusão para lens flare, em
        0x3155f8). Elas custavam ~10% por pedirem os dados ao worker do GS em pedaços de 256 bytes.
      - Agora o DMA GS→memória pega tudo numa chamada, e a FIFO do LQ é reabastecida em blocos de 64 KB, só quando
        está quase vazia (o FQC continua ≤ 16).
      - O custo restante é a sincronização com o GS, inerente à leitura.
    - Estado atual na luta: ~32-34 fields/s, ~13-14 FPS visíveis. Próximos alvos:
      1. As esperas do `asyncvif::sync` (~30%: escrita do CSR no handler de SIGNAL e registradores do VIF).
      2. O IOP (~20%). `PS2X_IOP_PC_HIST=<s>` (histograma de pc do IOP) mostra o custo espalhado, sem laço quente:
         ~4 milhões de instruções por segundo, com sobrecarga alta por instrução (leitura da instrução, checagem de
         stub de import, lookups em hash). O ganho grande seria rodar o IOP numa thread própria.
    - Regressão final da noite: o modo história chega à cutscene e a luta funciona com áudio.

27. **VU1/VIF1/GIF síncronos por padrão: +35% na luta.**
    - Com `PS2X_MTVU=0`, a luta foi de 34 para **46,2 fields/s e ~23 FPS visíveis** (antes ~14), com imagem
      correta.
    - Motivo: o EE esperava os workers assíncronos em cada acesso a registrador de GS/VIF (§26). Os dois andavam em
      série e o modo assíncrono só somava o custo da passagem entre threads.
    - O `src/main.cpp` agora usa `asyncvif::setDefaultEnabled(false)`; `PS2X_MTVU=1` volta ao modo
      assíncrono. O áudio também fica mais perto do tempo real (131 s de som em ~148 s de jogo).

28. **Travamento intermitente no "Now Loading" da missão do modo história (aberto, reduzido).**
    - Acontece com VU1 síncrono **e** assíncrono (2 de 3 no assíncrono). Apareceu depois que o ADX passou a tocar
      (§25).
    - Na rodada travada, o stream ADX da missão (LBN 981411…) lê só o primeiro bloco (25 + 134 setores) e para. Na
      rodada boa continua (981570, 981704…). O PSM é parado/religado igual nas duas (bancos de som carregando), o
      ADMA roda, mas a saída fica em silêncio: o EE não manda PCM decodificado.
    - Achado e corrigido: o STOP da libsd (`sceSdBlockTrans` modo 2) limpa o START do CHCR do DMA do SPU2, e o
      emulador do IOP mantinha a interrupção de conclusão já agendada. O handler da libsd rearmava a transferência
      parada, e com o start seguinte rodavam duas cadeias de ADMA (938 blocos em 5 s, o dobro do normal, visto no
      `PS2X_SPU2_LOG`).
    - Agora uma escrita no CHCR do SPU2 sem START cancela a conclusão pendente (`IopMemory::kDmaCancelDelay`;
      `PS2X_SPU2_DMA_CANCEL=0` desliga). Resultado: 5 de 6 rodadas passam (antes ~1 de 2).
    - O caso restante ainda trava. Próximo passo: comparar o estado do ADXT/SJ do EE numa rodada boa e numa
      travada (dump de RAM no "Now Loading").
    - **Correção posterior:** o "938 blocos em 5 s" era artefato do próprio `PS2X_SPU2_LOG`. A janela de 5 s era
      testada sobre um `m_cycle` já avançado pelo lote inteiro, então às vezes pulava uma janela e a seguinte somava
      duas (por isso aparecia 938 seguido de 0). O espaçamento real dos starts de ADMA (`PS2X_SPU2_DMA_LOG`) não
      tem nenhum intervalo curto. A janela agora usa um contador de amostras.
    - O cancelamento da conclusão de DMA no STOP continua correto em relação ao hardware e fica. A melhora de 5/6
      pode ter sido acaso: a causa do travamento restante continua aberta.
    - Num dump de RAM do estado travado (`URBAN_DUMP_RAM`), a tabela de streams da CRI (0x69efe8, 16 entradas
      ativas), o PSM (0x696948 = 1) e o contador de cargas de banco (0xa5e5c8 = 0) estão normais.
    - Confirmação: com `PS2X_HEAP_TO_STACK=0` (ADX não toca), 4 de 4 rodadas passam. O travamento é do streaming
      ADX.
    - Onde para:
      - Os handles do ADXT ficam em 0x68a660 (16 × 0xa8; byte 0 = em uso, byte 1 = estado: 1 DECINFO, 2 PREP,
        3 tocando). O servidor por handle é 0x2e17c8, chamado por 0x2e1b00.
      - No dump travado, o handle 0 (stream da missão) está em **PREP (2)** e seu decodificador (0x67e960) está no
        estado 0.
      - O PREP (0x2e1718 → 0x2e1158) espera o cabeçalho ADX (`0x8001`, chunk > 3 bytes) no SJ de entrada (handle+8),
        que o ADXF enche a partir do CD.
      - Como o CD só leu o primeiro bloco do stream, a suspeita passa a ser o caminho ADXF/`sceCdRead`/`sceCdSync`
        (HLE síncrono, o que pode expor uma corrida na máquina de estados do ADXF) e não o IOP.
    - Testado sem efeito no travamento (as mudanças ficaram por serem mais fiéis ao hardware):
      - `PS2X_IOP_RPC_SETTLE` (`iop_emulator.cpp`): depois de um RPC do EE, as threads do IOP mais urgentes que a
        thread do servidor rodam antes da resposta, como na preempção do IOP real. É a ideia do port do GoW
        (`IopKernel::bestReadyPriority`). Resultado: 3 de 6 rodadas ainda travam, então ficou **desligado** por padrão (`PS2X_IOP_RPC_SETTLE=1` liga).
      - O cancelamento do DMA no STOP, descrito acima.

29. **Reverb do SPU2** (`iop_spu2.cpp`, `reverbFrame`), pela fórmula do psx-spx, usando como referência o
    `port/audio/spu2.c` do ico-pc (MIT):
    - A entrada "wet" agora é montada como no hardware: máscaras VMIXEL/VMIXER (0x18C/0x194) das vozes, bits wet do
      MMIX para vozes, ADMA e entrada externa.
    - Essa entrada é decimada para 24 kHz (FIR half-band de 39 taps) e passa por um passo de reverb a cada 2
      quadros sobre a área ESA..EEA da RAM do SPU2: reflexões same/diff, 4 combs e 2 all-pass. A escrita na área de
      trabalho depende do ATTR bit 7.
    - A saída volta a 48 kHz com o mesmo filtro e entra na mixagem com EVOL.
    - O Urban usa reverb nos dois cores (ATTR 0xc080/0xc081, MMIX com bits wet). Na luta o resumo mostra
      `reverb!=0` em praticamente todas as amostras, sem clipping, e o FPS não mudou (46,4 fields/s).
    - Amostra: `build/autotest/fight_reverb.wav`. `PS2X_SPU2_REVERB=0` desliga.
    - Ainda faltam, em relação ao ico-pc: ruído, PMON e IRQ do SPU2 (IRQA).

30. **IOP: conclusões de DMA sem varredura por instrução.** O `servicePendingDmaInterrupts` percorria o mapa de DMAs
    pendentes a cada instrução do IOP, e com o ADMA contínuo sempre havia uma pendente. Agora há o prazo mais
    próximo (`nextDmaDue`) e só se varre quando ele vence. Na luta: 44-46 fields/s, ~24 FPS visíveis.
    - O `PS2X_SPU2_LOG` e outros logs mudam o timing e escondem o problema. Use `drive.py --route story` repetido
      sem logs.

## 2026-10-07 (tarde) — Lentidão do boot e perfil da luta

31. **Boot lento até os menus: pré-carregamento de shaders.** O usuário via lag do logo da Namco até os menus,
    também na segunda abertura. Com `PS2X_SCHED_STATS=1` (ciclos do EE por segundo ÷ 294,912 MHz):
    - O boot rodava a 40-80% por ~2 minutos. A GameThread passava ~40% parada em `GSThreadedBackend::PublishLocked`,
      esperando a GSThread, que estava presa dentro do driver.
    - A causa: o `GSShaderCompiler` recarregava os 3.401 shaders do `hw_keys.txt` (~15 ms cada no
      `glProgramBinary` da NVIDIA), e isso trava o contexto do renderizador no driver.
    - A ordem da fila não era o problema: priorizar os shaders pedidos não mudou nada.
    - Sem o pré-carregamento, o boot e os menus rodam a 100% e a luta chega em 58 s em vez de 75 s. A luta continua
      igual (~70-78%) e sem erros visuais.
    - Agora o pré-carregamento vem desligado (`PS2X_GS_HW_PRELOAD=1` liga). Ficaram as melhorias do worker: lotes
      de 8 jobs, um `glFinish` por lote e promoção para a frente da fila de um shader que o renderizador pede.
    - Também foi criado `PS2X_AUDIO_OFF=1` no SPU2, que pula a mixagem (o ADMA continua sendo consumido e as vozes
      terminam no key-on). Com ele o desempenho foi o mesmo, então o áudio não é gargalo.
32. **Perfil da luta (`URBAN_NATIVE_PROFILE`, 30 s).**
    - A GSThread fica ~94% ociosa: OpenGL e GPU não são o gargalo, e Vulkan não ajudaria.
    - A GameThread fica 100% ocupada: VU1 ~30% inclusivo (`fastBlock` do AOT), IOP interpretado ~21%, VIF1/GIF,
      `readIORegister` ~10%, SPU2 ~4%.
    - O caminho para mais FPS é tirar trabalho dessa thread: IOP em thread própria ou mais barato, VU0 AOT, e
      menos sincronização com o VIF/GS.
    - Ao fechar, o `PS2X_GS_GPU_PROF` imprime tempos lidos de forma síncrona a cada 4 quadros, o que infla os
      números. Não use como medida de gargalo.
    - O `controles.ini` ao lado do `urban.exe` define variáveis de ambiente (`NOME=valor`) ao abrir; o que já
      estiver definido no ambiente tem prioridade.
33. **IOP mais barato na thread do jogo** (custo do IOP na luta: 23,6% → 16,3% da GameThread). O IOP não estava
    girando em loop de espera: ele executa ~3,7 M instruções/s de trabalho real numa única thread (t75, 0x57000-0x5A000,
    provavelmente o decodificador ADX da CRI). O custo era o overhead por instrução:
    - `IopMemory::read8/16/32` com caminho de RAM inline no header (o resto vai para `read*Slow`), e
      `physicalAddress` constexpr inline.
    - `imports.decode` (procura da tabela de imports) só roda quando a instrução no pc é `jr ra`. Isso tirou 2,3%.
    - `schedulePendingDma` sai cedo com `hasDmaStart()` inline.
    - `IopKernel::beginNextReady` acorda as threads e escolhe a próxima numa única passada pelo mapa.
    - `EeScheduler::accountCycles` repassa os ciclos ao IOP a cada 512 ciclos do EE em vez de a cada checkpoint (o
      IOP já roda em lotes de 8192).
    - Velocidade média na luta (`PS2X_SCHED_STATS`, 35 s): 74% → 80-83%. O ruído entre lutas é de ±3%, porque a IA
      varia.
    - Sobra: interpretação em si ~5%, SPU2 ~4,5% (a maior parte é `stepAddress` de vozes desligadas, que o hardware
      também avança), `accountCycles` ~3,4% de custo próprio.
    - Modo história: o "Now Loading" do §28 continua travando em 2 de 4 rodadas, a mesma taxa de antes.
    - O `URBAN_NATIVE_PROFILE` não aloca mais com a thread suspensa (antes podia travar o jogo segurando a trava do
      heap). Ele desenrola com `RtlVirtualUnwind`, e `URBAN_NATIVE_PROFILE_STACKS=<arquivo>` grava pilhas "folded".
34. **"Now Loading" do modo história (§28): a causa real.** O que estava anotado no §28 (handle parado em PREP, CD)
    eram sintomas. Cadeia completa:
    - O jogo inicia o stream da cena pausado e só despausa quando `ADXT_GetStat(0x681c48) == 3` (PLAYING). Isso
      acontece em `FUN_002344a0` → `FUN_00282068`, e o despausar em `FUN_002820b8`.
    - `adxt_stat_prep` (0x2eb608) só passa a PLAYING quando o PSM (o espelho, no EE, do buffer de áudio do IOP)
      tem dados. O RNA (0x698d68) estava com uma limpeza pendente (`+0x5a/+0x5b = 1`): o servidor 0x2f6e30 espera
      que o SJ do PSM de cada canal tenha os 0x4000 bytes livres antes de religar as transferências.
    - Num dos canais faltavam 0x800 bytes: um bloco entregue ao IOP para tocar nunca foi devolvido. Os blocos vão
      e voltam por um protocolo de ping-pong sobre DMA do SIF (EE 0x2f0100 ↔ IOP 0x54c24, com números de
      sequência; lista de blocos EE `FUN_002fa2a0`/`FUN_002fa200`, IOP `0x5d10c`/`0x5d060`). Uma vez perdido, o
      bloco trava a próxima troca de música daquele canal para sempre.
    - O auditor `URBAN_PSM_AUDIT=1` (+ `URBAN_PSM_AUDIT_CHUNKS=1`) achou a perda numa partida normal: o IOP
      devolveu e3140 e depois e4140, pulando e3940+0x800.
    - Causa: o driver CRI_ADXI.IRX (IRX id 9, base 0x54100) protege as listas encadeadas com
      `CpuSuspendIntr`/`CpuResumeIntr`, e o callback de DMA do SPU2 mexe nas mesmas listas. O emulador do IOP
      atendia as conclusões de DMA **depois de cada instrução** e os timers/VBlank entre fatias, sem olhar o
      `I_CTRL`. Também trocava de thread no fim da fatia mesmo dentro da seção crítica.
    - Correção (`iop_emulator.cpp`): com `I_CTRL = 0`, DMA, callbacks agendados, timers e VBlank ficam pendentes.
      Uma thread cuja fatia acaba dentro da seção crítica continua rodando, a menos que bloqueie.
      `PS2X_IOP_INTR_MASK=0` volta ao comportamento antigo.
    - Resultado: 8 de 8 partidas normais do modo história chegaram à luta. Antes travava ~1 a cada 2, então 8
      seguidas por acaso teriam ~0,4% de chance. A velocidade na luta ficou igual, dentro do ruído.
    - Áudio pipocando (pendente): na luta o SPU2 produz ~37 mil amostras/s contra 48 mil pedidas (jogo a ~80%),
      ou seja, ~13 underruns/s. Cada underrun faz um fade para silêncio e espera juntar 43 ms (`ps2_audio.cpp`).
      Precisa de time-stretch ou reamostragem adaptativa.
35. **Laço de espera do GS: `CSR.FIFO` sempre "não vazio".**
    - `PS2X_IO_READ_HIST=1` mostrou que 99,5% das leituras de I/O na luta eram do `VIF1_STAT`, cerca de 1 milhão
      por segundo.
    - Origem: `0x122ca0`, um laço do jogo que espera `(CSR & 0xC000) == 0x4000` (FIFO vazio) e
      `(VIF1_STAT & 7) == 0`. Sem isso, ele gira até o campo de vídeo mudar e então chama `sceGsSyncPath`.
    - O runtime devolvia o `CSR` com os bits de FIFO em 00. Agora a leitura reporta "vazio" (01), como o PCSX2. Os
      GIF chegam ao front-end do GS de forma síncrona. `PS2X_GS_CSR_FIFO=0` desliga.
    - Os ciclos do EE/s não mudaram, porque o laço queimava ciclos emulados. Por isso a medida agora é de quadros
      do jogo: `URBAN_CALL_TRACE=128dc0` (uma chamada por quadro do laço principal; `sceGsSyncV` só roda no boot).
    - Resultado na luta: **12,4 → 22 quadros/s** (duas rodadas de cada lado, idênticas). Agora o jogo fica preso à
      velocidade da emulação: a ~75%, ~45 campos/s, o teto é ~22,5 quadros/s para um jogo de 30 FPS.
36. **IOP numa thread própria** (`iop_subsystem.cpp`; ligado por padrão, `PS2X_IOP_THREAD=0` desliga).
    - Os lotes de 8192 ciclos do EE vão para uma thread de trabalho enquanto o EE segue.
    - Toda outra entrada do `IopSubsystem` (RPC, DMA do SIF, memória do IOP, módulos, save state) espera o lote
      terminar (`join`) e então sincroniza o IOP como antes.
    - Um `DeferringHost` enfileira os efeitos que caem no lado do EE durante um lote (escritas na RAM do EE pela DMA
      do SIF e pacotes de comando SIF, que rodam handlers do EE). A fila é aplicada na thread do EE no `join`, então
      o EE os vê na mesma granularidade de lote de antes. Áudio, controle, arquivos e leituras passam direto.
    - O IOP pode ficar até 512 lotes atrás (`kMaxAsyncLagEeCycles`, ~14 ms emulados). Com 1 lote, o EE esperava
      11% do tempo nas fases ociosas (pulando tempo, ele gera lotes mais rápido do que o IOP processa).
    - `PS2X_IOP_THREAD_STATS=1` imprime lotes/s, ocupação da thread e espera do EE.
    - Luta: **22,0 → 25,2 quadros/s**. Thread do IOP ~19% ocupada, espera do EE ~3%.
    - Modo história: 3 de 3 rodadas passaram, e 2 minutos de `--mash` sem erros.
37. **Contador de FPS (F3) e medição de stutter.**
    - O overlay mostra quadros do jogo (o port chama `ps2_host_display::noteGameFrame()` na entrada de 0x128dc0),
      apresentações por segundo, velocidade (campos/s ÷ 59,94) e o pior intervalo entre quadros do jogo no último
      segundo. `PS2X_SHOW_FPS=1` abre com ele ligado.
    - O usuário relatou stutter depois do §36. Medido com `PS2X_FRAME_TIMES` + `PS2X_FRAME_HASH=1`
      (`presentstats`, intervalo entre imagens novas na tela, luta):
      - IOP no EE: 33 ms ×143, 50 ×254, 83 ×64, 100 ×30;
      - IOP em thread: 33 ×327, 50 ×178, 83 ×50, 100 ×2.
    - A apresentação (`interval_ms`) fica em 16,7 ms nos dois modos. Os saltos de 83-117 ms entre imagens novas já
      existiam antes da thread; a thread os reduziu. Causa provável: ~25 quadros/s de jogo num monitor de 60 Hz, mais
      quadros cuja cópia final chega fora de ritmo. Fica pendente investigar o ritmo da cópia de exibição.
38. **Ritmo de apresentação: vsync real + uma apresentação por campo emulado** (`ps2_runtime.cpp`).
    - O usuário via o jogo a 30 FPS estáveis "parecendo 20-24".
    - Duas causas:
      - A janela usava o limitador de 60 FPS por software do raylib (`SetTargetFPS`), fora de fase com o refresh
        do monitor.
      - Os 59,94 Hz da emulação e os 60 Hz do laço ficavam quase na mesma fase, então ~1 ms de tremida repetia um
        campo e pulava o seguinte (`fields/present` 0 e 2).
    - Agora `FLAG_VSYNC_HINT` está ligado (`PS2X_VSYNC=0` volta ao limitador). O laço de apresentação espera o tick
      de vsync emulado avançar, por até 50 ms, antes de enviar o quadro (`PS2X_PRESENT_SYNC=0` desliga).
    - Imagens novas na luta, em refreshes por imagem: antes 1×47 2×327 3×178 4×45 5+×59; agora 2×694 3×76 5×1.
      Menus: eventos 0/2 de 117 para 32 em 35 s. O FPS do jogo não mudou.
    - Análise: `PS2X_FRAME_TIMES=<csv>` + `PS2X_FRAME_HASH=1` e o script `pacing.py`.
    - Achado do usuário na seleção de personagens do multiplayer: 2 personagens na tela = 100%, 3 = 77%, 4 = 63%,
      e volta na hora ao tirá-los. O custo escala com os personagens (skinning no VU1), então o próximo alvo é o
      gerador do VU1.
39. **Progressive scan = 60 FPS** (`URBAN_PROGRESSIVE=1`, `main.cpp`).
    - O usuário joga no PCSX2 a 60 FPS com o patch embutido "Force Progressive Scan" (`patches.zip`, ativado em
      `gamesettings/SLUS-21209_BDD9BAAD.ini`).
    - O patch troca `jal 0x315c88` (0x1372e0) e `jal 0x1370c8` (0x1372e8) por `jal 0x1373b0`, o caminho
      progressive do próprio jogo: `sceGsResetGraph(0, 0, 0x50 = 480p, 1)`.
    - Aqui as duas funções são envolvidas e desviadas para 0x1373b0 quando o endereço de retorno é exatamente o do
      patch.
    - Resultado: menus a 60 quadros/s com velocidade 100% (antes 30). Luta a ~44 quadros/s (antes 25), mas a
      emulação cai para ~70% (câmera lenta), porque cada quadro custa o mesmo e agora são o dobro.
    - A pedido do usuário, ficou **ligado por padrão** (`URBAN_PROGRESSIVE=0` volta ao entrelaçado). O crédito do
      patch (Michael) está no README.
    - `drive.py`: as referências de tela foram capturadas em modo entrelaçado e em progressive ficam com diferença
      ~7, então o limite para agir subiu de 6 para 8. A tela de "Now Loading" em progressive não é reconhecida:
      na história, o driver chega à luta (tela FAILED no fim, sem input) mas reporta "goal NOT reached". Faltam
      novas referências.
40. **VU1: o que foi medido e tentado** (luta em progressive, referência 31,4 quadros/s).
    - Tempo do VU1: dentro dos segmentos estáticos (`seg_`), espalhado pelas FMACs.
    - Experimento (incorreto de propósito) sem checagem de faixa nem flags nas FMACs: 34,7 (+11%). Esse é o teto
      de otimizar as flags. O microcódigo lê flags MAC (`FMAND` em vários programas), então não dá para eliminá-las
      no programa todo, e o gerador já agrega as flags que ninguém observa (`fmacV`). Não implementado.
    - `vu1_recomp`: segmentos que voltam para si mesmos (38) continuam no corpo sem recarregar os registradores
      em cache, refazendo só as checagens de entrada (`VU1_RECOMP_NO_SELF_LOOPS=1` desliga). `PS2X_VU1_VERIFY` +
      `PS2X_VU0_VERIFY`: 573 mil execuções, 0 divergências. Ganho: nenhum (31,5). Ficou, por ser neutro e
      verificado.
    - Atenção: regenerar a partir de `build/vu1_capture` apaga o VU0 AOT (imagem `e2a875f42172c34a`) do registro e
      derruba o FPS (23,5). A entrada certa é `build/vu_capture_all`, que tem as duas imagens. Com
      `VU1_RECOMP_NO_SELF_LOOPS=1`, ela reproduz o código antigo byte a byte.
    - MTVU (`PS2X_MTVU=1`): só +2% (32,0), porque o EE passa 60% do tempo em `asyncvif::sync` chamado pelas escritas
      em CSR/IMR/SIGLBLID (~2.300/s, o aperto de mão de SIGNAL; `PS2X_GS_PRIV_WRITE_HIST=1` conta).
    - Para tirar essa espera, a liberação do stall de SIGNAL foi para a thread do GIF
      (`asyncvif::postGifStallRelease`), em vez de reprocessar os pacotes adiados na thread do EE, ao mesmo tempo
      que a do GIF. Mesmo assim, `PS2X_MTVU=1` + `PS2X_GS_WRITE_SYNC=0` ainda trava ao carregar o save. Falta
      investigar a entrega de SIGNAL/FINISH entre as threads. O padrão continua MTVU desligado.
41. **Captura de desempenho no jogo: F6 / F7** (`src/urban/native_profile.cpp`).
    - F6 inicia uma amostragem de 1 ms de todas as threads nomeadas (GameThread, GSThread, IOPThread...), e F6 de
      novo para. F7 grava uma marca.
    - Saída: `perfil_<n>.txt` ao lado do `urban.exe`, com:
      - uma tabela por segundo com o % da GameThread em VU1, VIF1 (que inclui o VU1), VU0, GS, envio ao GS, IOP,
        espera da thread do IOP, EE ocioso e folga (`processDueDeadlines`: esperando o tempo real), mais a linha de
        FPS daquele segundo;
      - os pontos quentes de cada thread.
    - As pilhas completas vão para `perfil_<n>_pilhas.txt` (analisar com `callers.py`).
    - Feito para o usuário capturar a seleção de personagens do multiplayer, indo de 2 a 4 lutadores na tela.
    - O runtime ganhou `ps2_host_display::setOverlayNote` (faixa no topo da tela) e `fpsLine` (a linha do contador de
      FPS). A thread do IOP agora se chama "IOPThread".
42. **Captura do usuário: seleção do multiplayer, de 1 a 4 lutadores** (`perfil_2.txt`, progressive, com a captura
    ligada):

    | lutadores | VU1 | GS | velocidade | quadros/s |
    |---|---|---|---|---|
    | 1 | ~27% | ~17% | ~90% | ~54 |
    | 2 | ~45% | ~13% | ~70% | ~42 |
    | 3 | ~62% | ~15% | ~46% | ~28 |
    | 4 | ~66% | ~14% | ~40% | ~24 |

    - Todo o aumento vem do VU1: o bloco 9 (laço de vértices) e o XGKICK (~9,5%).
    - Achados fora do VU1:
      - Cada DMA em cadeia alocava um vetor do tamanho da cadeia e o liberava depois (`RtlFreeHeap` →
        `ZwFreeVirtualMemory`, ~4%). Agora há um pool de buffers (`takeChainBuffer` / `recycleChainBuffer` em
        `ps2_memory.cpp`). Luta: 32,0 → 33,7 quadros/s (+5%).
      - `sub_00292670` (27 KB) é um descompressor de bytes do jogo: ~12% na seleção (dados de cada lutador que
        entra), ~4% na luta. Candidato a HLE.
      - As cópias (`memmove` ao enfileirar a cadeia de DMA e os pacotes para a thread de vídeo, ~5%) continuam.
    - Ferramentas novas:
      - `--save-at <tela>` / `--load-state` no `drive.py`, e `!save`/`!load` no `PS2X_PAD_FIFO`.
      - `URBAN_QUICK_STATE=<arquivo>`, um slot separado do F5 do jogador. Um state salvo na seleção de missão
        depois da perda trava 100% das vezes: serve de reprodução rápida.
      - `URBAN_CALL_PROFILE_TOP`.
      - `PS2X_EE_SIFDMA_DUMP=<endereço do IOP>`. Cuidado: ele muda o timing, e com ele o bug some.

43. **MTVU (VU1 + GS fora da thread do jogo) funcionando, mas sem ganho de FPS** (opcional:
    `PS2X_MTVU=1` + `PS2X_GS_WRITE_SYNC=0`).
    - Três travamentos corrigidos:
      - **Tela do save (≈50% das execuções).** O jogo ficava preso em `0x1298a0`, esperando o byte
        `0x35a27a`, que o handler do GS (`0x10bc38`) marca quando `SIGLBLID` chega a 3. O handler faz
        ler-modificar-escrever no `SIGLBLID`, e um SIGNAL aplicado pela thread do GIF nesse meio se perdia. Agora
        SIGNAL/FINISH esperam o handler terminar, como esperam um `CSR.SIGNAL` pendente:
        - `ps2xGsSetHandlerBusy` (`ps2_memory.cpp`);
        - o hook `gs_handler_exclusive` no `main.cpp` (só com o MTVU ativo);
        - `ps2xGsSignalBlocked` no `gs_frontend.cpp`.
        Resultado: 4/4 execuções chegam à luta.
      - **Download de VRAM** (`VIF1_STAT.FDR` / DMA do VIF1 com `DIR=0`): agora drena o pipeline antes. O pacote
        `TRXDIR=1` podia ainda estar na fila da thread do GIF.
      - **Liberação perdida do stall de SIGNAL**: rede de segurança na thread do GIF, que acorda a cada 2 ms.
    - Duas correções de latência no scheduler (`EeScheduler.cpp`):
      - **Despertar pelo GS.** O GIF agora acorda a espera de pacing quando levanta SIGNAL/FINISH
        (`ps2xWakeEeForGsInterrupt`).
      - **Prazo adiado.** Com uma interrupção na fila ou rodando, o prazo vencido em ciclos (mas não em tempo
        real) sai dos checkpoints até o handler voltar (`g_deadlinesDeferredUntil`). Antes, todo checkpoint
        voltava ao scheduler e o handler só rodava depois do vblank. Era ele que disparava o próximo DMA, então o
        quadro perdia a troca. Efeito: 20 → 23,6 quadros/s.
    - Medição na luta do `drive.py` (desafio 1 contra 1, ou seja, 2 lutadores; uma versão anterior desta nota
      dizia 4 por engano):
      - MTVU: ~23 quadros/s com velocidade de jogo 100%.
      - Modo normal: 33,4 quadros/s com velocidade ~57-70%.
      - Menus: 60 no modo normal, 30-35 no MTVU.
      - O modo normal não mudou (luta 33,4; história OK).
    - De onde vem a diferença (trace com thread e caminho GIF de cada evento, analisado com o `mtvutl.py` do
      scratchpad; 37 quadros, 48,7 ms por quadro):
      - SIGNALs: 13 por quadro via PATH3 (DMA do GIF) e 13 via PATH2 (VIF1 DIRECT), nenhum via PATH1 (XGKICK).
        O FINISH (1 por quadro) vem do PATH3.
      - As passagens entre threads custam microssegundos (medianas):
        - SIGNAL aplicado na GIFThread → handler no EE: 8 µs;
        - kick de DMA → job no VU: 7 µs;
        - fim do job → GIFThread: 5 µs.
        Ping-pong isolado com condition variable: 0,7 µs com o parceiro ativo, ~25 µs com ele dormindo.
      - O EE não espera o SIGNAL: `asyncvif::sync` soma 0,02 ms por quadro. O modelo já é o do hardware: a
        GIFThread segura só a própria fila, o EE trata a interrupção no checkpoint e libera ao escrever no CSR.
      - Tempo de um quadro:
        | Estado | ms por quadro |
        |---|---|
        | todos parados, esperando o vblank em tempo real | 20,5 |
        | VU1 trabalhando | 17,1 |
        | só o EE | 6,8 |
        | só o GIF | 3,8 |
        | handler do GS | 0,5 |
      - A causa é o arredondamento para o campo. O jogo dispara o render no vblank. A corrente leva ~22 ms (um
        único job de VU1 tem 11 ms), passa do vblank seguinte, e o quadro ocupa 3 campos (48,7 ms). No modo
        normal o render cabe no mesmo campo em tempo emulado; o que encolhe é a velocidade do jogo (57-70%), não
        o FPS.
      - PCSX2: SIGNAL/FINISH dos PATH2/PATH3 são tratados na thread do EE (`Gif_HandlerAD`, em `Gif_Unit.cpp`),
        no tempo emulado. Só o PATH1 (XGKICK) com MTVU é assíncrono: `Gif_HandlerAD_MTVU` marca flags atômicas
        e o EE as recolhe em `Get_MTVUChanges`, sem stall. O vblank do PCSX2 é por ciclos emulados, e o limitador
        só espera no fim do quadro. Um VU1 lento baixa a velocidade, sem arredondar quadros para o campo.
    - Ferramentas novas:
      - `PS2X_PACING_STATS=1`: sono do pacing por segundo e quem acordou.
      - `PS2X_MTVU_TRACE=<s>`: linha do tempo entre threads (`mtvu_trace.txt`: µs, evento, argumento, thread
        e/v/g; o SIGNAL `J` leva o caminho GIF).

44. **Referência do PCSX2** (v2.7.515, DX12; Emulação: EE Cycle Rate 100%, sem cycle skipping, MTVU ligado,
    cheats ligados). Uso de CPU por thread em % de um núcleo, medido por 12 s com `tools/thread_usage.py`. As
    threads sem nome foram identificadas pela pilha: o GS fica no driver D3D da NVIDIA, e o MTVU em código de JIT.

    | Cena (60 FPS estáveis) | EmuThread (EE+IOP) | GS | MTVU (VU1) |
    |---|---|---|---|
    | Luta 1v1 (como a do `drive.py`) | 31,5% (~5,3 ms/quadro) | 14,0% (~2,3 ms) | 5,8% (~1,0 ms) |
    | Seleção do multiplayer, 4 lutadores | 30,5% (~5,1 ms) | 16,2% (~2,7 ms) | 8,0% (~1,3 ms) |
    | Luta com 8 lutadores (pnach do Coop Tool) | 46% (~7,6 ms) | 26% (~4,3 ms) | 22% (~3,7 ms) |

    - Com 8 lutadores, às vezes cai para ~40 FPS e volta.
    - Nós, nas mesmas cenas:
      - VU1: ~13-17 ms por quadro no 1v1 e ~27 ms na seleção com 4. Ou seja, 13-20x o microVU.
      - Front end do GS: ~4 ms no 1v1.
    - RAM das três cenas (via PINE) em `build/pcsx2_ref/` (`ee_fight1v1.bin`, `ee_select4.bin`,
      `ee_8fighters.bin`). O `.text` só difere do ELF nas 2 palavras do progressive.
    - O pnach de 8 jogadores tem 101 escritas de dados (`patch=1`, 8/16/32 bits, em 0x5B0000-0x5EFFFF, 0x94xxxx e
      0xABxxxx) e nenhuma em código. Dá para suportar só aplicando as escritas a cada quadro, sem recompilar.

45. **Plano 1: gerador de VU1 "estilo microVU", em etapas** (medidas na seleção do multiplayer com 4 lutadores).
    - Bancada: `tools/autotest/bench_state.py`. Ela carrega `bench/states/select4.state`, um F5 do usuário
      nessa tela, e mede quadros/s por 20 s depois de 8 s de espera. O ruído entre rodadas fica em ±0,2.
    - Verificação: `PS2X_VU1_VERIFY=obs` / `PS2X_VU0_VERIFY=obs` compara contra o interpretador só o que é
      observável, sempre bit a bit:
      - memória do VU;
      - pacotes do GS;
      - registradores, com os resultados ainda no pipeline já resolvidos;
      - MAC/status/clip finais (num corte por orçamento, como ficam depois que o pipeline esvazia);
      - ciclos.
      Não compara os tempos de "pronto" nem a fila de registros de flags.
    - O `vu1_recomp` voltou a linkar: ganhou uma tabela de funções do EE vazia. Para rodá-lo, o `build/port/bin` tem
      de estar no PATH (DLLs).
    - Linha de base: **26,7 quadros/s** (velocidade 44%). No perfil: bloco 9 com 52,5%, bloco 8 com 7,2%,
      `sub_00292670` com 10,3%, `memmove` com 5,2%.
    - **(a) Flags só onde podem ser lidas.** Uma análise da imagem inteira (ponto fixo para trás sobre o fluxo
      estático) marca os pares de onde um FMEQ/FMAND/FMOR/FSEQ/FSAND/FSOR/FSSET, ou um JR/JALR, pode ser alcançado.
      Nos demais (1.726 de ~2.150), todas as FMACs do segmento juntam as flags num único registro empurrado pela
      última. Antes, as últimas 4 posições de cada segmento calculavam e empurravam o próprio registro.
      `VU1_RECOMP_NO_DEAD_FLAGS=1` reproduz o código anterior byte a byte.
      - Resultado: **27,8 quadros/s** (+4%, velocidade 45%).
      - `obs`: 0 divergências em ~197 mil execuções de VU1+VU0.
    - Para onde vai o tempo do código de VU depois da (a). Perfil por linha (`URBAN_NATIVE_PROFILE_LINES=5000`),
      classificado pelo `vucost.py` do scratchpad. O código de VU é 55% da GameThread:

      | Tipo de comando gerado | % do tempo de VU |
      |---|---|
      | FMAC (conta + checagem de faixa para exatidão + flags) | 66% |
      | loads da memória do VU + normalização | 16% |
      | gravação dos tempos de "pronto" | 5% |
      | carga dos registradores na entrada do segmento | 4% |
      | checagens de pipeline/orçamento | 3% |
      | stores/XGKICK, FTOI/MIN/MAX/CLIP, ciclos, outros | ~6% |

      A etapa (c) mexe no que mais pesa. As etapas (b) e (d) juntas valem no máximo ~15% do VU.
    - **(c) Contas e loads sem a checagem de exatidão.**
      - `fmacFastV` (em `ps2_vu1_impl.inl`) é usada nas FMACs com flags mortas. Ela faz só a operação SSE mais o
        acúmulo das flags sticky de zero/sinal.
        - Por que basta: os jobs de VU rodam com arredondamento para zero, FTZ e DAZ. Estouro já dá ±FLT_MAX e um
          resultado abaixo de FLT_MIN já dá ±0, que é o mesmo valor do caminho exato.
        - Exceção: um MADD/MSUB cujo produto satura vai para o caminho exato. O produto é truncado em FLT_MAX antes
          de somar o ACC; com ACC = −FLT_MAX dava 0 onde o exato estoura para +FLT_MAX. Era o programa 0x3568,
          MADD seguido de MINI.w 255: dava 0 em vez de 255, achado pela verificação na luta.
      - Imagens que cabem nos 4 KB do VU0 mantêm as FMACs exatas. O programa de skinning do VU0 tem underflow de
        verdade (flag U), e o EE lê o status do VU0.
      - Loads LQ sem `vnorm`: o DAZ cobre operandos desnormalizados.
      - Chaves: `VU1_RECOMP_EXACT_FMAC=1` e `VU1_RECOMP_NORM_LOADS=1` desligam cada parte.
      - O `obs` agora conta à parte as diferenças de status final do VU1 quando nenhuma instrução da imagem lê
        status (FSEQ/FSSET/FSAND/FSOR). O EE não lê o status do VU1, então elas não são observáveis. Também mostra os
        valores de VF e de status que diferem.
      - Verificação, 0 divergências nos três cenários:
        - seleção com 4: ~197 mil execuções;
        - luta 1v1: ~557 mil;
        - modo história: ~573 mil, mais 60 diferenças de status que nada lê.
      - Resultado: **37,8 quadros/s** (velocidade ~62%). Sem a guarda do produto seria 39,1, mas com o erro.
      - Perfil depois: o VU é 40% da GameThread (era 55%). Dentro dele, FMAC 60%, tempos de pronto 10%,
        checagens 8%, loads 5%. Fora do VU: `sub_00292670` 14,7%, `memmove` 7,1%.

    - **Sem acúmulo sticky no VU1.** Como nenhuma instrução da imagem de VU1 lê status (FSEQ/FSSET/FSAND/FSOR),
      as FMACs rápidas usam `fmacFastN`, que é só a operação SSE.
      - A última FMAC de cada segmento continua empurrando MAC/status exatos.
      - Proteções:
        - o gerador mantém o acúmulo em qualquer imagem que tenha leitor de status;
        - o runtime registra (`[vu1-window]`) qualquer acesso do VU0 à janela de registradores do VU1 (0x4000-0x43FF),
          o único caminho de fora para o status do VU1, e o `PS2X_VU1_VERIFY` acusa isso como erro;
        - `VU1_RECOMP_STICKY=1` volta ao comportamento anterior. Com ela, o código de VU1 sai idêntico ao da (c).
      - Resultado: **40,0 quadros/s**. `obs` sem divergências na seleção e na luta, nenhum acesso à janela.

46. **`sub_00292670` é o decodificador de trilhas de animação, não um descompressor de arquivo.**
    - Assinatura: `(saída, animação, osso, quadro)`. Ele amostra a trilha do osso no quadro e grava 16 bytes (um
      quaternion) em `saída`.
    - Na seleção com 4 lutadores: ~5.700 chamadas/s, ~88 por quadro, sempre em pares (quadro N e N+1, para
      interpolar). São 4 animações × 21 ossos × 90 quadros = 13.860 combinações, todas repetidas depois da primeira
      volta da animação ociosa.
    - Dados do osso: `sub_00299130` retorna `*(anim+0x10) + *(*(anim+0x0C) + osso*4)`.
    - Escritas, medidas tirando foto de toda a RAM e do scratchpad em volta de chamadas (`URBAN_ANIM_PROBE`): só os 16
      bytes da saída e o próprio quadro de pilha.
    - Leituras: **não é uma função pura**. Com `URBAN_ANIM_VERIFY=2`, a mesma (animação, osso, quadro) às vezes dá
      outro resultado, e a causa foi achada em etapas:
      - um local do quadro de pilha que alguns caminhos não inicializam;
      - o registrador `$t4`, inclusive a metade alta de 128 bits (a função usa MMI), lido sem ser escrito;
      - depois de pôr os dois na chave, ainda sobra ~1 divergência em 100 mil, de origem não identificada.
    - Pôr a metade alta do `$t4` na chave derruba o acerto para ~40%.
    - Por isso o cache (`src/urban/anim_decoder.cpp`) fica **desligado** (`URBAN_ANIM_CACHE=1` liga). Ligado,
      leva a seleção de 39,8 para 41,1 quadros/s.
    - Custo: ~42 µs por chamada, ~15% da thread do jogo na seleção.

47. **Por que o decodificador era lento: o MSVC não otimizava as funções gigantes.**
    - Medição por chamada (`URBAN_ANIM_STATS=1`):
      - ~1.550 instruções de EE por chamada, contadas num experimento com um contador após cada instrução do
        arquivo gerado (depois restaurado). Num PS2 a 1 instrução/ciclo isso dá ~5,2 µs.
      - O algoritmo é um range coder lendo byte a byte, sem decodificar a trilha desde o início.
      - Os "ciclos" do scheduler não servem para isso: o recompilador só soma 32 a cada desvio para trás.
    - No PC: 26,5 µs por chamada, ~17 ns por instrução.
    - Pelas categorias pedidas:

      | Categoria | Peso no decodificador |
      |---|---|
      | MMI | nenhuma no laço quente |
      | loads/stores | caminho rápido inline, nada no scratchpad; os lambdas `READ*` chamados fora de linha somam 11% |
      | overhead por instrução | a gravação de `ctx->pc` em toda instrução vale 6% (experimento sem elas: 17,9 → 16,8 ns); checkpoints ~1,5% |
      | interpretador / fallback | nenhum (`dispatchGuestBranch` 0,8%) |

    - **A causa real estava no assembly.** O código saía no nível do `/Od`: condições constantes não resolvidas,
      `switch` sobre índice constante e todo temporário gravado numa pilha de ~400 KB.
    - O arquivo (1,27 MB) estava entre os 4 "gigantes" (>900 KB) que o `src/CMakeLists.txt` compila sem
      `/d2OptimizeHugeFunctions`. A exclusão existe porque o `sub_001A7AE8` levava 40+ minutos com a opção. Sem
      ela, o MSVC desiste de otimizar funções enormes (C4883).
    - Correção: `URBAN_HUGE_OPTIMIZED` (lista) devolve a opção a `sub_00292670`, `sub_00299260` e `sub_001A1948`.
      O `sub_001A7AE8` continua de fora. As três compilam em ~12 min, só quando mudam.
    - Resultado:
      - decodificador: **26,5 → 3,9 µs por chamada**, ~2,5 ns por instrução;
      - seleção com 4: **40,0 → 47,5 quadros/s**, velocidade **78%**;
      - no perfil, o decodificador caiu de 14,7% para 2,1% da GameThread.
      - Luta 1v1 e história continuam funcionando (cutscene e missão).
    - Perfil da seleção agora:
      - VU1: blocos 9+8 com 47,7%;
      - `memmove` 8,1%;
      - `accountCycles` + `checkpointDue` 4,2%;
      - locks (SRW/Mtx) ~4%;
      - `vertexKick` + `buildDrawBatch` 3,2%.

48. **Varredura de funções que o MSVC não otimizou. O código de VU1 também estava sem otimização. 60 FPS na
    seleção com 4.**
    - Como achar essas funções: uma função que o MSVC desiste de otimizar (C4883) guarda todo temporário na pilha
      e ganha um quadro de centenas de KB. Quadros acima de 4 KB chamam `__chkstk` com o tamanho em `eax`.
    - `tools/frame_scan.py` lê os objetos COFF do código gerado (9.954 objetos, ~4 min) e lista as funções com
      quadro grande. Resultado:
      - `sub_001A7AE8`: 773 KB. Pesa 0% na seleção, na luta e na história (perfil com pilhas), então a EE-e não
        sobe na fila.
      - `fastBlock` dos blocos 3 e 9 do VU1: 473 e 462 KB. A biblioteca `urban_vu1_code` era compilada **sem**
        `/d2OptimizeHugeFunctions`. No bloco 9, 45% das instruções acessavam a pilha acima de 64 KB.
      - O `sub_002991D8` de 410 KB era um objeto velho, de uma recompilação anterior (o fonte atual tem 7 KB).
    - Correção: `/d2OptimizeHugeFunctions` no `urban_vu1_code`. Compila em ~3 min. `obs`: 0 divergências em
      ~213 mil execuções.
    - **Seleção com 4: 47,5 → 59,9 quadros/s, velocidade 100%**, pior quadro 22 ms, 16,8% de folga na thread do
      jogo. O bloco 9 caiu de ~41% para 4,6% da GameThread.
    - **Luta 1v1: 60 quadros/s a 100%**, folga 2,7%. História (missão): ~58 quadros/s a 96%.
    - As medições do Plano 1 (itens 45-46) foram feitas sobre esse código não otimizado. Os ganhos relativos de
      (a)/(c)/sticky continuam valendo, mas o peso que sobra para (b)+(d) é bem menor agora.
    - Locks na GameThread (4%):
      - `PS2Runtime::drainCompletedDmacHandlers`: 2,4%;
      - `GSThreadedBackend::Enqueue`: 1,0%;
      - `GS::processGIFPacket` (mutex recursivo do estado): 0,2%.
    - Perfil da seleção agora, GameThread:
      - folga 16,8%;
      - `memmove` 10,1%;
      - VU1 bloco 8 8,6% e bloco 9 4,6%;
      - `accountCycles` + `checkpointDue` 5,7%;
      - `vertexKick` 4,0%;
      - `sub_00292670` 2,6%;
      - locks 4%.
    - clang-cl não está instalado (nem no VS, nem como LLVM avulso). O experimento depende de instalar.

49. **Cena pesada de referência: luta com 4 lutadores no mapa do heliporto** (`bench/states/fight4_heavy2.state`,
    F5 do usuário no começo da luta; `bench_state.py --state ...`, `tools/frame_times.py` para o tempo de cada quadro).
    - Ferramentas novas:
      - A captura F6 grava também `perfil_<n>_quadros.txt` (horário de cada quadro) e `perfil_<n>_linha.txt` (amostras
        da GameThread no mesmo relógio). `frame_times.py --capture build/port/bin/perfil_<n>` mostra a mediana, o
        p99, o pior quadro e o que a thread fazia nos quadros lentos.
      - `URBAN_NATIVE_PROFILE_TIMELINE=<arquivo>` faz o mesmo para as execuções automáticas.
      - `URBAN_NATIVE_PROFILE_WALK=<thread>` percorre as pilhas de outra thread (ex.: GSThread).
    - Linha de base, sem profiler: **40,2 quadros/s**, mediana 25,4 ms, p99 30,6 ms, pior 37,2 ms. Não houve quedas
      pontuais: os quadros lentos são de carga normal. Na captura do usuário: 36,9 quadros/s, mediana 27,7 ms.
    - Na GameThread, ~18% era espera. Quem espera o quê:
      - **Downloads de VRAM de 8×8 pixels**, ~1 por quadro (`sub_003155F8`, um deles do buffer de profundidade, Z32),
        provavelmente o teste de visibilidade do lens flare: 9,5%. Cada um obriga a GPU a terminar tudo antes
        (`localToHostTransfer` na GSThread).
      - **Fila cheia para a GSThread** (`Enqueue → PublishLocked`): 8,6%. A GSThread ficava 66% parada esperando
        trabalho: a produção vem em rajadas (a saída de cada job de VU1) e só cabiam 8 blocos de 64 KB.
    - Correções:
      - fila de 64 blocos por padrão (`PS2X_GS_QUEUE_CHUNKS`; 256 dá o mesmo): **44,6 quadros/s**;
      - VCALLMS sem o reset duplo do VU0. `reset()` e `execute()` zeravam o pipeline duas vezes por chamada, ~4% em
        memset; agora `ps2xVuResetStateForExecute` zera só o estado.
    - Resultado: **45,7 quadros/s, velocidade 85%**, mediana 22,0 ms, p99 28,3 ms, pior 34,8 ms. `obs` sem divergências.
    - Ainda pesa: VU1 (~20% próprio), VU0 (~9%, contas exatas), GS front end (`vertexKick`, ~5%), downloads de VRAM
      (~9%), `memmove`.

50. **Plano 3: MTVU como padrão, com o modelo de tempo do PCSX2.** `PS2X_MTVU=0` volta ao modo síncrono.
    - **Vblank condicionado** (`asyncvif::vblankWaitEnabled`, `PS2X_MTVU_VBLANK_WAIT=0` desliga):
      - Um vblank vencido em ciclos espera os workers de VU/GIF ficarem sem trabalho executável. Dados que o GS segurou
        num SIGNAL esperam o EE e não contam como trabalho.
      - Uma interrupção que chega durante a espera roda antes, com o prazo tirado dos checkpoints. 100 ms sem
        progresso gera log e o vblank segue.
      - Sem recuperação de tempo (`hostCatchUp`) e sem o pacing do `vu1Busy`.
    - **O tempo emulado não corre à frente do real enquanto os workers trabalham** (`hostCatchUp` nesse modo; até
      0,5 ms de folga, espera de no máximo 1 ms por checagem).
      - Sem isso, o EE girava no laço que espera o "quadro pronto" e queimava quase um campo de ciclos emulados antes de
        o render terminar nas outras threads. O jogo "achava" que o quadro tinha levado um campo: quadros em +0,01 e
        +0,97 campo, seleção bimodal entre 48 e 58 quadros/s.
    - **XGKICK durante o job:** a saída PATH1 é publicada para a GIFThread a cada 32 KB, sem esperar o fim do job
      (`PS2X_MTVU_STREAM=0` desliga).
    - **Escritas de CSR/IMR/SIGLBLID sem sincronizar** por padrão (`PS2X_GS_WRITE_SYNC=1` volta).
    - Resultado no heliporto:

      | Medição | Antes (síncrono) | MTVU |
      |---|---|---|
      | Quadros/s | 45,7 | **58,8-59,6** |
      | Velocidade | 85% | **100%** |
      | Mediana | 22,0 ms | 17,1 ms |
      | p99 | 28,3 ms | 22,9-24,1 ms |
      | Pior quadro | 34,8 ms | 25-29 ms, com um pico raro de 68-79 ms ainda não explicado |

      Seleção com 4: 60 quadros/s estáveis (4/4 execuções).
    - Regressão com MTVU:
      - lutas pelo `drive.py`, inclusive a tela do save: 3/3;
      - modo história: OK;
      - `obs` do VU: 0 divergências (~360 mil execuções).
    - **Downloads de VRAM assíncronos** (`gs_threaded_backend.cpp`, `PS2X_GS_ASYNC_READBACK`).
      - Padrão: só blocos de até 8×8 do buffer de profundidade (formatos Z). A chave é buffer + largura + formato +
        tamanho, sem a posição. Num acerto, o jogo recebe o bloco da leitura anterior (um quadro de atraso) e a GSThread
        lê o novo em segundo plano; a primeira leitura é síncrona. Os outros downloads continuam exatos.
      - Espera por download na GameThread, sob MTVU: **13,5% (síncrono) → 10,0% (só profundidade) → 4,4% (`all`,
        inclui o bloco de cor 8×8)**.
    - GameThread no heliporto, sob MTVU:
      - espera 13,6%;
      - VU0 ~13% (bloco 0 da imagem de VU0, `executeVU0Microprogram`, `syncAllNorm`);
      - `memmove` 8,3%;
      - `accountCycles` 4,1%;
      - IOP 3,2%;
      - locks 2,4% (`drainCompletedDmacHandlers` 2,0%).

51. **Save states no meio do jogo (F5/F9) com MTVU e a thread do IOP.**
    - **F9 travava em 0 FPS** (música tocando, velocidade 100%): o jogo esperava o "quadro pronto" em `sub_00128DC0`.
      - Causa: o estado transitório do MTVU não está no arquivo (dados retidos num SIGNAL pendente, fila de
        SIGNAL/FINISH, serials de interrupção, prazo adiado, leitura de VRAM assíncrona) e continuava valendo por cima
        do estado carregado.
      - Correção: o load zera esse estado (`ps2xGsResetStall`, `ps2xGsResetSignalState`, `ps2xGsResetAsyncReadback`,
        `ps2xSchedulerResetGsTransientState`). O save espera o ponto seguro enquanto o GS retém dados num SIGNAL
        ("GS SIGNAL stall pending"), coisa de alguns ms. O formato do arquivo não mudou.
    - **A música sumia depois de alguns loads, e o controle demorava a responder.**
      - Causa: o IOP só era sincronizado (`join` + entrega dos efeitos adiados da thread do IOP: escritas na RAM do EE,
        comandos SIF) na seção EIOP, **depois** de a RAM e o scheduler do EE já terem sido gravados. O que isso produzia
        do lado do EE ficava fora do arquivo. No load, o mesmo `join` escrevia os efeitos da sessão antiga por cima da
        RAM recém-carregada.
      - Reproduzido com `saveload_psm.py` (scratchpad): ciclos de F5/F9 no meio da luta com `URBAN_PSM_AUDIT` e
        `PS2X_AUDIO_DUMP`. Depois do 6º load, as respostas do IOP ao gerenciador de som da CRI (0x2fa200) foram a 0/s
        para sempre, e o áudio passou a ter trechos de silêncio.
      - Correção: `IopSubsystem::flushPendingWork()` no início de `serviceStateRequests`, antes da checagem do ponto
        seguro e de qualquer serialização.
      - 10 ciclos: respostas do IOP contínuas, áudio sem silêncio em 119 s, 57-60 quadros/s depois de cada load.

52. **Heliporto "60 FPS mas parece 30-40": quadros alternando em volta do vblank.**
    - **Medição.** `PS2X_FRAME_TIMES` + `PS2X_FRAME_HASH=1` (hash da imagem mostrada em cada present) e
      `tools/image_cadence.py`.
      - Heliporto com 4 lutadores: 58-59 quadros de jogo/s, mas só **37-41 imagens distintas/s**.
      - 62% das imagens ficavam 2 refreshes na tela e a seguinte era pulada (padrão `.N.N`).
      - O contador do overlay mede outras coisas:
        - `Jogo` = chamadas de `0x128dc0` por segundo;
        - `Tela` = presents do host;
        - `Velocidade` = campos emulados / 59,94.
      - Nenhum deles conta imagens distintas.
    - **Como o jogo exibe.**
      - DISPFB nunca muda: circuito 2 (`PMODE=2`), sempre `0x8c`.
      - O primeiro pacote da cadeia de cada quadro copia o back buffer (fbp 0) para `0x8c`. `sub_0010C680`, chamada
        no início de `sub_00128DC0`, dispara essa cadeia.
      - Então a imagem muda no início de cada quadro (o `F` do trace).
      - Com o present síncrono (`PS2X_GS_ASYNC_PRESENT=0`), um present mostra imagem nova se e somente se houve um
        `F` desde o anterior. O present assíncrono não tem culpa.
    - **O limitador do jogo** (modo 480p, `[0x34d398]+0xEA == 2`):
      - O loop em `0x12a1a0..0x12a204` gira até `T3 − T3(no vblank) >= s0`, com `s0 = 504 + [obj+0x4C]` (= 510)
        ticks de H-BLANK, de 525 por campo. Usa `sub_00140D40`; a referência é guardada por `0x128290` em `+0xFE/+0xFC`.
      - Ou seja, o quadro começa 15 linhas (~0,48 ms) antes do vblank. O jogo então entra numa seção com
        interrupções desligadas, onde dispara a cadeia.
    - **A causa.**
      - Nosso modelo de ciclos do EE cobra 32 ciclos por desvio para trás (iteração de loop) e 8 por chamada. Código
        em linha reta não custa nada.
      - A seção do início do quadro (muitos loops curtos) custava ~300 mil ciclos, mais que as 15 linhas (~147 mil).
      - O vblank caía dentro dela, e o handler de vblank só rodava no EI, ~165 unidades (≈18 linhas) depois.
        Medido: atrasado em exatamente metade dos campos (`Y` no trace).
      - Com isso a referência do T3 ficava atrasada, e o alvo do quadro seguinte passava do vblank: o quadro
        começava depois dele.
      - Resultado: dois quadros num campo e nenhum no seguinte.
      - Não é o timing do VU1: `PS2X_VU1_TIMING=0` dá o mesmo padrão.
    - **Correção (port).** `PS2X_EE_LOOP_CYCLES=16` por padrão (`src/main.cpp`; o runtime continua com 32; `=32`
      volta ao comportamento antigo).

      | cena | ciclos/loop | quadros/s | velocidade | imagens distintas/s | imagem 1 refresh | present mediana / p99 / pior |
      |---|---|---|---|---|---|---|
      | heliporto 4 | 32 | 55,9 | 93% | 32,5 | 29% | 18,2 / 26,6 / 29,8 ms |
      | heliporto 4 | 24 | 56,6 | 94% | 44,1 | 72% | 17,6 / 25,3 / 29,9 ms |
      | heliporto 4 | 20 | 56,6 | 94% | 48,9 | 85% | 17,6 / 24,6 / 36,7 ms |
      | heliporto 4 | **16** | 54,7 | 91% | **52,5** | **96%** | 18,1 / 24,3 / 27,2 ms |
      | heliporto 4 | 8 | 49,5 | 82% | 48,7 | 99% | 20,0 / 26,2 / 28,7 ms |
      | mapa pesado 1 (4) | 32 | 59,8 | 100% | 49,6 | 80% | 16,7 / 20,1 / 52,0 ms |
      | mapa pesado 1 (4) | **16** | 59,9 | 100% | **58,9** | **98%** | 16,7 / 19,9 / 47,8 ms |
      | seleção 4 | 32 | 60,0 | 100% | 52,3 | 85% | 16,7 / 20,3 / 26,5 ms |
      | seleção 4 | **16** | 60,0 | 100% | **59,4** | **99%** | 16,7 / 19,6 / 23,2 ms |

      - As linhas do heliporto desta tabela foram medidas numa máquina mais carregada que a das medições anteriores
        (a 32 dava 58-59 quadros/s antes).
      - O que limita o heliporto agora é a cadeia de desenho: ~18 ms reais por quadro (EE + VU1 + GIF + GS). O vblank
        espera a cadeia, então ele roda a ~55 com cada imagem mostrada uma vez.
      - Para 60 lisos ali, a cadeia precisa caber em ~16 ms.
    - **Trace novo** (`PS2X_MTVU_TRACE`, buffer de 200 mil eventos):
      - `D` = escrita em DISPFB;
      - `R` = present (DISPFB1/2, PMODE, campo);
      - `C` = troca de render target no GS (fbp | contexto << 12);
      - `W` agora leva o ciclo do EE / 1024;
      - `Y` = ciclo do EE / 1024 quando uma interrupção é despachada.
    - `PS2X_GS_ASYNC_PRESENT`:
      - 0 = readback síncrono;
      - 1 = assíncrono, mostra o readback mais novo já pronto;
      - 2 (padrão quando o port liga o assíncrono) = espera até 8 ms pelo readback do present anterior.
      - No heliporto os três dão a mesma cadência.

53. **Controle parado depois de F9 (20 s no primeiro load, não voltava nos seguintes).**
    - **Reproduzido** com `loadinput.py` (scratchpad): F9 três vezes no heliporto, apertando um botão o tempo todo, e
      `PS2X_PAD_RATE=1` (comandos de pad por segundo, no IOP).
      - Depois do 2º load, o PADMAN parou de consultar o controle.
    - **Causa.** `IopVblank` (os handlers de `RegisterVblankHandler` e o ciclo do último vblank atendido) não ia para
      o arquivo.
      - No load, o `totalCycles` restaurado ficava atrás do último vblank. Nenhum handler rodava até o IOP alcançar o
        tempo do momento do load.
      - O PADMAN consulta o controle de um desses handlers. Então o controle ficava parado pelo tempo que o jogo tinha
        rodado desde o ponto do state.
    - **Correção.**
      - Seção `IVBL` no estado do IOP.
      - States antigos, sem ela, reancoram o relógio de vblank no tempo restaurado e continuam carregando.
      - Testado com o state antigo (3 loads) e com save + 2 loads no formato novo: 53-60 comandos/s depois de cada load.

54. **Saves da história corrompidos ao gravar no memory card.**
    - `tools/mc_check.py` (novo) checa a imagem do cartão: superbloco, FAT, árvore de diretórios, ECC.
      - O cartão estava íntegro.
      - O conteúdo dos arquivos do Urban gravados aqui não estava. Em `PROFILEn` e `STORYn`, só os primeiros 8 bytes
        e alguns bytes isolados estavam certos. O resto era zero ou lixo do `icon.sys` gravado logo antes ("PS2D" no
        meio do save).
    - **Causa.** `sceSifGetOtherData` (sifcmd, import 23) era um stub.
      - É com ela que o MCSERV puxa da RAM do EE os dados de uma escrita. O libmc manda no RPC só o começo e o fim
        desalinhados.
      - Toda gravação levava o buffer velho do IOP.
      - A leitura funcionava porque vai pelo `sceSifSetDma`.
    - **Correção.** A função agora copia `size` bytes do EE (`a1`) para o IOP (`a2`).
    - `URBAN_MC_DIR=<pasta>` aponta os cartões para uma cópia (testes).
    - **Formatar travava.** "Formatting complete" aparecia sem nada gravado; o acesso seguinte ficava em 0 FPS com
      `RPC server 0x4403c still blocked`.
      - O emulador do IOP executa cada RPC até o fim e desistia depois de 2 s de tempo do IOP.
      - A formatação lê as páginas 0 e 1 dos 1024 blocos antes de gravar (~6 s de tempo do IOP).
      - O RPC voltava antes da hora, e a chamada abandonada ficava com o MCMAN preso.
      - Agora o prazo é estendido a cada janela em que a SIO2 teve transferências (até 120 s).
      - Reproduzido e testado com `fmtdrive.py` (scratchpad), num cartão de teste: perfil novo → salvar → formatar →
        "Save complete" → menu principal. O cartão final passa no `mc_check`, e o perfil tem o mesmo padrão de um
        perfil bom do PCSX2.

55. **Salvar num slot novo: tela parada e ~10 s sem controle.**
    - **Medido** com `live.py`/`savetest.py` (scratchpad) e `PS2X_PAD_RATE`, `PS2X_AUDIO_STATS`, `PS2X_AUDIO_DUMP`.
      - Cada RPC do memory card rodava até o fim dentro da chamada do EE: o EE ficava parado ~3 s enquanto o IOP corria
        ~8 s.
      - Depois o IOP esperava o EE alcançar: ~10 s sem consultar o controle.
      - O stream de música ficava sem dados (silêncio) durante o save.
      - A "música alta" depois do save **não era bug**: o perfil novo vem com o volume do jogo no máximo, e o save
        antigo do usuário tinha o volume mais baixo (RMS ~8000 contra ~1300).
    - **Correção 1 (testada).** O tempo do IOP numa chamada bloqueada não vira dívida com o EE (`eeSyncCycle`).
      - Resultado: controle a 60 comandos/s logo após o "Save complete".
    - **Correção 2 (testada pelo usuário: o controle funciona durante o save; ainda há pequenos cortes de áudio e
      travadinhas, deixados assim por enquanto).** RPC NOWAIT assíncrono, que é como o libmc chama todas as operações
      do cartão.
      - Se o servidor bloqueia, a thread fica rodando no IOP com pilha própria (até 4 chamadas pendentes) e o EE
        retorna na hora com o cliente ocupado.
      - No fim: cópia da resposta → `IopHost::completeRpc` (adiado pelo DeferringHost) → `completeAsyncIopRpc` no
        EE, que sinaliza o semáforo, roda a função de término como invocação e limpa o "busy".
      - Savestate é bloqueado enquanto houver RPC pendente ("IOP RPC pending").

56. **Carregador de pnach** (`ps2xRuntime/src/lib/ps2_pnach.cpp`, genérico).
    - **Configuração:** `PS2X_PNACH=<arq>[;<arq>...]` no controles.ini ou no ambiente; `none` desliga.
    - **Quando aplica:**
      - `patch=0`: uma vez, depois do ELF;
      - `patch=1`: a cada vblank, como no PCSX2, então vale também depois de carregar um savestate;
      - `patch=2`: nos dois momentos.
    - **Tipos:** byte, short, word, double e extended (códigos CodeBreaker 0/1/2).
    - **Log:** `[pnach] <arq>: N patches loaded` e, a cada ~10 s, `[pnach] X writes applied so far`.
    - O `SLUS-21209_BDD9BAAD.pnach` do usuário (8 jogadores) carrega 114 patches, aplicados a cada vblank.
    - `bench_state.py --pnach <arq>` liga o pnach e falha se o log não mostrar que ele foi aplicado. Sem a opção, roda
      com `none`, mesmo que o controles.ini liste arquivos.

57. **Primeira medição com 8 lutadores no heliporto** (`bench/states/fight8_heli.state`, pnach ativo, nada
    otimizado).
    - **Resultado:** 47,3 quadros/s, velocidade 79%, 46,5 imagens distintas/s, 98% mostradas uma vez. Present:
      mediana 21,0 ms, p99 28,4 ms, pior 40,8 ms. Reprova em todos os critérios.
    - **CPU por thread** (`tools/thread_usage.py`, % de um núcleo → ms por quadro de 20,8 ms):

      | thread | CPU | ms/quadro |
      |---|---|---|
      | GameThread | 88% | ~18,3 |
      | VUThread | 47% | ~9,8 |
      | GSThread | 36% | ~7,5 (+3,6 ms esperando fences da GPU) |
      | present | 35% | ~7,3 |
      | GIFThread | 28% | ~5,8 |
      | IOPThread | 98% | 77% é espera ativa no `workerLoop`; trabalho real ~4 ms |

    - **GPU:** intervalo `TIME_ELAPSED` de 16-18 ms por quadro. Inclui ociosidade, então é um teto. 354 lotes e
      213 cargas de CLUT por quadro.
    - **A GameThread limita:** pacing 0, espera de sync no VU/GIF 0,05 ms por quadro.
    - **Composição da GameThread** (profiler nativo, inclusivo):
      - VU0 22,5%;
      - espera de download de VRAM 10,6%;
      - memmove 8%;
      - overhead do runtime ~12% (`accountCycles` 4,8, `checkpointDue` 3,3, dispatch 2,4...);
      - handler de GS 5,5% (1,6 ms por quadro);
      - RPC do IOP 5,3%;
      - decodificador de animação 5,9%;
      - handlers de vblank 0,7 ms por quadro.
58. **ROADMAP A1 — IOP sem espera ativa** (2026-10-10).
    - **O quê** (`ps2xIOP/src/iop_subsystem.cpp`): o worker do IOP gira só `PS2X_IOP_WORKER_SPIN_US` (padrão 20 µs)
      com `pause` entre lotes e depois dorme em `WaitOnAddress` sobre `busy`. O EE, ao postar um lote
      (`runEeCyclesAsync`), grava `busy` e chama `WakeByAddressSingle` só se vir o worker dormindo (pares
      store/load `seq_cst` dos dois lados, estilo Dekker; o `WaitOnAddress` reconfere o valor, então não se perde
      wake). O `join()` do EE gira `PS2X_IOP_JOIN_SPIN_US` (padrão 200 µs; um lote leva ~15 µs) e só depois dorme;
      o worker acorda o EE ao terminar só se ele estiver dormindo. Nenhum `yield`. `Synchronization.lib` por
      `#pragma comment`.
    - **Chave:** `PS2X_IOP_SPIN=1` volta ao comportamento antigo. O log mostra a política no boot
      (`[iop-thread] wait policy: ...`); `PS2X_IOP_THREAD_STATS=1` agora também dá wakes do worker/s e sonos do join/s.
    - **Bench:** `bench_state.py` ganhou `--affinity 0-3` (afinidade depois de lançar, psutil) e `--threads` (roda
      `tools/thread_usage.py` na janela medida e grava `<tag>/threads.txt`).
    - **fight8_heli** (pnach, 32 s, mesma sessão):

      | | quadros/s | velocidade | distintas/s | 1× | present med/p99/pior | IOPThread | processo |
      |---|---|---|---|---|---|---|---|
      | antes | 49,5 | 83% | 49,2 | 98% | 19,7 / 28,3 / 54,5 | 95,9% | 348% |
      | depois | 50,0 | 83% | 49,0 | 98% | 19,9 / 26,2 / 31,1 | 35,6% | 293% |
      | afinidade 0-7, antigo | 47,4 | 79% | 46,5 | 98% | 21,1 / 27,3 / 29,2 | 97,7% | |
      | afinidade 0-7, novo | 48,1 | 80% | 47,2 | 98% | 20,8 / 26,5 / 28,6 | 38,1% | |
      | afinidade 0-3, antigo | 41,8 | 70% | 33,4 | 75% | 23,8 / 31,1 / 33,4 | 79,5% | |
      | afinidade 0-3, novo | 41,8 | 70% | **38,9** | **92%** | 23,8 / 30,3 / 32,6 | 29,6% | |

      - Lotes: ~14,5 mil/s de ~14-15 µs (worker ocupado ~21%); ~4,9 mil wakes/s do worker; o join dorme ~20 vezes/s.
      - "EE join wait" igual (0,8-1,7%). Profiler nativo: `IopSubsystem::handleRpc` 5,3% → 5,2% da GameThread,
        `Impl::join` 1,3% → 0,6%; `workerLoop` 76,8% → 13,2% da IOPThread.
      - Na máquina de 2 núcleos/4 threads o ritmo não muda (a GameThread limita), mas a cadência melhora muito: a
        CPU que o IOP queimava ia para as threads de render.
    - **Regressão:** `fight4_heavy2` antigo 59,4 / novo 59,5 quadros/s (distintas 58,1 / 58,0; pior present 57,5 /
      32,5 ms); `select4` 59,9 quadros/s, 59,8 distintas/s, 100% 1×. Recarga do state no meio da luta
      (`--reload-after 12`): 59,3 quadros/s, nada de erro no log. Regressão funcional completa (história, memory
      card, áudio de ouvido) fica para o fim da Fase A, como pede o roadmap.
    - **Critério de 60 liso no fight8_heli:** continua reprovando nos 4 itens (a GameThread ainda limita, ~86-89%).
59. **ROADMAP A2 — afinidade e prioridade por topologia** (2026-10-10).
    - **O quê:** módulo novo do port, `src/urban/thread_policy.cpp` (nada no runtime). No boot, ele nomeia a thread
      principal `MainThread` e sobe uma thread leve (`ThreadPolicy`) que acha as threads pelo nome (snapshot do
      Toolhelp: a cada 1 s até achar todas, depois a cada 15 s ou quando a máscara do processo muda) e aplica o plano.
      - **Topologia** por `GetLogicalProcessorInformationEx`: núcleos físicos, irmãos SMT, grupos de L3/CCX, classe de
        eficiência (P/E em híbridos), dentro da máscara de afinidade do processo.
      - **GameThread:** sozinha num núcleo físico (o primeiro dos mais rápidos que não tem o LP 0, onde caem as
        interrupções), prioridade acima do normal. O irmão SMT dela fica livre.
      - **VU, GIF, GS, MainThread, IOP**, nessa ordem, um LP por núcleo; o irmão SMT só quando acabam os núcleos.
        `auto` põe VU/GIF primeiro no mesmo L3 da GameThread; `spread` primeiro em outro L3.
      - **auto** = processador ideal (`SetThreadIdealProcessorEx`) para todas + **máscara rígida (um núcleo físico) só
        nas duas threads de OpenGL**, `MainThread` e `GSThread`. **ccx** = máscara rígida em todas.
    - **Chaves:** `PS2X_THREAD_POLICY=off|auto|spread|ccx` (padrão auto), `PS2X_THREAD_PRIORITY=0`,
      `PS2X_THREAD_HARD=<nomes>|none` (quais threads têm máscara rígida em auto/spread).
      `PS2X_PROCESS_AFFINITY=0-7` restringe o processo **no boot** (vale até com a política off). O `bench_state.py
      --affinity` agora usa isso no lugar do psutil depois de lançar: mudar a afinidade de fora reseta as máscaras das
      threads depois de o contexto GL existir, o que um PC pequeno de verdade nunca faz, e a medição saía errada.
    - **Achado: a thread "present" de ~35-40% da tabela do §57 não é o present.** É a thread de "threaded
      optimization" do driver NVIDIA (`nvoglv64.dll`), que repassa as chamadas GL da GSThread; a thread principal
      (janela/present) gasta só ~5%. Uma máscara rígida na thread da janela **antes de o contexto GL nascer** faz o
      driver desligar esse worker. A GSThread passa a fazer o GL ela mesma (~66-76% de um núcleo), e o processo gasta
      o mesmo ou menos CPU.
    - **Isolando (fight8_heli, 6 núcleos, quadros/s):**

      | configuração | quadros/s | distintas/s |
      |---|---|---|
      | off (3 execuções) | 50,1 / 49,5 / 49,9 | 49,0 / 48,5 / 48,6 |
      | auto só com processador ideal | 49,9 / 48,7 | 48,6 / 47,9 |
      | spread só com processador ideal | 49,0 | 47,4 |
      | ccx, tudo rígido (3 execuções) | 52,2 / 53,1 / 53,0 | 51,6 / 52,3 / 52,3 |
      | rígida só na GSThread | 50,0 | 48,6 (o worker do driver continua) |
      | rígida só na MainThread | 50,6 | 49,5 (o worker some) |
      | rígidas em Game/VU/GIF/IOP | 50,3 | 49,5 |
      | rígidas em Main+Game+VU+GIF | 49,5 | 48,6 |
      | **rígidas em Main+GS (o auto final)** | **52,6 / 52,6** | **51,8 / 51,9** |

      O ganho vem de duas coisas juntas: o driver sem o worker dele e a GSThread presa a um núcleo só dela. Isso
      provavelmente encurta a espera da GameThread nos downloads de VRAM (8×8), que dependem da GSThread.
    - **Resultado do auto final** (afinidade aplicada no boot):

      | cena | off | auto |
      |---|---|---|
      | fight8_heli, 6 núcleos | ~49,8 q/s, 48,7 distintas | **52,6**, 51,9; present med/p99/pior 18,9 / 24,9 / 27,1 ms; velocidade 88% |
      | fight8_heli, 4 núcleos/8 threads (0-7) | 46,5, 45,4 | **49,5**, 48,6 |
      | fight8_heli, 2 núcleos/4 threads (0-3) | 40,8, 37,8, 92% 1× | **42,6**, 40,9, 96% 1× |
      | fight4_heavy2 | 58,6, 56,9 | **59,8**, 58,8 |
      | select4 | — | 59,9, 59,7, 100% 1× |

      - `ccx` com 2 núcleos é ruim (30,8 quadros/s): tudo rígido em 2 núcleos sufoca. Por isso o padrão é o auto.
      - Tabela por thread, fight8, auto: GameThread 88,7%, GSThread 69,9%, VUThread 45,3%, IOPThread 39,1%,
        GIFThread 36,1%, MainThread 7,5%; o worker do driver sumiu; processo 287%.
    - **Para a A3:** a "leitura GPU→CPU→GPU de ~7,3 ms na thread de present" do roadmap foi tirada dessa thread do
      driver. Rever o que a A3 ainda tem a ganhar medindo de novo.
    - Sem `timeBeginPeriod(1)`: o sono de ritmo continua 0 nessa cena.
    - **Critério de 60 liso no fight8_heli:** ainda reprova (52,6 quadros/s, velocidade 88%); a GameThread limita.
60. **ROADMAP A3 — present sem a cópia GPU→CPU→GPU** (2026-10-10).
    - **Por que estava desligado:** não há bug conhecido. O `PS2X_GS_DIRECT_PRESENT` veio do runtime do SotC (commit
      `1808cc1`, "experimental", padrão 0 no runtime), e o port do SotC o liga por padrão no `sotc.ini` dele. O
      Urban só nunca ligou.
    - **Como funciona:** o contexto GL da GSThread compartilha texturas com o da janela. O compositor CRTC escreve
      num buffer de GPU que vira textura por PBO, num anel de 8 com fences de produtor/consumidor; a janela pega a
      mais nova pronta. Não há leitura para a RAM nem upload por quadro. Com `PS2X_FRAME_HASH=1`, o hash já é
      calculado na GPU (compute) e só 8 bytes são lidos depois da fence do produtor, presos ao número da imagem: não
      bloqueia e não precisa de casamento posterior. O caminho por cópia fazia o hash na CPU na thread principal.
    - **Mudança:** `src/main.cpp` liga `PS2X_GS_DIRECT_PRESENT=1` por padrão (`=0` volta à cópia). Nada no
      runtime. As outras chaves "experimentais" do SotC (`PS2X_GS_COMPACT_QUEUE`, `PS2X_VIF_SIMD_UNPACK`,
      `PS2X_VU1_SIMD_CLIP`) já vêm ligadas no runtime.
    - **Correção do roadmap:** depois da A2 a cópia já custava pouco. A espera de fence do present era 0,01 ms/quadro
      (`PS2X_GS_GPU_STATS`), e a thread principal gastava ~7% (os "7,3 ms" eram do worker do driver NVIDIA, §59).
    - **Medidas** (mesma sessão, com o A2 ligado):

      | cena | cópia (`=0`) | direto (padrão novo) |
      |---|---|---|
      | fight8_heli, 6 núcleos | 51,5 q/s, 50,6 distintas, 98% 1×, p99 25,7 ms, pior 29,0 | **52,2**, **51,7**, 99%, p99 **23,3**, pior **26,3** |
      | fight8_heli, 0-7 | 48,8, 47,9 | 49,7, 48,9 |
      | fight8_heli, 0-3 | 42,3, 40,8, 96% 1× | 42,6, **42,1**, **99%** 1× |
      | fight4_heavy2 | (59,8, §59) | 59,8, 59,2, 99% |
      | select4 | 59,9, 59,8 distintas, 100% | 59,9, 59,5, 99% |

      - O hash na GPU dá os mesmos números do hash na CPU na `select4` (59,5 contra 59,8, dentro do ruído).
      - MainThread 6,7% → 2,9% de um núcleo; processo ~2% a menos (o ganho de CPU fica na casa do ruído com 6
        núcleos). A captura de tela do bench e o overlay F3 saem normais.
    - **Não testado** (o usuário pediu foco só em desempenho): tela inteira, minimizar/restaurar, F5/F9, F6. Se algo
      quebrar na tela, a primeira coisa a tentar é `PS2X_GS_DIRECT_PRESENT=0` no controles.ini.
    - **Critério de 60 liso no fight8_heli:** ainda reprova (52,2 quadros/s, velocidade 87%, pior 26,3 ms); a
      GameThread limita (~90%).
61. **ROADMAP A4 — compilação: base SSE4.1, um executável só** (2026-10-10).
    - **Antes:** só o código gerado (`urban_game_code`, `urban_game_code_huge`, `urban_vu1_code`) tinha
      `/arch:AVX2`; o runtime, o IOP e o port já compilavam sem `/arch` (SSE2), com intrinsics SSE4.1 explícitos.
    - **MSVC 14.36 não tem `/arch:SSE4.2`:** ignora a opção com o aviso D9002. A base SSE4.1 é, então, **sem
      `/arch`**: o código que o compilador gera é SSE2, e os intrinsics SSE4.1 do runtime (MMI etc.) continuam.
    - **Mudanças:**
      - `src/CMakeLists.txt`: opção `URBAN_ARCH=SSE41|AVX2` (padrão SSE41) no lugar do `/arch:AVX2` fixo. `AVX2`
        fica só para comparar. Define `URBAN_ARCH_NAME` no `urban`.
      - `src/main.cpp`, `checkCpu()`, primeira coisa do `main`: CPUID (SSE4.1; AVX2 com XGETBV). Loga
        `[cpu] SSE4.1 yes, AVX2 yes; build SSE41`. Sem SSE4.1 (ou sem AVX2 num build AVX2), mostra uma mensagem em
        inglês e português e sai com código 4, em vez de cair com instrução ilegal. No build padrão, o próprio
        código do compilador é só SSE2, então nada antes dessa checagem usa SSE4.1.
      - `CMakeLists.txt` (raiz): opção `URBAN_LTO` (padrão **OFF**), que põe `/GL` no `ps2_runtime`, no `ps2_iop` e no
        `urban` (não no código gerado), ligados com `/LTCG`.
      - `bench_state.py --exe <nome>` para comparar executáveis na mesma pasta.
    - **FMA:** o build AVX2 não contraía FMA (`/fp:precise` no VS 2022 não contrai; testado num `a*b+c` com
      `/arch:AVX2 /O2`, nenhum `vfmadd` no asm).
    - **SSE4.1 contra AVX2** (fight8_heli, alternados na mesma sessão): SSE4.1 51,9 / 52,1 q/s (51,2 / 51,5
      distintas); AVX2 51,9 / 52,6 (51,4 / 51,7). VUThread 45,5-45,9% contra 44,2-45,4%. **Diferença dentro do
      ruído:** sem despacho por CPUID, fica só SSE4.1 (passo 3 do roadmap). Por isso também não existe
      `PS2X_CPU_PATH`: não há dois caminhos.
    - **Verificação do VU:** `PS2X_VU1_VERIFY=obs` + `PS2X_VU0_VERIFY=obs` no fight8_heli, nos dois builds: 311.296
      execuções, **0 divergências** em ambos.
    - **LTO de verdade** (`URBAN_LTO=ON`, alternado): 49,6 / 49,4 q/s contra 51,8 / 51,9 sem LTO. **Piora ~2,3
      q/s**, consistente. Fica OFF. Hipótese não verificada: o `RelWithDebInfo` compila o runtime com `/Ob1`, e o
      LTCG muda inlining e layout das funções quentes chamadas pelo código gerado (`accountCycles` etc.), que não
      participa do LTCG. O PGO precisa de `/GL`, então não foi tentado.
    - **Build:** o build completo foi morto uma vez por falta de memória (12 `cl.exe` em paralelo, cada arquivo
      gerado passa de 1 GB). Com `cmake --build build/port --target urban -j 8` passou. Retomar não perde nada: o
      Ninja continua de onde parou.
    - **Pico raro:** apareceu duas vezes um pior quadro de ~215 ms (uma no AVX2, uma no SSE4.1 sem LTO), em
      execuções normais. É o pico raro conhecido (ROADMAP §9), agora maior; não investigado aqui.
    - **Final (SSE4.1, sem LTO, A1-A3 ligados):** fight8_heli 52,1 q/s, 51,4 distintas, 99% 1×, velocidade 87%,
      present 19,3 / 22,9 / 26,7 ms (mediana/p99/pior); com 4 núcleos (0-7) 50,2 q/s, 49,6 distintas, 99% 1×.
      Tabela por thread: GameThread 90,0%, GSThread 73,1%, VUThread 44,2%, GIFThread 39,5%, IOPThread 38,3%,
      MainThread 4,4%.
    - **Critério de 60 liso no fight8_heli:** ainda reprova; a GameThread limita (~90%). Próximo: Fase B (GPU) ou C
      (dieta da GameThread), conforme o usuário.
62. **ROADMAP C1 — VU0: FMACs rápidos e chamada enxuta** (2026-10-10). O usuário pediu a Fase C antes da B1.
    - **Censo:** o jogo **nunca** executa CFC2 de vi16 (status) nem de vi17 (MAC) do VU0. Lê só as flags de clip (23
      lugares, `grep` no `generated/`), que os FMACs rápidos não alteram. Só existe uma imagem de VU0
      (`e2a875f42172c34a`, 174 pares).
    - **Parte 1, FMACs rápidos no VU0** (`ps2xTest/tools/vu1_recomp.cpp`): imagens que cabem em 4 KB agora também
      usam `fmacFastV`/`fmacFastN` e são emitidas duas vezes: rápida com o hash normal, exata com
      `hash ^ "VU0EXACT"` (`b4fd45b17933801e`).
      - O runtime (`lookupCompiledVu0Program`) roda a exata com `PS2X_VU0_FAST=0`. `VU1_RECOMP_VU0_EXACT=1` no
        gerador volta à saída antiga.
      - A imagem de VU1 saiu byte a byte igual, e a variante exata é igual ao código antigo.
      - **Verificação** (`PS2X_VU0_VERIFY=obs`, fight8_heli, select4, fight4_heavy): 0 divergências. Sobram 95
        diferenças só nos bits sticky do status final (sinal/underflow), que só um CFC2 vi16 leria. O verificador
        agora as conta à parte ("VU0 final status differences only CFC2 vi16/vi17 could read").
      - **Ganho: nenhum mensurável** (52,8 / 51,9 contra 52,1 / 52,3 q/s). O profiler mostrou o motivo: o programa
        em si é só ~39% do tempo do VU0.
    - **O custo real era a preparação por chamada:** ~4.500 VCALLMS por quadro com 8 lutadores (~250 mil/s).
      Quebra do VU0 (24% da GameThread), pelas pilhas do profiler nativo:
      - `resetScheduler` zerando ~3,5 KB de arrays: 16%;
      - `syncAllNorm`, 33 registradores renormalizados: 13%;
      - cópias de contexto com dois `memset` do estado: ~16%;
      - o programa: ~39%.
    - **Parte 2, chamada enxuta** (`PS2X_VU0_LEAN`, padrão 1; `0` volta ao antigo; `check` liga as checagens):
      - `loadVu0Registers` (`ps2_vu1_impl.inl`): a sombra normalizada do VU0 não é mais jogada fora a cada
        chamada. Só os VF/ACC que o EE mudou são copiados e renormalizados. Qualquer par interpretado continua
        invalidando a sombra.
      - `executeVu0Lean`: os arrays do escalonador só são zerados se o interpretador os usou desde o último
        zeramento (`vu0SchedulerClean`; o ramo interpretado de `runLoopWith` e o load de savestate o derrubam).
        Os campos escalares são preparados como no `execute()`.
      - `executeVU0Microprogram` (`ps2_runtime.cpp`): sem `resetStateOnly` nem `memset`, copia só os escalares.
    - **Bug da primeira versão (o usuário viu antes de mim):** câmera no céu e piscando na luta, seleção sem os
      modelos, com "59,7 q/s" (a VUThread caiu para 6%).
      - Causa: o código AOT **usa** `m_vfReady`/`m_viReady`/`m_accReady` para os stalls entre blocos. Eu tinha
        olhado só o runtime e concluído que só o interpretador os usava.
      - Com o ciclo voltando a 0 a cada chamada e os valores velhos da chamada anterior no array, cada chamada
        "esperava" centenas de ciclos, estourava o orçamento de 4096 ciclos e terminava pela metade. Resultado:
        matrizes de skinning erradas.
      - Por que a verificação não pegou: no modo verify, a sombra interpretada derruba o `vu0SchedulerClean` e a
        sombra normalizada em toda chamada, então o caminho enxuto nunca roda ali.
      - Correção: em vez de zerar, o ciclo do VU0 continua de onde a chamada anterior parou, mais
        `kVu0LeanCycleGap` = 64 (as latências de VF/VI/ACC são ≤ 4). Todo valor velho fica "pronto", o mesmo efeito
        do zeramento. Os arrays de escrita pendente, só o interpretador usa (o código AOT toca só `m_*Ready`,
        `m_fdiv`, `m_viBranchBackup*` e `m_state`, por `grep` no gerado).
      - `PS2X_VU0_LEAN=check` confere em toda chamada que a sombra mantida = renormalização completa e que nenhum
        ciclo de "pronto" está à frente. Resultado: 0 registradores velhos e 0 escalonadores sujos em ~11 milhões de
        chamadas (select4 + fight8).
      - **Lição:** depois de mexer no VU, olhar as capturas `final.png`, não só os números e o verify.
    - **Resultado** (fight8_heli, alternado, imagens conferidas):

      | | antigo (`=0`) | enxuto |
      |---|---|---|
      | quadros/s | 52,2 / 52,7 | **54,5 / 54,8** |
      | distintas/s | 51,7 / 52,2 | **53,9 / 54,1** |
      | velocidade | 87-88% | **91%** |
      | present p99 | 23,3-24,0 ms | 22,0-22,5 ms |

      - fight4_heavy2: 59,9 q/s, 59,4 distintas, GameThread 74% (era ~77-80%). select4: 59,9 q/s, 59,5 distintas.
      - Tabela por thread, fight8: GameThread 88,7-89,5%, GSThread 71-77%, VUThread 47,6-48,5%, IOP ~40%,
        GIF 33-41%, MainThread 4%.
    - **Critério de 60 liso no fight8_heli:** ainda reprova (~54,6 quadros/s, 91%). A GameThread continua limitando.
63. **ROADMAP C2 — para que serve o download de cor 8×8** (2026-10-10).
    - **Censo** (`PS2X_GS_DOWNLOAD_LOG=1`, fight8_heli): exatamente **duas leituras por quadro**, as duas no ponto do
      sol na tela:
      - **profundidade**: `sbp=0x1a40 sbw=10 spsm=0x30` (Z32, o buffer de 640), 8×8 em (624,122). É o teste de
        oclusão do lens flare, já assíncrono por padrão desde o §50;
      - **cor**: `sbp=0x3480 sbw=5 spsm=0x1` (CT24, uma cópia de **meia resolução**, 320 de largura) em (312,61), o
        mesmo ponto dividido por 2. O tamanho varia de 1×16 a 8×8 quando o sol encosta na borda. É o brilho no ponto
        do sol, usado para dosar o flare/brilho.
      - Ambas vão por VIF1 DMA GS→memória (`0x44a7d0`, 256 bytes; `0x44a8d0`, 128 bytes) e são lidas por
        `sub_003155F8`.
    - **Decisão:** as duas só alimentam o efeito de luz do sol, então um quadro de atraso (1/60 s) na intensidade do
      flare é imperceptível. `src/main.cpp` passa a definir `PS2X_GS_ASYNC_READBACK=all` por padrão.
      - No runtime nada muda: o mecanismo do §50 usa a chave buffer + largura + formato + tamanho, sem a posição; a
        primeira leitura de cada chave é síncrona e as outras recebem o bloco da leitura anterior enquanto a
        GSThread lê o novo em segundo plano.
      - `=1` volta a só profundidade; `=0` deixa tudo síncrono.
    - **Medidas** (fight8_heli, alternado na mesma sessão): padrão antigo 54,9 / 54,7 q/s (54,1 / 54,0 distintas);
      `all` **58,5 / 57,8** (58,2 / 57,5 distintas), velocidade 91% → 97-98%.
    - **Padrão novo:**

      | cena | quadros/s | distintas/s | 1× | velocidade | present med / p99 / pior |
      |---|---|---|---|---|---|
      | fight8_heli | **57,9** | 57,4 | 99% | 97% | 17,2 / 22,3 / 33,4 ms |
      | fight8_heli, 4 núcleos (0-7) | 56,0 | 55,3 | 99% | 94% | 17,7 / 22,4 / 32,8 ms |
      | fight4_heavy2 | 59,9 | 59,1 | 99% | 100% | |
      | fight4_heavy | 59,9 | 59,5 | 99% | 100% | |
      | select4 | 59,9 | 59,5 | 99% | 100% | |

      - Tabela por thread, fight8: GameThread **94,9%**, GSThread 80,4%, VUThread 53,4%, IOPThread 43,4%,
        GIFThread 42,8%, MainThread 5,8%. A GameThread esperava menos e agora está quase sempre ocupada.
    - Imagens conferidas (`final.png`): heliporto com o sol e o flare no lugar; o outro mapa pesado normal.
    - **Critério de 60 liso no fight8_heli:** quase. 57,9 quadros/s, 99% mostradas 1×, velocidade 97%; o pior
      quadro (33,4 ms) é uma imagem repetida. Faltam ~2 q/s, e a GameThread continua limitando (~95%).
64. **ROADMAP C3 — overhead do runtime na GameThread** (2026-10-10).
    - **Perfil de partida** (próprio, fight8_heli com A1-C2): `accountCycles` 5,7%, `dispatchGuestBranch` 2,9%,
      `checkpointDue` 2,3%, `eeCheckpointDue` 1,4%, `inRange` 1,0%, `processPendingEvents` 0,9%, `pushDispatchPc` 0,7%,
      `translateAddress` 0,7%. Pelas linhas, o mais quente era a chamada `advanceEeTimers` dentro do `accountCycles`
      (2,6%) e a checagem dos serials do GS (1,1%).
    - **Mudanças:**
      1. `EeScheduler::accountCycles` (`PS2X_EE_FAST_ACCOUNT=0` volta ao antigo):
         - o caso comum de `PS2Memory::advanceEeTimers` (somar ciclos ainda abaixo do próximo evento de timer) é
           feito ali mesmo, nos campos públicos do `PS2Memory`. A função original só é chamada quando um timer vai
           disparar ou há escrita pendente no GIF_STAT, então os timers disparam no mesmo ciclo de antes;
         - o estado de interrupção do GS (CSR e os serials de SIGNAL/FINISH, lidos por ponteiro direto aos atômicos,
           `ps2xGsSignalSerialAddress`) é olhado a cada `PS2X_EE_GS_POLL_CYCLES` = 256 ciclos de EE (~1 µs), em vez
           de em todo checkpoint. São eventos de outras threads, sem ciclo fixo de chegada; a linha de cache que eles
           dividem com as threads GIF/GS custava um miss por checagem;
         - os ajustes viram globais simples: os `static` locais com inicialização dinâmica do MSVC checam um guarda
           em TLS a cada chamada.
      2. `eeCheckpointDue` e `dispatchGuestBranch`: os `static` de `PS2X_EE_LOOP_CYCLES`/`PS2X_EE_SINGLE_LOOKUP` sem
         guarda TLS.
      3. `gs_threaded_backend.cpp`: no modo `PS2X_GS_ASYNC_READBACK=all`, blocos de até **16×16** são assíncronos
         (eram 8×8). A leitura de cor do flare vira 1×16 a 7×16 quando o sol encosta na borda, e essas continuavam
         síncronas: 3,1% da GameThread esperando `ConsumeLocalToHostBytes` → **0,0%**.
    - **Efeito no perfil:** `accountCycles` 5,7 → 4,2-4,6%, `eeCheckpointDue` 1,7 → 0,9%, espera por download
      3,1 → 0%.
    - **Testado e descartado:** `PS2X_IOP_WORKER_SPIN_US=60` (acordar menos o worker do IOP). Mesmos quadros/s
      (57,9 / 58,7 contra 58,5 / 58,5) e IOPThread 42% → 61%. Fica 20 µs.
    - **Achados para os próximos passos** (pilhas da GameThread):
      - **6,3% é o IOP rodando dentro do `SifCallRpc`** (`IopSubsystem::handleRpc` → `IopEmulator::handleRpc`, o
        servidor RPC executado de forma síncrona), mais 0,5% de `sceSifSetDma`. É o item C5 do roadmap.
      - 1,9% é a GameThread acordando o worker do IOP (`WakeByAddressSingle`, ~4.900/s, ~4 µs cada): o custo da A1
        na GameThread. Girar mais não compensa (acima).
    - **Resultado** (fight8_heli): **58,5 / 58,5 quadros/s**, 57,7 / 58,1 distintas/s, 99-100% 1×, velocidade
      97-98%, present 17,1 / 22,0-22,2 / 33,4 ms (mediana/p99/pior). Vinha de ~57,5-57,9.
      - Tabela por thread: GameThread 95,4-95,9%, GSThread ~82%, VUThread ~51%, GIFThread ~43%, IOPThread ~42%,
        MainThread 5,9%.
      - fight4_heavy2 59,9 (59,4 distintas); select4 59,9 (59,5). Imagem conferida.
    - **Critério de 60 liso no fight8_heli:** ainda reprova por pouco (~58,5 q/s, 98%). A GameThread segue no limite.
65. **Comparação de CPU com o PCSX2 na mesma cena** (2026-10-10).
    - O usuário ligou os cheats do Urban no PCSX2 (o `gamesettings/SLUS-21209_BDD9BAAD.ini` tinha
      `EnableCheats = false`) e entrou numa luta de 8 no heliporto. O pnach é o mesmo arquivo que o port usa.
    - **Configuração do PCSX2:** DX12 (Renderer -1), 6× de resolução, MTVU, `HWDownloadMode = 0` (downloads exatos),
      filtros, mipmaps, FXAA e substituição de texturas ligados.
    - **Medição:** `tools/thread_usage.py --process pcsx2-qt.exe`, 20 s. Identificação das threads sem nome
      amostrando o RIP (script de rascunho com `GetThreadContext`; o `CONTEXT` precisa estar alinhado em 16 bytes):
      EmuThread **43%** (JIT do EE), thread GS **25%** (`nvwgf2umx`, D3D12), MTVU **18%** (JIT); processo ~86%.
    - **Port (C3):** GameThread 95% + IOPThread 42%, VUThread 51%, GSThread 82% + GIFThread 43%, MainThread 6%;
      processo ~320%.
    - Análise e itens novos (C3b contagem inline, C6 VU0 sem cópia, C7 HLE do NUSOUND, C8 regalloc do VU1, GS) no
      ROADMAP §7.1.
66. **ROADMAP C5 — o RPC de som sai da GameThread** (2026-10-11).
    - **Medido** com o novo `PS2X_IOP_RPC_STATS=1` (`IopSubsystem::handleRpc`: chamadas/s por sid e função, modo,
      tempo de alcance do IOP e do servidor): é **uma chamada só**, `sid 0x81000201 fn 1`, **NOWAIT**, uma vez por
      quadro (~60/s). Cada uma rodava ~1 ms do servidor no interpretador do IOP dentro do `SifCallRpc`, na GameThread:
      **6,3% do EE**. As outras (sid 0x90000200) são raras.
    - **Por que rodava na GameThread:** um NOWAIT só ficava assíncrono se o servidor bloqueasse (o mecanismo do §55).
      Este não bloqueia, então rodava inteiro na hora.
    - **Mudança** (`PS2X_IOP_NOWAIT_DEFER`, padrão 1; `0` volta ao antigo):
      - `IopRpcBridge::handleRpc` → `IopGuestExecutor::startServerFunctionAsync` (novo): o servidor vira uma thread
        do IOP pronta, com uma posição própria na área de pilhas de chamada, **sem executar nada na hora**;
      - o escalonador do IOP a roda junto com o resto do IOP (no worker) e `servicePendingServerCalls` faz o término:
        cópia da resposta → `completeRpc` → `completeAsyncIopRpc` na GameThread (semáforo, função de fim, cliente
        livre). É assim no PS2: o IOP atende em paralelo.
      - Se as 2 posições estiverem ocupadas, ou a chamada for aninhada, roda síncrona como antes.
    - **Dois bugs antigos que impediam qualquer RPC assíncrono** (inclusive o do memory card do §55):
      1. A área de pilhas de chamada do IOP (`0x1F0000`-`0x1FFF00`, 8 KB cada) tem **7 posições**, e a regra do
         `runServerFunction` exige mais de 4 pendentes + 4 aninhadas = 8: nunca havia vaga. O caminho assíncrono do
         §55 provavelmente nunca rodou. O adiamento usa regra própria: 2 posições, as mais baixas, deixando 5 para
         aninhamento (1-2 níveis na prática). A regra do §55 não foi mexida, para não mudar o memory card agora.
      2. `PS2Runtime::handleIopRpc` **sobrescrevia o `callToken`** com o token de escopo do host. O `SifCallRpc`
         registra a chamada pendente com o token dele, e o IOP devolvia o término com outro: o término se perdia, o
         cliente ficava ocupado para sempre e o jogo parava de mandar o comando de som (0,1 chamada/s). Agora o token de
         quem chama é mantido.
    - **Resultado:** a chamada continua 59,9/s e custa **6-9 µs** na GameThread (0,3% do EE, era 6,3%). A IOPThread
      sobe de ~43% para ~50%.
      - Áudio (`PS2X_AUDIO_STATS`): 0 underruns.
      - F5 durante a luta salva na hora; o state salvo carrega e roda a 59,9 com áudio normal e a chamada a 59,9/s.
      - Imagem conferida; o overlay mostra "Jogo 60 FPS | Tela 60 | Velocidade 100%".

      | fight8_heli (alternado) | quadros/s | distintas/s | 1× | velocidade | GameThread |
      |---|---|---|---|---|---|
      | antigo (`=0`) | 59,2 / 59,4 | 58,9 / 59,0 | 99% | 99% | 94-96% |
      | **adiado** | **59,9 / 59,9** | **59,6 / 59,7** | **100%** | **100%** | **89-90%** |

      - 4 núcleos/8 threads (0-7): 59,3 q/s, 58,9 distintas, 99%. fight4_heavy2 59,9 (GameThread 69%); select4 59,9.
    - **Critério de 60 liso no fight8_heli:** passam 3 dos 4 itens (≥ 59,5 distintas/s, ≥ 99% 1×, velocidade 100%).
      **Falta o pior quadro < 25 ms:** há 6-7 apresentações por janela de 34 s com intervalo de 26-33 ms (o present
      perdeu um vsync), mesmo com cada imagem mostrada uma vez só. Aparece também na select4 e na fight4_heavy2.
      Parece soluço do lado do host (present/DWM/driver), não do jogo. É o próximo alvo.
67. **Os soluços de present: vblank emulado travado no monitor + controle de taxa do áudio** (2026-10-11).
    - **O bench não mede o load:** `bench_state.py` carrega o state em ~10 s, espera 8 s e mede de 18 a 52 s do CSV.
      A queda logo depois do load fica fora da média e dos critérios (pergunta do usuário).
    - **Padrão dos soluços:** em cada um, o `SwapBuffers` fica preso ~17 ms (um vsync inteiro), o campo emulado avança
      2, e a imagem seguinte chega 0,6 ms depois. Antes, o tempo de swap vai subindo (4 → 7 → 11 ms): o momento do
      present escorrega até a borda do vsync.
    - **Causa:** batimento entre o relógio emulado e o monitor. Campos a 59,87/s (NTSC 59,94) contra um monitor de
      59,997 Hz. A cada ~6-8 s as bordas se cruzam, o present perde o vsync e depois tira o atraso. Vale para qualquer
      cena, inclusive o modo história.
    - O PCSX2 do usuário tem `VsyncEnable = false` (mostra na hora, com tearing possível) e
      `SyncToHostRefreshRate = false`.
    - **Dois monitores:** o usuário tem DISPLAY1 2560×1080 a 60 Hz (principal, onde o jogo abre) e DISPLAY2 2560×1080 a
      75 Hz. O `DwmGetCompositionTimingInfo` devolve o de 75 Hz, então não serve.
    - **Mudança 1, `PS2X_VSYNC_LOCK`** (padrão 1; `0` desliga):
      - `Kernel/host_vsync.cpp` (novo): a thread `VsyncThread` espera o vblank **do monitor onde a janela está**
        (`D3DKMTOpenAdapterFromHdc` + `D3DKMTWaitForVerticalBlankEvent`, carregados do gdi32), anota o horário e o
        período medido. Ela segue a janela se ela mudar de monitor (o runtime registra o HWND depois do `InitWindow`).
      - `EeScheduler::processDueDeadlines`: o prazo em tempo real do próximo vblank emulado passa por
        `lockToHostVsync`, que o puxa 1/8 do caminho (no máximo 0,25 ms por campo) até 1 ms depois de um vblank do
        monitor. Isso trava fase e frequência. **Os ciclos emulados não mudam**; só o tempo real por campo segue o
        monitor (o jogo fica 0,09% mais rápido). Só liga com o monitor a ±2% de 59,94 Hz; o log diz qual taxa achou.
      - `PS2X_VSYNC_LOCK_STATS=1`: erro de fase médio 0,12-0,2 ms, pior 0,15-0,6 ms em regime.
    - **Mudança 2, `PS2X_AUDIO_RATE_CONTROL`** (padrão 1; `0` desliga), em `ps2_audio.cpp`:
      - o callback toca até 0,5% mais rápido ou mais devagar, com interpolação linear, para manter a fila em ~85 ms
        (`kRateTargetSamples`, metade do anel de 170 ms);
      - motivo: com o jogo 0,09% mais rápido, a fila crescia ~1 ms/s e, cheia, perdia ~127 ms de uma vez (estalo);
        antes da trava acontecia o inverso, com underruns ocasionais;
      - com alvo de 43 ms houve 2 underruns em 70 s, porque a thread do IOP produz o áudio em rajadas. Com 85 ms:
        **0 underruns e 0 descartes em ~85 s**, fila entre 70 e 107 ms.
    - **Resultado** (alternado, mesma sessão; "soluços" = presents > 25 ms na janela de 34 s):

      | cena | trava desligada | **trava ligada** |
      |---|---|---|
      | fight8_heli | 6 e 6 soluços, pior 55,3 / 33,2 ms | **0 e 0**, pior **19,2 / 19,2 ms**, p99 18,0 |
      | fight4_heavy | 8, pior 33,4 | **0, 0 e 0** em três rodadas, pior 18,8-23,7 |
      | select4 | 6, pior 33,4 | **0**, pior 24,9 |

      - fight8_heli com a trava: **60,0 quadros/s, 59,8-59,9 distintas/s, 100% 1×, velocidade 100%, pior 19,2 ms:
        passa nos 4 itens do critério.**
      - 75 s seguidos (72 s de janela, 4.318 apresentações): um único present de 27,4 ms (atraso do lado do jogo, swap
        de 0,4 ms).
      - Uma rodada isolada do fight4_heavy teve 7 soluços concentrados entre 28 e 35 s; as três seguintes, 0. Foi
        evento externo.
      - 4 núcleos/8 threads: 59,0 q/s, 2 soluços.
68. **Teste do usuário + 2 núcleos** (2026-10-11).
    - **Retorno do usuário** (modo história e multiplayer, depois do §67):
      - save/load no memory card sem travar e sem a música parar; os pipocos de áudio sumiram;
      - sem quedas grandes no multiplayer;
      - os engasgos do modo história (troca de câmera etc.) **também acontecem no PCSX2**;
      - há engasgo nas trocas de tela de loading e um leve engasgo no fim da missão (tela piscando, efeito do jogo);
      - as texturas parecem pontilhadas (*dithering*), o que não acontece no PCSX2 dele (6×, bilinear, FXAA).
      Anotado no ROADMAP §9.
    - **2 núcleos com SMT (afinidade 0-3), fight8_heli:** 49,5 q/s, velocidade 83%, 48,7 distintas/s, **33-39
      soluços por 34 s**. O áudio tem 8-11 underruns a cada 5 s, porque o jogo a 83% produz menos som que o tempo real.
      O controle de taxa de ±0,5% não cobre isso; precisaria de *time-stretch*, como o do PCSX2.
    - **2 núcleos sem SMT (0,2):** 20 q/s (o jogo cai para 1/3 da taxa), o processo usa só 142% dos 200%. Fora da meta.
    - **Problema da política da A2 com 2 núcleos:** a GameThread fica sozinha no núcleo 1 com o irmão SMT livre; as
      outras cinco threads (~225% de um núcleo) ficam em 2 threads lógicas, com máscaras rígidas no GS e na janela. Com
      `PS2X_THREAD_POLICY=off`: mesmos 49,4 q/s, mas só 4 soluços.
    - **Mudança** (`src/urban/thread_policy.cpp`): com **menos de 4 núcleos** na máscara do processo, nenhuma
      máscara rígida (só processador ideal) e o irmão SMT da GameThread entra na distribuição. Com 4 núcleos ou mais,
      nada muda. O log diz `hard masks: none (fewer than 4 cores)`.
    - **Resultado em 2 núcleos/4 threads:** 49,9 / 50,1 q/s, 49,2 / 49,3 distintas/s, **2 e 3 soluços** (eram 33-39),
      pior 25,6 / 27,6 ms (era 37), p99 23,7 ms (era 29,5). A velocidade continua em 83-84%: a GameThread (95%) limita.
    - Seis núcleos, sem mudança: 60,0 q/s, 100%, 59,8 distintas/s, 1 soluço de 33 ms nesta rodada (0 nas anteriores).
69. **Time-stretch do áudio, como o do PCSX2** (2026-10-11).
    - **O usuário ouviu as execuções de 2 núcleos:** a ~50 q/s e 83% de velocidade o som cortava muito; na de 2
      núcleos sem SMT, a 20 q/s e 100%, ficou bom. Quem manda é a **velocidade**: o SPU2 emulado gera som no tempo
      emulado, e a 83% faltam 17% para a placa. Ele pediu o mesmo comportamento do PCSX2.
    - **Mudança** (`ps2_audio.cpp`, `PS2X_AUDIO_STRETCH`, padrão 1; `0` desliga):
      - classe `TimeStretch` (WSOLA, o método do TDStretch do SoundTouch): peças de 40 ms, crossfade de 8 ms e busca
        de 15 ms pela melhor emenda (correlação cruzada normalizada). O som estica no tempo sem mudar o tom.
      - Fica no `submitPcm`, do lado de quem produz. A velocidade é medida pela produção de áudio em janelas de 0,5 s
        (suavizada; janelas quase sem áudio, como loading ou pausa, são ignoradas).
      - Liga abaixo de 97% e desliga acima de 99% com a fila de volta a ±25% do alvo de 85 ms, contando o áudio que o
        stretch ainda segura. O tempo do stretch é a velocidade, corrigida em até ±10% pela fila. A 100% o caminho é o
        de antes; o controle fino de ±0,5% do §67 continua no callback.
    - **Resultado em 2 núcleos/4 threads** (fight8_heli, 83-85%): produção de ~39-42 mil amostras/s contra 48 mil
      pedidas, **0 underruns e 0 quadros faltando** em todas as janelas (antes, 8-11 underruns e ~16-21 mil quadros
      faltando a cada 5 s), fila entre 80 e 127 ms. O áudio de saída não foi avaliado de ouvido aqui; fica para o
      usuário.
    - **Seis núcleos:** o stretch só liga por ~2 s logo depois do load do state (a produção cai durante a carga) e
      desliga com a fila em ~105 ms. Depois, 0 underruns; 60,0 q/s, 0 soluços.
    - A primeira versão desligava cedo demais, com a fila em 140 ms (o controle fino levaria minutos para drenar), e
      depois devolvia de uma vez os ~50 ms que segurava. Corrigido com a condição de saída e a contagem do áudio
      segurado.
70. **ROADMAP C4 — memmove** (2026-10-11).
    - **Censo** (profiler nativo com pilhas; `URBAN_NATIVE_PROFILE_WALK=<thread>` para as outras threads), fight8_heli:
      - **GameThread 3,9%:** 2,6% no `writeIORegister` (kick de DMA: a cadeia é copiada da RDRAM para o
        `chainBuf`), 0,8% nos `memset` do caminho enxuto do VU0 (§62), 0,3% no DMA do scratchpad. A cópia do kick é a
        obrigatória: uma só, e o buffer é *movido* para o worker, sem segunda cópia.
      - **GIFThread 6,3%:** `GSThreadedBackend::Enqueue` a cada primitiva (`vertexKick`), com 2-3 `vector::insert` de
        tamanho variável por lote, cada um uma chamada de `memmove` de poucas dezenas de bytes.
      - **GSThread 7,2%:** `addState`, `newPrim`, `flushBatch`/`bindUpload`, `uploadImage`. É a estrutura do back end
        do GS e fica para a fase do GS.
      - **VUThread 4,3%:** cópias de pacotes GIF do XGKICK. Há uma cópia dupla (`GifArbiter::submit` →
        `asyncvif::submitGif`) só para pacotes que precisam de arbitragem entre PATH2/PATH3 (~0,8% da VUThread); não
        mexido, porque exigiria segurar ponteiros para dados que podem mudar antes da drenagem. Também o
        `resetScheduler` do VU1 (0,5%).
    - **Mudanças:**
      - `GSThreadedBackend::Enqueue`: o registro de cada lote (cabeçalho + estado + vértices) é montado num buffer
        local de tamanho fixo (cópias de tamanho conhecido, inline) e entra com um `insert` só. Os bytes enviados são
        os mesmos.
      - `executeVu0Lean`: em vez de três `memset`, limpa só `m_fdiv.valid`, `m_efu[i].valid` e `m_flagValid`, que
        são tudo o que é lido (conferido no runtime e no código AOT gerado).
    - **Resultado:** `memmove` na GIFThread 6,3% → 4,5%; CPU da GIFThread ~44% → 36-42%; processo ~321-327% →
      309-321%. GameThread sem mudança visível (~89%). 6 núcleos: 60,0 q/s, 100%, 0-1 soluço. 2 núcleos/4 threads:
      50,8 q/s, 85% (antes 49,9-50,7). Imagem conferida.
    - **Conclusão:** quase não há `memmove` evitável na GameThread. O que pesa está no GS (fase do GS) e na contagem
      de ciclos (C3b).
71. **ROADMAP C3b, versão leve: contagem de ciclos adiada no `checkpointDue`** (2026-10-11).
    - **Ideia:** o roadmap previa gerar o desconto de ciclos dentro do código gerado (regenerar + build completo). O
      perfil mostrou que a chamada `eeCheckpointDue` em si custa ~1%; o caro é o que vem depois a cada laço
      (`checkpointDue` + `accountCycles`, ~7%). Então o adiamento foi feito **dentro do `EeScheduler::checkpointDue`**
      (membro público definido no `.cpp`): sem mexer em header do PCH, sem regenerar e com build rápido.
    - **Como** (`PS2X_EE_DEFER_CYCLES`, padrão 256; `0` = antigo):
      - **caminho rápido:** se `ciclo + adiados + custo < g_deferUntil`, sem evento pendente nem pedido de parada (duas
        leituras atômicas), só soma em `g_deferred` e retorna;
      - **caminho completo:** contabiliza tudo (`accountCycles` agora inclui `g_deferred`), faz as checagens de antes
        (o corpo antigo virou uma lambda) e recalcula `g_deferUntil` = o mais próximo entre 256 ciclos, o próximo
        prazo, o fim da fatia de tempo, a checagem de ritmo e o próximo evento de timer. Nenhum prazo nem timer é
        atrasado, e eventos de outras threads são vistos na hora;
      - **leituras de tempo exatas:** `readIORegister`, `writeIORegister` e `handleSyscall` chamam primeiro
        `ps2xEeFlushDeferredCycles()`, que contabiliza os adiados (só na thread do EE). O jogo lê os timers no mesmo
        ciclo de antes;
      - o total de ciclos cobrados não muda; depois de carregar um savestate, os adiados são zerados.
    - **Resultado** (alternado, mesma sessão):

      | | desligado (`=0`) | **adiado** |
      |---|---|---|
      | fight8_heli, 2 núcleos/4 threads | 49,4 / 48,9 q/s, 81-82% | **51,8 / 51,4 q/s, 86-87%** |
      | fight8_heli, 6 núcleos: GameThread a 60 q/s | 92,6% | **84,9%** |

      - 6 núcleos: 60,0 q/s, 59,9 distintas/s, 100% 1×, 0 soluços, pior 19,4 ms.
      - fight4_heavy2: 60,0 q/s, GameThread 66% (era ~69%); fight4_heavy: 60,0, 58,5%; select4: 60,0, 45% (era ~50%).
        Áudio sem cortes em todas.
      - Recarga do state duas vezes no meio da luta: 59,9 q/s, áudio normal. Imagens conferidas.
    - A C3b "completa" (contador inline no código gerado) tiraria só o ~1% restante da chamada: fica fora do plano
      por enquanto.

72. **Resolução interna (2026-10-11).** Configurável no `config.ini` (`[Video] ResolucaoInterna=1..8`, padrão 1) ou
    por `PS2X_GS_SCALE`; a variável de ambiente tem prioridade.
    - **Como funciona** (estilo paraLLEl-GS, supersampling): o buffer da VRAM no GPU vira `escala²` *planos de
      amostra* de 8 MB (4 MB de VRAM + 4 MB de páginas-sombra). O plano `(i, j)` é a memória inteira do GS desenhada
      com o ponto de amostra deslocado `(i, j)/escala` de pixel; o **plano 0 é a imagem nativa**, a que o jogo lê de
      volta, então nada muda para o jogo.
      - **raster:** triângulos testam as arestas em cada subamostra (posição em 1/16 de pixel, regra top-left igual),
        e cor, Z, UV e fog são interpolados nela; o raster por hardware usa um framebuffer `2048×escala` e o compute
        despacha um grupo por plano. **Sprites cobrem pixels inteiros como no 1x** (só a textura é lida na posição da
        amostra): o jogo desenha passes de tela cheia em faixas de 64 px que se sobrepõem em meio pixel
        (`[0,64)` e `[63,5;127,5)`), o que no 1x não aparece; com bordas por amostra a sobreposição era misturada duas
        vezes e virava uma linha clara a cada 64 px no heliporto;
      - **uploads, cópias, clears e escritas avulsas** escrevem todos os planos; leituras do jogo (`localToHost`,
        CLUT, savestate) usam o plano 0;
      - **textura de conteúdo desenhado:** o CPU marca por página de 8 KB o que foi desenhado (`hiresPages`; upload
        desmarca, cópia herda da origem, páginas-sombra herdam da original) e manda os bits a cada flush. Bilinear
        numa página desenhada usa a grade de alta resolução com a mesma convenção de meio texel do nativo
        (`(u − 0,5)·escala`), então uma cópia 1:1 com o meio texel de sempre lê cada amostra exata. Nearest em sprite
        usa o texel que o pixel nativo leria, no plano da própria amostra. Texturas enviadas pelo jogo têm o mesmo
        conteúdo em todos os planos e são lidas como no 1x;
      - **present:** sai `escala×` maior; a janela redimensiona.
    - **Cache de shaders:** em 1x os blocos `#if HIRES` são removidos como texto e o shader sai **byte a byte igual
      ao de antes** (conferido), então o cache já montado continua valendo. Acima de 1x a escala é lida de um buffer
      (binding 12): **2x a 8x compartilham os mesmos shaders**, e trocar a escala não recompila nada.
      O cache do modo alto se monta jogando (cada variante leva ~4-9 s no driver da NVIDIA, igual ao 1x com cache
      vazio; ~40-50 variantes por cena).
    - **Dois pontos síncronos que também pesavam no 1x:**
      - o quadro ia para a textura compartilhada por `glTexSubImage2D` de um pixel buffer, que o driver da NVIDIA
        faz esperando o GPU (~3 ms por quadro em 1x, ~7 ms em 2x). Agora um compute grava direto na textura com
        `imageStore` (`kPresentBlit`; `PS2X_GS_PRESENT_BLIT=0` volta ao antigo);
      - a leitura assíncrona do bloco 8×8 do flare (§50) ainda esperava na GSThread: o `localToHost` fazia
        `glGetBufferSubData`, e um `glCopyBufferSubData` para buffer mapeado também era síncrono (8-9 ms). Agora o
        shader de cópia grava direto num buffer mapeado persistente e o resultado é colhido por fence no quadro
        seguinte (`GSGpuBackend::BeginDownloadAsync`; `PS2X_GS_ASYNC_DOWNLOAD=0` volta ao antigo).
    - **Resultado** (6 núcleos, cache quente):

      | cena | 1x | 2x | 3x | 4x |
      |---|---|---|---|---|
      | fight8_heli | **60,0**, 59,8 distintas, GSThread **52,6%** (era ~80%), 0 soluços | ~42-45 apres./s, ~33 distintas | — | — |
      | fight4_heavy2 | 60,0, 0 soluços | 57,6, 50 distintas, ~30 picos | — | — |
      | select4 | 60,0 | **60,0**, 0 soluços | **60,0**, 1 soluço | 36,5, 20 distintas |

      - Imagens conferidas em 2x e 3x com a janela maximizada: nítidas, sem linhas e sem erros visíveis.
      - Em 2x com 8 lutadores o limite é o GPU (3D ~52% ocupado pelo contador do Windows, flushes de dezenas de ms
        dentro do driver em momentos pesados): o nosso GS emula tudo por amostra num fragment shader com interlock,
        bem mais caro que o renderizador nativo do PCSX2. Otimizar isso é a fase GS (depois da B1).
    - **Outros:** `config.ini` novo (criado com os padrões se faltar; aceita também nomes `PS2X_*`/`URBAN_*`
      direto), `[Video] Maximizada` (`URBAN_START_MAXIMIZED`), `PS2X_GS_HW_COMPILE_THREADS` (threads do compilador
      de variantes; mais threads não acelerou no driver atual), `PS2X_GS_HIRES_TEXTURES=0` (diagnóstico: textura
      desenhada lida só do plano 0).
    - **A acompanhar:** duas travadas no boot logo depois de `hardware rasterization active` (GSThread a 100%),
      uma em 1x e outra em 2x, não reproduzidas em 9 boots seguidos. Pode ser anterior a esta mudança.

73. **ROADMAP B1 — tempo de GPU por passe, e os dois desperdícios que ele mostrou (2026-10-11).**
    - **Ferramenta** (`PS2X_GS_GPU_PROFILE=1`; `PS2X_GS_GPU_PROF=1` continua valendo): um par de timestamps em volta
      de cada passe (raster por hardware/compute, CLUT, upload, cópias, page copy, clear, present, publicação). Os
      resultados só são lidos quando o GPU já terminou o quadro (disponibilidade consultada a cada present), então a
      medição não trava o renderizador. A cada 120 quadros: GPU ocupado por quadro (média, p50, p95, máx; união dos
      intervalos), ociosidade dentro do quadro, tempo e quantidade de cada passe, o quadro mais pesado passe a passe,
      raster por alvo (fbp/flags), motivos dos flushes e, para uploads, o motivo de cada dispatch.
    - **Achado 1: ~280 passes de 1 sprite por quadro.** O jogo copia a imagem da cena (`tbp 0`, CT24, 1024 de
      largura) para um buffer de 16 bits que fica na parte não usada à direita do mesmo buffer (`fbp 0x8c`), em faixas
      de 64×16, com `REGION_CLAMP` em 640×448. O conjunto de páginas lidas era a textura inteira (1024×512), que
      cobre o destino: cada faixa parecia ler o que a anterior escreveu e fazia flush. Agora, com `REGION_CLAMP`, só
      o retângulo do clamp conta (`PS2X_GS_REGION_READ=0` volta ao antigo). Passes de raster: ~340 → ~35 por quadro.
    - **Achado 2: ~320 dispatches de upload por quadro.** A luta manda ~360 texturas e paletas pequenas por quadro
      (2,7 MB); elas ficam lado a lado na mesma página de 8 KB, e um upload numa página com outro pendente forçava a
      execução. Agora os pendentes são marcados também por bloco de 256 bytes (`GSSwizzle::Locate` num ponto a cada
      8 pixels; retângulos grandes caem para página), e só blocos realmente em comum forçam
      (`PS2X_GS_UPLOAD_BLOCKS=0` volta ao antigo). Dispatches de upload: 342 → 21 por quadro.
    - **GPU por quadro (perfil):**

      | cena | antes | depois |
      |---|---|---|
      | fight4_heavy2 1x | 14,8 ms (raster 9,8, upload 4,3) | 11,1 ms (só com o achado 1) |
      | fight8_heli 1x | 13,5 ms (raster 8,6, upload 4,1) | **9,5 ms** (raster 6,0, upload 1,5) |
      | fight8_heli 2x | 20,7 ms (raster 17,1) | **14,7 ms** (raster 13,5, upload 0,7) |

      O limiar da B1 (8-10 ms em 1x) agora é atendido; o que sobra em 2x+ é o raster da cena (`fbp 0`, flags 4f:
      triângulos texturizados com Z e blend), proporcional aos pixels.
    - **Critério (sem o perfil, 6 núcleos):**

      | cena | q/s | distintas/s | 1× | pior quadro |
      |---|---|---|---|---|
      | fight8_heli 1x | 60,0 | 59,9 | 100% | 19,5 ms, 0 soluços |
      | fight8_heli 2x | 59,7 | 58,4 | 97% | 52 ms, 4 soluços (era 42 q/s, 33 distintas) |
      | fight4_heavy2 2x | 60,0 | 59,4 | 99% | 30,8 ms, 1 soluço (era 52 distintas) |
      | fight8_heli 3x | 35,8 | 27,1 | 49% | limitado pelo raster |

      GSThread em 1x na luta de 8: 48%. Imagens conferidas em 1x e 2x.

74. **Folga de CPU, parte 1: de onde vem o custo do IOP e da GameThread (2026-10-11).** Luta de 8, 1x.
    - `PS2X_IOP_PC_HIST=<s>` agora também soma as amostras por módulo IRX e por thread do IOP.
    - **IOP, instruções interpretadas por módulo:** CRI_ADXI ~66% (quase tudo na thread 75, o servidor do ADX, acordado
      por semáforo; não é laço ocioso, e o mais quente são os seus `CpuSuspendIntr`/`CpuResumeIntr`), NUSOUND ~17%,
      PADMAN ~10%, LIBSD ~3%. O HLE do NUSOUND (C7 como estava no roadmap) tiraria só ~17% do interpretador.
    - **IOPThread (~46% de um núcleo):** ~50% das amostras dormindo, trabalho real 15-30% ("worker busy" do
      `PS2X_IOP_THREAD_STATS`), o resto é a espera ativa antes de dormir (A1). `PS2X_IOP_WORKER_SPIN_US` 20 → 5 → 0:
      IOPThread 46,4 → 40,5 → 38,7%, sem mudar os q/s (59,2-59,8, distintas 59,1-59,6). Fica em 20 por enquanto.
    - **GameThread (~86%):** folga (espera do tempo real) 12,8%; VU0 18,6% inclusivo (código do microprograma 7,8%,
      preparação por chamada ~4%: `loadVu0Registers` 2,1% + `executeVU0Microprogram` 1,8%); contabilidade do
      escalonador ~10% (`checkpointDue` 4,5%, `dispatchGuestBranch` 2,8%, `eeCheckpointDue`, `accountCycles`,
      `pushDispatchPc`); memmove 4,4%; decodificador de animação `sub_00292670` 3,7%; função de quadro 3,7%.

75. **Folga de CPU, parte 2: espera do IOP e preparação do VU0 (2026-10-11).**
    - **Espera ativa do worker do IOP: padrão 20 → 5 µs** (`PS2X_IOP_WORKER_SPIN_US=20` volta ao antigo).
    - **VU0 por chamada:** as configurações de `executeVU0Microprogram` e o modo `check` do `loadVu0Registers`/
      `executeVu0Lean` viraram statics simples lidos uma vez (antes, `static const` com lambda = checagem de
      inicialização thread-safe em cada uma das ~4500 chamadas por quadro). Refatoração sem chave (mesmo comportamento).
    - **Resultado** (luta de 8, 1x, alternado, 2 rodadas):

      | | 6 núcleos: IOPThread | 6 núcleos: GameThread | 2 núcleos/4 threads: q/s | 2 núcleos: IOPThread |
      |---|---|---|---|---|
      | espera 20 µs | 48,0-48,6% | 86,9-89,1% | 53,0 / 53,2 | 47,1-48,6% |
      | **espera 5 µs** | **40,8-42,3%** | 87,6-88,4% | 53,0 / 52,7 | **40,8-42,7%** |

      - IOPThread −6 a 7 pontos de um núcleo; q/s iguais (6 núcleos: 59,8-59,9, distintas 59,4-59,7, 99% 1×).
      - A preparação do VU0 ficou dentro do ruído da GameThread (±1,5 ponto): as guardas pesavam pouco.
      - 2 núcleos: 53 q/s (era ~51,6 no §71), efeito das correções da B1 na GSThread; continua limitado pela GameThread
        (~96%).

76. **C3b "completa": testada inline e desfeita; o teste de orçamento ficou fora do código gerado (2026-10-11).**
    - **Tentativa:** `eeCheckpointDue` virou função inline no `ps2_runtime.h` (PCH): cada desvio para trás do código
      gerado só somava os ciclos e comparava com um orçamento (`g_ps2xEeDeferBudget` = `g_deferUntil - m_eeCycle`,
      no máximo 256 ciclos; zerado por `accountCycles`, `postEvent`, `requestStop` e pelo reset do savestate), sem
      regenerar o código. Build completo (~35 min).
    - **Resultado ruim:** o executável cresceu ~540 KB (a checagem copiada em dezenas de milhares de laços) e a
      GameThread ficou ~4 pontos mais pesada em tudo, inclusive com o orçamento desligado: select4 45 → 49%, luta de 8
      87-89 → 91-95%, 58,1-59,1 q/s. O inline economizava só 1,5-3 pontos sobre ele mesmo desligado.
    - **Ficou assim:** o `ps2_runtime.h` voltou ao original; o mesmo teste de orçamento roda no começo do
      `PS2Runtime::eeCheckpointDue` (chamada normal), antes de chamar `checkpointDue`.
      `PS2X_EE_INLINE_CHECKPOINT=0` mantém o orçamento em 0 (sempre chama o escalonador, como na C3b leve).
    - **Resultado** (depois do rebuild; a primeira rodada após o build teve 54,9 q/s e 20 picos, descartada):

      | | luta de 8, 6 núcleos | 2 núcleos/4 threads | select4 |
      |---|---|---|---|
      | orçamento ligado | 59,7-59,9 q/s, 59,4-59,6 distintas, 99% 1×, GameThread 87,1-89,6% | **54,6 q/s** (91%) | 59,9, GameThread 44,2% |
      | orçamento desligado | 59,5-59,7 q/s, 59,1-59,4 distintas, GameThread 90,3-91,7% | 53,7 q/s (90%) | — |

      2 núcleos: melhor resultado até agora (era 53,0 no §75, 51,6 no §71).
    - Lição: em código gerado gigante, inline de qualquer coisa nos laços custa mais em cache de instruções do que
      economiza na chamada.

77. **C8, parte 1: o gerador de VU só grava os tempos de "pronto" que ainda importam (2026-10-11).**
    - `vu1_recomp` (`readyStores`): no fim de cada segmento, só os registradores escritos nas últimas
      `kMaxVfLatency`/`kMaxViLatency`/`kMaxAccLatency` posições têm o tempo de pronto gravado (`m_vfReady`/`m_viReady`/
      `m_accReady`). Uma escrita anterior já está pronta no fim do segmento, e o valor antigo que a gravação trocaria
      (de antes do segmento, pelo menos essa latência mais cedo) também; toda checagem de parada seguinte lê o mesmo.
      Vale também para os laços que voltam para o próprio segmento.
    - Gravações no código gerado: **3798 → 653**. `VU1_RECOMP_ALL_READY=1` gera o código anterior byte a byte
      (conferido com `diff`). Regenerado de `build/vu_capture_all` (VU1 + as duas imagens do VU0); backup do anterior em
      `build/vu1_generated_backup_c8`.
    - `PS2X_VU1_VERIFY=obs` + `PS2X_VU0_VERIFY=obs` na luta de 8: **0 divergências** (~410 mil execuções de VU0, ~33 mil
      de VU1; a comparação inclui os ciclos, então as paradas são as mesmas).
    - **Resultado** (luta de 8, 1x, executáveis antigo e novo alternados; a 1ª rodada de cada um, logo depois do build,
      teve travadas de 1,4-1,9 s e foi descartada):

      | | VUThread, 6 núcleos (3 rodadas) | q/s, 6 núcleos | 2 núcleos/4 threads |
      |---|---|---|---|
      | antes | 53,4-56,7% | 59,7-59,9 | 54,2 q/s, 53,3 distintas, VUThread 68,2% |
      | **depois** | **48,3-49,9%** | 59,6-59,9 | **55,6 q/s, 54,5 distintas**, VUThread 64,8% |

      ~6 pontos a menos na VUThread (~11% do trabalho dela); 2 núcleos com o melhor resultado até agora.

78. **Itens menores da GameThread: medidos, dois descartados (2026-10-11).** Luta de 8, 1x.
    - **Cache do decodificador de animação (`URBAN_ANIM_CACHE=1` + `URBAN_ANIM_VERIFY=1`):** na luta só **10%** das
      chamadas acertam o cache (62 mil de 623 mil, 36 mil entradas) e **1,8% dos acertos dão resultado diferente**
      (1141 divergências). O decodificador custa ~3,2 µs por chamada, ~18 mil chamadas/s (~5,8% de um núcleo).
      Continua desligado; descartado.
    - **memmove (2,9% da GameThread):** 2,6% vêm de `writeIORegister` (o disparo de DMA copia o pacote para a fila
      da thread do VU, porque o jogo pode reescrever a memória logo depois; mesma conclusão da C4, §70). Descartado.
    - **VU0 (20,8% da GameThread):** código do microprograma 8,6%; `loadVu0Registers` 2,7%;
      `executeVU0Microprogram` 2,5% (cópia do estado de volta, preparação); cadeia `executeVu0Lean` → `run` →
      `runLoopWith` → `runFast` ~2,5%; `foldStatus` ~1%; FMAC exata ~1,4%. O custo fixo por chamada (~9%, ~4500
      chamadas por quadro) pesa mais que o código. Reduzir de verdade é o C6 completo: o VU0 trabalhar direto sobre os
      registradores do EE (`ctx->vu0_vf`) e entrar direto no bloco compilado, sem a cadeia de execução do interpretador.

79. **C6 completo (registradores do VU0 por máscara): implementado, medido, sem ganho, desfeito (2026-10-11).**
    - **O que foi feito:** análise por entrada de microprograma (todo par alcançável: fall-through, os dois lados de
      cada desvio, só o delay slot depois de um E bit; JR/JALR = desconhecido), em cache até o código do VU0 mudar
      (`getVU0CodeGeneration`). Carregava só os registradores lidos ou escritos (um escrito em parte precisa chegar
      inteiro) e copiava de volta só os escritos. Modo de conferência (`=check`: carregar tudo, copiar mascarado e
      acusar qualquer registrador fora da máscara que mudou): **0 em ~25,7 milhões de chamadas**.
    - **Máscaras reais na luta:** 5 entradas; carga 6-17 registradores (era 33), escrita 2-8 (era 32).
    - **Resultado: nada.** Luta de 8, 3 rodadas alternadas: GameThread 87,7-89,5% com máscara, 85,9-88,8% sem.
      Perfil: `loadVu0Registers` 3,4 → 2,6%, mas a busca da análise custava 1,1% (hash) / 0,6% (tabela direta) e o
      VU0 inteiro ficou em 20,7-21,5% contra 19,6-20,8%. O custo da carga é normalizar os registradores de entrada (o
      EE muda os de entrada a cada chamada) e os acessos à memória, não a quantidade de cópias.
    - Desfeito (o código voltou ao §75). O que resta no VU0 é o próprio microprograma (~8,5%) e a cadeia de chamadas
      até ele (~2,5%), que só mudariam com o VU0 trabalhando sobre os registradores do EE no código gerado do EE.

80. **Troca do GS: estudo de viabilidade (2026-10-11).** Ver `docs/GS_PCSX2.md`.
    - Fonte do PCSX2 em `H:\JOGOS\pcsx2-ref` (clone raso, GPL-3). O GS é `pcsx2/GS/` (~88 mil linhas), com a API do antigo
      plugin GSdx (`GSopen`, `GSgifTransfer`, `GSvsync`, `GSwriteCSR`, `GSInitAndReadFIFO`, `GSfreeze`, `GSUpdateConfig`).
    - SIGNAL/FINISH/LABEL não são do renderizador (handlers nulos no `GSState`; o núcleo do PCSX2 trata): a nossa lógica de
      GIF/MTVU fica, e o frontend só repassa os qwords crus.
    - Registradores privilegiados: um bloco de 8 KB no layout do PS2 (`basemem`). Shaders lidos de
      `bin/resources/shaders` e compilados em tempo de execução (variantes pequenas, ms cada).
    - Dependências: `common/`, `Config.h` (`GSOptions`), funções de host/VM (o `pcsx2-gsrunner` mostra quais), ImGui e
      terceiros (fmt, glad, vulkan, d3d12memalloc, xbyak, lzma...). C++20, MSVC v143.
    - O GameIndex do PCSX2 tem correções para o `SLUS-21209`: `alignSprite` (linhas verticais), `halfPixelOffset: 4`
      (bloom), `textureInsideRT`, `nativeScaling: 2`, `getSkipCount: GSC_UrbanReign` — os dois primeiros são os
      problemas que achei no nosso upscale (§72).
    - Fases: (1) biblioteca `pcsx2_gs` só com DX11 + funções de host nossas; (2) ligação no runtime (GIF, registradores,
      vsync, leituras, janela-filha); (3) desempenho e escala; (4) savestate, `config.ini`, shaders; (5) widescreen,
      texturas HD, Vulkan/DX12.

81. **Troca do GS, fase 1: biblioteca `pcsx2_gs` (2026-10-11).** Critério da fase (`docs/GS_PCSX2.md`): linka e abre o
    device numa janela de teste — passou.
    - `thirdparty/pcsx2-gs/CMakeLists.txt` (projeto próprio; ainda lê o PCSX2 de `H:\JOGOS\pcsx2-ref` e os
      DirectX-Headers de `H:\JOGOS\DirectX-Headers`; os arquivos usados entram no repositório depois). Alvos:
      `pcsx2_fmt`, `pcsx2_imgui`, `pcsx2_d3d12ma`, `cpuinfo`, `pcsx2_common` (parte do `common/`), `pcsx2_gs` (GS
      inteiro + DX11/DX12, sem dumps, captura de vídeo, PNG e troca de texturas), `pcsx2_gs_host` e `pcsx2_gs_test`.
      Flags do PCSX2 (C++20, `/Zc:preprocessor`, `/permissive-`, `/utf-8`, `__SSE4_1__`).
    - `compat/HostStubs.cpp`: as funções de host/VM que o GS chama (janela, pastas, `EmuConfig`, `MTGS` direto,
      `VMManager` com o serial `SLUS-21209`, OSD/dumps/captura vazios) e um ImGui mínimo (contexto e frame, sem OSD).
      `compat/Pcsx2ConfigGS.cpp` é gerado por `tools/extract_config.py` a partir do `Pcsx2Config.cpp` (construtores das
      opções e tabelas de nomes). `compat/pcsx2gs_host.h`: `SetRenderWindow`, `SetFolders`, `SetVerticalFrequency`.
    - `compat/pcsx2gs_imconfig.h`: o `imconfig.h` do PCSX2 liga o FreeType (não compilado aqui); usamos o stb_truetype do
      próprio ImGui. `dxcompiler.dll` (só do DX12) é carregada com atraso.
    - `test/GSTest.cpp`: janela 1280x896, registradores privilegiados da tela 480p do Urban, `GSopen(DX11)`, um pacote
      GIF A+D (fundo azul, sprites vermelho e verde) e `GSgifTransfer` + `GSvsync` em laço. Resultado: imagem certa em
      2x (resolução interna 1280x896) e 4x (2560x1792), saída limpa (código 0), cache de shaders em `gs_cache/`
      (`d3d_shaders_sm50.bin`, ~120 KB).
    - Compilar: `cmake -S thirdparty/pcsx2-gs -B build/pcsx2gs` e `cmake --build build/pcsx2gs --target pcsx2_gs_test -j 8`
      (no ambiente do `tools/win/vsenv.cmd`). Rodar: `build/pcsx2gs/pcsx2_gs_test.exe [escala] [segundos]`.
    - O `urban.exe` não muda nesta fase.

82. **Troca do GS, fase 2: GS do PCSX2 ligado no runtime (2026-10-11).** `PS2X_GS_RENDERER=pcsx2` (ou `config.ini`
    `[Video] Renderizador=pcsx2`; o padrão continua `nativo`). Critério da fase: boot, menus e luta desenhando certo em
    1x — passou.
    - **`pcsx2gs.dll`** (`thirdparty/pcsx2-gs`, alvo `pcsx2gs`, compilado junto com o `urban` pelo CMake de cima,
      `URBAN_PCSX2_GS=ON`): o GS inteiro do PCSX2 atrás de uma API C pequena (`include/pcsx2gs_api.h`, `compat/Bridge.cpp`):
      `open`, `gif`, `vsync`, `read_fifo`, `resize`, `set_scale`, `load_vram`, `save_vram`, `snapshot`. DLL porque o
      `urban.exe` já tem o seu ImGui (depurador F1) em outra versão: os símbolos do ImGui/fmt do PCSX2 ficam dentro da
      DLL. O `urban.exe` carrega a DLL só quando pedida (`LoadLibrary`); sem ela, volta ao nosso GS com aviso no log.
      Os shaders do PCSX2 são copiados para `bin/resources/shaders` no build; o cache deles fica em `bin/shadercache_pcsx2`.
    - **Correções do GameIndex fixas** (`SLUS-21209`): `alignSprite`, `textureInsideRT 1`, `halfPixelOffset 4`
      (Native), `nativeScaling 2`, `getSkipCount GSC_UrbanReign`.
    - **Runtime:** `GSPcsx2Backend` (`gs_pcsx2_backend.cpp`) é o back end dentro do `GSThreadedBackend`; a GSThread é
      a thread do GS do PCSX2 (todas as chamadas da DLL nela). Ops novas na fila: `RawGif`, `VSync`, `GsReset`.
      `GSRasterBackend` ganhou `WantsRawGif/RawGif/VSync/GsReset` (vazios nos back ends antigos).
    - **Frontend do GS** (`gs_frontend.cpp`, `m_rawGif`): continua lendo o GIF (SIGNAL/FINISH/LABEL, transferências,
      CSR), mas não manda desenhos, CLUT, imagens nem TEXFLUSH; repassa ao back end exatamente os bytes que consumiu, na
      ordem. No stall do SIGNAL o resto do pacote fica cru na fila e o tag continua por uma continuação
      (`g_gifCont`), em vez de re-taggear: o PCSX2 recebe o fluxo original. `uploadImageNative` e `writeRegister`
      (caminhos que não passam pelo GIF) viram pacotes GIF.
    - **Vsync:** `GS::vblank()` no início do vblank do EE (`EeScheduler`, antes dos handlers do jogo), com cópia dos
      registradores privilegiados (PMODE...BGCOLOR, CSR, IMR, BUSDIR, SIGLBLID), como o `PostVsyncStart` do MTGS; o
      campo vem do CSR.FIELD. A fila é publicada no vsync.
    - **Leitura GS → EE** (flare do sol): `GSInitAndReadFIFO` no tamanho da transferência (TRXREG × bpp). Por ora
      síncrona: o cache assíncrono do §50/§64 fica desligado com o GS do PCSX2 (ele lê a transferência que o próprio
      fluxo GIF montou). Religado no §83.
    - **Janela:** janela-filha (`WS_CHILD`, `HTTRANSPARENT` para o mouse passar para a do jogo) criada na thread
      principal sobre a área cliente da janela do raylib e redimensionada a cada quadro; o GS apresenta nela com DX11
      em mailbox (`PS2X_PCSX2_VSYNC`), 4:3 com barras (`PS2X_PCSX2_STRETCH=1` estica). O laço principal deixa de
      enviar a imagem e só cuida de janela, entrada e ritmo. O device abre na primeira operação da GSThread (não no
      `Initialize`), com a thread principal livre para as mensagens da janela.
    - **Savestate:** `load_vram` (reset de hardware do GS + memória local, como o `Defrost`) e `save_vram`
      (`ReadbackTextureCache` + memória local). Estados do nosso GS carregam no do PCSX2 e vice-versa.
    - **Testes (1x salvo indicação):**
      - boot sem estado: logo da Namco e depois preto, igual ao nosso GS (o FMV de abertura é o bug conhecido de FMV);
      - seleção de personagens (`select4`): igual ao nosso GS, 60 fps;
      - luta de 4 (`fight4_heavy2`): imagem certa, 59.9 fps;
      - savestate F5/F9 no modo PCSX2 e estado do PCSX2 carregado no nosso GS: certos;
      - fight8 (`b.sh`, mesmo PC, nosso GS na mesma hora: 59.9 fps, GameThread 88 / VU 53 / GS 48 / GIF 43 / IOP 43%):

        | GS do PCSX2 | fps | GameThread | VUThread | IOPThread | GSThread | GIFThread | driver (sem nome) |
        |---|---|---|---|---|---|---|---|
        | 1x | 54.7 | 88.5% | 52.5% | 40.2% | 29.5% | 16.4% | 10.9% |
        | 2x | 56.9 | 84.7% | 50.0% | 39.1% | 28.0% | 16.4% | 14.1% |
        | 4x (2560x1792) | 56.3 | 81.6% | 46.8% | 38.7% | 29.2% | 16.0% | 17.5% |

        A GSThread fica em ~29% até em 4x (no nosso GS: 48% em 1x; 2x já não fechava 60); a imagem de 4x está em
        `build/autotest/gs2_f8_4x/last.png`. O que faltava para 60 era a leitura do flare síncrona — resolvido no §83.
    - **Ainda não:** contador F3, mensagens de savestate e depurador F1 (desenhados pelo raylib) ficam atrás da
      janela-filha; `PS2X_FRAME_TIMES` não media as apresentações do PCSX2 (resolvido no §83);
      `capture_window.ps1` (PrintWindow) devolve preto: usar `tools/win/capture_screen_window.ps1` (copia da tela) ou
      `PS2X_PCSX2_SNAPSHOT_DIR`.

83. **Troca do GS, fase 3: desempenho e escala (2026-10-11).** Critério: luta de 8 em 1x/2x/4x/6x a 60 lisos — passou,
    até 8x.
    - **Leitura do flare assíncrona com o GS do PCSX2.** Na fase 2 a GameThread esperava a GSThread e a GPU a cada
      leitura GS → EE (54.7 fps em 1x). O cache do §50/§64 volta a valer (`PS2X_GS_ASYNC_READBACK`; o jogo recebe o
      bloco do quadro anterior): a renovação roda na GSThread com `read_local` da DLL (`Flush` + `InvalidateLocalMem`
      da região, que traz os alvos da GPU para a memória local, + `ReadImageX`), fora do estado de transferência do
      GS: a transferência que o próprio fluxo GIF abriu (TRXDIR=1) fica sem leitor e é descartada no próximo TRXDIR.
      Leituras que não estão no cache (a primeira de cada bloco) seguem síncronas por `GSInitAndReadFIFO`.
      O modo "Unsynchronized" do PCSX2 não serve aqui: ele pula a leitura de profundidade (o teste do flare).
    - **`PS2X_FRAME_TIMES` com o GS do PCSX2:** o back end grava uma linha por apresentação (vsync na GSThread), nas
      colunas do arquivo do laço principal, então `b.sh`, `pacing_summary.py` e `image_cadence.py` funcionam. Sem hash
      da imagem (seria uma leitura da GPU por quadro): conta como imagem nova o field em que o jogo mandou >= 4 KB de
      GIF. O intervalo é entre apresentações na GSThread (mailbox: não espera o monitor).
    - **fight8, GS do PCSX2, mailbox** (`b.sh`; nosso GS na mesma sessão: 59.9 fps, GameThread 88%, GSThread 48%):

      | escala | fps | intervalo med / p99 / max (ms) | distintas/s | GameThread | VUThread | IOPThread | GSThread | GIFThread | driver |
      |---|---|---|---|---|---|---|---|---|---|
      | 1x | 60.0 | 16.6 / 20.4 / 25.4 | 60.0 | 86.8% | 49.5% | 41.7% | 28.9% | 15.9% | 12.1% |
      | 2x | 60.0 | 16.7 / 19.8 / 26.1 | 60.0 | 86.5% | 51.2% | 42.9% | 28.5% | 17.3% | 15.1% |
      | 4x | 60.0 | 16.7 / 19.3 / 23.1 | 60.0 | 88.0% | 53.4% | 41.4% | 28.5% | 16.1% | 19.0% |
      | 6x | 60.0 | 16.7 / 19.3 / 23.9 | 60.0 | 86.0% | 50.0% | 39.9% | 28.9% | 15.4% | 19.2% |
      | 8x (5120x3584) | 60.0 | 16.6 / 19.2 / 21.6 | 60.0 | 86.6% | 49.3% | 41.2% | 28.1% | 15.5% | 23.8% |

      GTX 1660 SUPER. A GSThread não muda com a escala (o trabalho por pixel é da GPU); a GameThread é a mesma do nosso
      GS (o limite agora é o jogo). Primeira rodada após o build: um pico de 60 ms em 1x e 101 ms em 2x, que não
      voltaram na repetição. Apresentações a menos de 10 ms uma da outra (duas no mesmo refresh, uma descartada no
      mailbox): 0-2 em 1800; intervalos acima de 20 ms: 7-26 em 30 s (nosso GS: 3).
    - FIFO (`PS2X_PCSX2_VSYNC=1`) testado: 1x pior (p99 28.7 ms, 200 intervalos > 25 ms), 4x igual ao mailbox; o
      padrão continua mailbox.
    - Imagem de 8x conferida (`build/autotest/gs3_8x/snap`).

84. **GS do PCSX2 com 2 e 4 núcleos + travamento depois de load (2026-10-11).** fight8_heli, `b.sh` com afinidade
    (`PS2X_PROCESS_AFFINITY`, aplicada no boot), mesma sessão:

    | máquina | GS | fps (rodadas) | velocidade | distintas/s | 1 refresh | GameThread | processo |
    |---|---|---|---|---|---|---|---|
    | 2 núcleos/4 threads (0-3) | nosso, 1x | 55.5, 54.8 | 92-93% | 54.4, 53.8 | 98% | 95% | 294-297% |
    | 2 núcleos/4 threads (0-3) | PCSX2, 1x | 57.1, 56.8, 56.4, 56.7 | 94-95% | = fps | 100% | 95% | 262-265% |
    | 2 núcleos/4 threads (0-3) | PCSX2, 4x | 55.2, 56.0, 57.0, 57.0, 56.6, 56.8, 56.3 | 92-95% | = fps | 100% | 94-96% | 267-274% |
    | 4 núcleos/8 threads (0-7) | nosso, 1x | 53.7, 53.3, 53.7 | 98% | 53.1-53.4 | 90% | 89-90% | 291-294% |
    | 4 núcleos/8 threads (0-7) | PCSX2, 1x | 59.7, 59.7, 59.7, 59.5 | 99-100% | 59.6-59.8 | 100% | 89-91% | 252-258% |
    | 4 núcleos/8 threads (0-7) | PCSX2, 4x | 59.6, 59.9, 59.0, 59.7 | 98-100% | 59.1-59.9 | 100% | 89-91% | 258-266% |

    - Com 2 núcleos o limite é a GameThread (95%) nos dois GS; o do PCSX2 dá ~1-2 fps a mais e 4x custa o mesmo que 1x.
    - Com 4 núcleos o GS do PCSX2 fecha 60 em 1x e 4x; o nosso caiu para ~53.5 (GIFThread 53% + GSThread 48% disputando
      8 threads). No §67 o nosso fazia 59.0 com 4 núcleos; o `urban_c8old.exe` (antes do C8) hoje dá 51.2, então a
      diferença é do estado do PC nesta sessão, não de código. "1 refresh" e "distintas" do PCSX2 usam a aproximação
      do §83 (field com GIF de desenho).
    - **Travamento depois de load (1 em 5 rodadas de 4x com 2 núcleos, antes da correção):** o jogo parou logo após
      o `!load` (nenhum byte de GIF depois, threads ociosas). Causa provável: com o GS do PCSX2, o stall do SIGNAL deixa
      a continuação do GIFtag (`g_gifCont`) ativa (§82); o load limpava a fila do stall (`ps2xGsResetStall`) mas não a
      continuação, e os pacotes seguintes eram lidos como resto de um tag antigo (um SIGNAL falso segura a fila para
      sempre). `ps2xGsResetStall` agora zera `g_gifCont` também (vale para os dois GS: uma continuação de antes do load
      não tem sentido depois dele). Depois da correção: 12 rodadas (1x/4x, 2 e 4 núcleos, 3 de cada) sem travar.

85. **Troca do GS, fase 4 (parte 1): GS do PCSX2 como padrão e filtros no `config.ini` (2026-10-11).**
    - **Padrão:** sem `PS2X_GS_RENDERER` (e sem `Renderizador` no `config.ini`) o runtime usa o GS do PCSX2;
      `Renderizador=nativo` volta ao nosso. Sem a `pcsx2gs.dll`, cai no nosso com aviso no log.
    - **Opções de imagem** (API da DLL versão 2: `Pcsx2GsOpenParams::options`, lista `chave=valor;`): o back end monta a
      lista a partir de `PS2X_PCSX2_*`, e o `config.ini` mapeia as chaves em português:

      | `config.ini` | opção do PCSX2 | valores (padrão) |
      |---|---|---|
      | `FXAA` | FXAA | 0/1 (**1**) |
      | `FiltroTexturas` | TextureFiltering | 0 nenhum, 1 bilinear forçado, 2 como o PS2, 3 forçado menos sprites (2) |
      | `FiltroTrilinear` | TriFilter | -1 automático, 0 desligado, 1 como o PS2, 2 forçado (-1) |
      | `Anisotropico` | MaxAnisotropy | 0, 2, 4, 8, 16 (0) |
      | `Nitidez`, `NitidezForca` | CASMode, CAS_Sharpness | 0 desligada, 1 nitidez, 2 nitidez + redimensionamento (0); 0-100 (50) |
      | `AjusteCores`, `Brilho`, `Contraste`, `Saturacao`, `Gama` | ShadeBoost | 0/1 (0); 1-100 (50) |
      | `ShaderTV` | TVShader | 0 nenhum, 1 scanlines, 2 diagonal, 3 triangular, 4 onda, 5 CRT Lottes, 6 4xRGSS, 7 NxAGSS (0) |
      | `FiltroTela` | LinearPresent | 0 nenhum, 1 suave, 2 nítido (1) |
      | `Pontilhado` | Dithering | 0 desligado, 1 escalado, 2 sem escala, 3 forçar 32 bits (2) |
      | `PrecisaoMistura` | AccurateBlendingUnit | 0 mínima ... 5 máxima (1) |
      | `Esticar` | AspectRatio Stretch / 4:3 | 0/1 (0) |

      Os padrões são os do PCSX2, menos o FXAA, ligado. A DLL loga a lista aplicada (`[pcsx2-gs] ... options:`).
    - **`config.ini` antigo:** depois de ler, o `urban.exe` acrescenta no fim as chaves do modelo que faltam, com os
      comentários e o valor padrão, numa seção "Opções novas"; os valores que já estavam ficam como estão.
    - **Teste:** fight8 com os padrões (PCSX2 + FXAA): 1x 59.9 fps (p99 20.3, 0 soluços), 4x 60.0 (p99 19.3); GSThread
      ~30%, igual ao §83. FXAA conferido na imagem (`build/autotest/fxaa_cmp.png`: sem e com, bordas da grade e dos
      lutadores suavizadas).

86. **Troca do GS, fase 4 (fim): OSD, recursos no repositório e cache de shaders (2026-10-11).**
    - **OSD com o GS do PCSX2** (API da DLL versão 3, `set_osd`, chamável de qualquer thread): o laço principal passa
      a mensagem do topo (mensagem de savestate com o fade dela, ou o aviso da captura de perfil F6 em vinho) e a linha
      do contador F3; a DLL desenha no `ImGuiManager::RenderOSD` (chamado pelo PCSX2 a cada apresentação) com a
      Roboto do PCSX2, no tamanho e lugar do desenho do raylib (altura da janela / 36). O "Screen" do F3 conta as
      apresentações da GSThread (`GSPcsx2Backend::Presents`).
    - **Textos do OSD em inglês** (os dois GS): "Game N FPS | Screen N | Speed N% | worst frame N ms" e os avisos do
      F6/F7 ("CAPTURING PROFILE n (F6 to stop)", "mark", "SAVING", "PROFILE SAVED"); as mensagens de savestate já eram.
    - **F1:** o depurador é ImGui do `urban.exe` desenhado pelo raylib, embaixo da janela do PCSX2; enquanto ele está
      aberto, a janela do PCSX2 fica escondida (`SetHostUiVisible`, chamado pelo callback do depurador em `main.cpp`):
      o depurador aparece sobre fundo preto e a imagem do jogo volta ao fechar.
    - **Recursos no repositório:** `thirdparty/pcsx2-gs/resources/` com `shaders/dx11`, `shaders/common` (FXAA, CAS) e a
      fonte (`fonts/Roboto-Regular.ttf` + copyright), do commit `2954ebe` do PCSX2 (licenças em `resources/README.md`);
      o build copia de lá para `bin/resources` (antes vinha de `H:\JOGOS\pcsx2-ref`).
    - **Cache de shaders pronto:** `thirdparty/pcsx2-gs/shadercache/d3d_shaders_sm50.{bin,idx}` (292 KB, DXBC, vale
      para qualquer placa DX11) com as variantes de menus e lutas de 1x a 8x, com e sem FXAA; o build copia para
      `bin/shadercache_pcsx2` só os arquivos que o jogador ainda não tem (`tools/copy_if_missing.cmake`), então o cache
      que cresce jogando nunca é trocado. Rodar seleção, luta de 4 e luta de 8 em 1x/2x/4x não acrescentou nenhuma
      variante ao cache.
    - **Teste:** OSD conferido na tela (`build/autotest/gs4_osd2`: linha do F3 e "State saved"); F1 com
      `URBAN_DEBUG_UI=1` (`gs4_f1`); fight8 em 2x só com os recursos do repositório: 59.9 fps, 60 distintas/s.
    - Ainda fora do repositório: o código do PCSX2 (`H:\JOGOS\pcsx2-ref`) e os DirectX-Headers, que o CMake lê.

87. **Troca do GS, fase 5 (parte 1): texturas HD (2026-10-11).** A troca de texturas do PCSX2 (`GSTextureReplacements`)
    entra na DLL; o pacote do usuário (`Documents/PCSX2/textures/.bakSLUS-21209`: 2875 PNG em `replacements`, 6.4 GB
    com as "Custom Colors") funciona sem ser renomeado nem copiado.
    - **Sem libpng nem libzip:** `compat/GSTextureReplacementLoaders.cpp` é gerado por `tools/make_loaders.py` a partir
      do arquivo do PCSX2, com o leitor de PNG trocado pelo WIC do Windows (`compat/PngWic.inl`: mesma saída, RGBA 32
      bits, A = 0x80 em imagens sem alfa como o PCSX2 faz); o leitor de DDS é o do PCSX2. `compat/zip.h` é a parte da
      API do libzip que o código usa, sempre falhando: pacotes `.zip` não são lidos (só arquivos soltos).
    - **Pasta:** a DLL recebe `texture_dir=<pasta que tem replacements/>`; o GS do PCSX2 procura
      `<Textures>/<serial>/replacements`, então `Textures` vira a pasta de cima e o "serial" (`VMManager::GetDiscSerial`)
      vira o nome da pasta dada. O carregamento é o do PCSX2: assíncrono, conforme o jogo usa cada textura
      (`PrecarregarTexturas=1` carrega tudo ao abrir).
    - **`config.ini`:** `TexturasHD=1` (padrão; `PS2X_PCSX2_TEXTURES`), `PastaTexturas=` (vazio = `textures\SLUS-21209`
      ao lado do `urban.exe`; `PS2X_PCSX2_TEXTURE_DIR`), `PrecarregarTexturas=0` (`PS2X_PCSX2_TEXTURE_PRECACHE`).
    - **Teste** (pacote lido de Documentos por `PS2X_PCSX2_TEXTURE_DIR`): luta de 4 em 2x com e sem HD
      (`build/autotest/tex_cmp.png`, `tex_cmp_crop.png`): prédio, mar, grade, chão, roupas e retratos do HUD trocados.
      Um segundo a 34 fps no aquecimento logo depois do load (primeiro uso das texturas). fight8 com HD:

      | escala | rodada | fps | intervalo med / p99 / max (ms) | distintas/s | GameThread | GSThread |
      |---|---|---|---|---|---|---|
      | 1x | 1 | 59.7 | 16.7 / 20.7 / 29.7 | 59.7 | 91.5% | 28.4% |
      | 4x | 1 | 59.7 | 16.7 / 20.0 / 24.8 | 59.8 | 90.8% | 31.4% |
      | 1x | 2 | 59.7 | 16.7 / 20.6 / 22.7 | 59.8 | 90.9% | 29.9% |
      | 4x | 2 | 59.8 | 16.7 / 19.4 / 22.1 | 59.8 | 90.2% | 28.8% |

    - As "Custom Colors" (cores alternativas dos lutadores) ficam fora de `replacements`; para usar, copiar as
      escolhidas para dentro dela, como no PCSX2.

88. **`config.ini` e `controls.ini` em inglês (2026-10-11).** A pedido do usuário.
    - **`config.ini`:** chaves e comentários em inglês: `InternalResolution`, `StartMaximized`, `Renderer`
      (`pcsx2`/`native`), `FXAA`, `TextureFiltering`, `TrilinearFiltering`, `AnisotropicFiltering`, `Sharpening`,
      `SharpeningStrength`, `ColorCorrection`, `Brightness`, `Contrast`, `Saturation`, `Gamma`, `TVShader`,
      `ScreenFilter`, `Dithering`, `BlendingAccuracy`, `Stretch`, `HDTextures`, `HDTexturesFolder`, `PreloadHDTextures`.
      As variáveis de ambiente por trás não mudaram (§85, §87).
    - **`controls.ini`** (antes `controles.ini`): as variáveis já eram em inglês (`PS2X_BIND_*`, `PS2X_SHOW_FPS`...);
      os comentários passaram para o inglês. Agora é criado com os padrões quando falta (o `controles.ini` só existia
      no `bin` do usuário).
    - **Conversão dos arquivos antigos** (`src/main.cpp`): na primeira execução, um `controles.ini` vira
      `controls.ini` e um `config.ini` com chaves em português é reescrito em inglês, preenchendo o modelo novo com os
      valores do arquivo antigo (`Renderizador=nativo` vira `Renderer=native`); linhas que o modelo não tem vão para o
      fim ("Other settings"), e o antigo fica como `controles.ini.old` / `config.ini.old`. Os nomes em português
      continuam aceitos se alguém os escrever.
    - **Teste** com os arquivos do usuário: conversão feita, nenhuma chave perdida ou mudada (comparação por script dos
      `.old` com os novos: teclas nas setas, F3 ligado, janela maximizada, caminho do pnach comentado); na segunda
      execução os arquivos ficam iguais e nada é acrescentado.

89. **FMVs funcionando (2026-10-11).** Abertura (logos Dolby Pro Logic e Sofdec/ADX, vídeo de abertura com legendas),
    som e imagem; o jogo segue para o título e o menu. Quatro problemas, nesta ordem:
    - **IPU:** emulação de hardware do IPU portada do Ace Combat 5 recompilado (`AC5-Recomp-ref`, Apache-2.0):
      `src/lib/ipu/ps2_ipu.c` + `ps2_ipu_dct.inc` (iguais aos originais, só os includes trocados por
      `ps2_ipu_compat.h`; `ps2_ipu_glue.c` com o log). O lado do DMA, que no AC5 fica no `ps2_hw.c` dele, foi escrito em
      `ps2_memory.cpp`: registradores `0x10002000-0x1000203C`, FIFOs `0x10007000/0x10007010`, toIPU (canal 4, normal e
      chain, alimentando o IPU conforme o comando pede) e fromIPU (canal 3, esvaziado a cada comando). Avaliado antes:
      o IPU do PCSX2 (preso ao núcleo dele) e decodificar o `.sfd` com FFmpeg por HLE do Sofdec (exigiria trocar o
      player inteiro); o do AC5 é autocontido e genérico. `PS2X_IPU=0` desliga.
    - **A thread do decodificador nunca acordava** (sem IPU nenhum comando chegava ao IPU, e nem o som do filme
      tocava): o Sofdec decodifica na thread ociosa do ADXM da CRI (prioridade 25). A função em `0x2dfad8` só a acorda
      (`WakeupThread`) quando `ResumeThread` devolve o id da thread, como o kernel do PS2 faz; o nosso devolvia 0
      (`KE_OK`). Agora `SuspendThread`, `ResumeThread` e `(i)WakeupThread` devolvem o id em caso de sucesso
      (`PS2X_THREAD_RET_ID=0` volta ao antigo). Achado com `PS2X_THREAD_TRACE` (temporário): a thread 6 dormiu uma vez
      e, depois, a cada quadro só era suspensa/retomada.
    - **HLE da libmpeg:** o M2V da CRI usa a libmpeg da Sony por baixo das funções que o HLE (FFmpeg) substitui; com
      o HLE ele ligava o DMA e não mandava mais comandos. Com o IPU emulado, `sceMpeg*`/`sceIpu*` deixam de ser
      ligadas ao HLE e a libmpeg do jogo dirige o IPU (`URBAN_MPEG_HLE=1` religa o HLE; `PS2X_IPU=0` também).
    - **Imagem verde (só resíduo):** os quadros P/B buscam os macroblocos de referência no scratchpad por DMA toSPR em
      modo chain, e o nosso DMA de scratchpad só fazia o modo normal (a transferência não acontecia: predição zero,
      YCbCr 0 vira verde no CSC). Agora: interleave (MOD=2, `D_SQWC`) nos dois canais e chain no toSPR
      (`PS2X_SPR_INTERLEAVE=0` desliga). Achado pelo trace: saída do BDEC no scratchpad (`0x80001000`/`0x80003000`),
      CSC do quadro inteiro (1023 + 97 macroblocos) de `0x00eda580`.
    - **Velocidade no vídeo:** os comandos do IPU rodam dentro da escrita do registrador (um CSC de quadro inteiro
      leva ~3,3 ms) e atrasavam o vblank além dos 2 ms de tolerância do modo MTVU: 82% de velocidade. Enquanto o IPU
      decodifica (comando BDEC/IDEC/CSC/PACK nos últimos 0,5 s), o vblank recupera o atraso como no modo síncrono
      (`ps2xIpuActiveRecently`, `PS2X_IPU_CATCHUP=0` desliga): 100%, 60 apresentações/s (mediana 14 ms, p99 30 ms; o
      vídeo é a 30 fps). Custo do IPU por segundo de vídeo: BDEC ~80 ms, CSC ~85 ms, VDEC ~11 ms (`PS2X_IPU_STATS=1`).
    - **Regressão:** fight8 60.0 fps / 60 distintas (GameThread 86%), luta de 4 e seleção 60; boot até o título e o
      menu de perfil.
    - Ferramentas novas: `PS2X_IPU_TRACE`, `PS2X_IPU_STATS`, `PS2X_IOP_CD_LOG`, `URBAN_CALL_PROFILE_RANGE`.
    - Falta: a mini tela do "mission info" (modo história); o estado do IPU não entra no savestate (salvar durante um
      vídeo).

90. **Repositório reorganizado (2026-10-12).** Estrutura combinada com o usuário:
    - `README.md` em inglês (padrão) com link para `README.pt-BR.md`; os dois atualizados (status, estrutura, build).
    - `src/` (era `Port/src`; o `CMakeLists.txt` do port foi junto).
    - `runtime/`: o PS2Recomp **dentro do repositório**, não mais submódulo: `ps2xRuntime`, `ps2xIOP`, `ps2xRecomp` e
      só `gs_reference/` + `tools/` do `ps2xTest` (vu1_recomp, gs_replay, vu0_audit), com `CMakeLists.txt` próprios no
      lugar dos de cima do upstream (sem analyzer, studio, testes, Android/Vita). Base: fork do Taylor, branch
      `sotc-port`, commit `ac9efa0` (`runtime/README.md`). Acabou o `runtime-urban-port.patch` e o `CORRECOES.md`; a
      cópia antiga, com o histórico git e as mudanças sem commit, ficou fora do repositório em `H:\JOGOS\PS2Recomp-urban`.
    - `thirdparty/pcsx2`: só os arquivos do PCSX2 que o build compila ou inclui (fontes do CMake + cabeçalhos do
      `ninja -t deps` de um build completo + licenças + o `3rdparty/cpuinfo` inteiro, que tem CMake próprio): 271
      arquivos, 12 MB, contra 151 MB do clone. `thirdparty/directx-headers`: só o `include/` (1,9 MB).
      `thirdparty/pcsx2-gs`: o nosso wrapper (era `External/pcsx2-gs`), agora apontando para esses dois, sem
      depender de `H:\JOGOS`. Origem e commits em `thirdparty/README.md`.
    - `tools/`, `docs/` (`AMBIENTE.md` → `ENV.md`, `JOGO.md` → `GAME.md`), `game/` (a ISO; só o README versionado),
      `textures/` (pacotes HD; só o README versionado), `bench/` (`states/`, `refs/` e `runs/` do benchmark e do
      drive.py; fora do git: são dados do jogo).
    - `recomp/urban_functions.csv`: o mapa de funções do Ghidra, que ficava em `build/ghidra` (fora do git); agora
      gerar o código não precisa do Ghidra.
    - `build.py` no lugar de `configure.bat`/`build.bat`/`run.bat`/`tools/generate.bat`: menu numerado (build, build e
      jogar, jogar, gerar o código da ISO, reconfigurar, benchmark escolhendo o savestate, só a DLL do PCSX2, abrir a
      pasta do jogo) e os mesmos comandos por argumento (`python build.py build|play|generate|configure|bench`).
      `-j 8` por padrão (`URBAN_JOBS`); recusa compilar com o `urban.exe` aberto.
    - O `urban.exe` procura a ISO em `game/` (era `Game/`) e as texturas HD em `textures/SLUS-21209` ao lado dele ou até
      três pastas acima (a `textures/` do repositório quando roda de `build/port/bin`).
    - Removidos (movidos para `H:\JOGOS\UrbanRecomp-removed`, para o usuário apagar): `perfil.txt`, `imgui.ini`,
      `mc0/`, `mc1/` da raiz; logs, capturas, backups e execuções antigas de `build/`; sobras de teste em
      `build/port/bin` (`.pcm`, `.csv`, `ram_*.bin`, `urban_c8old.exe`, `.ini.old`...).
    - O build continua em `build/port` (as fontes do FetchContent vêm do cache do build do SotC); com os caminhos novos
      ele recompilou tudo (10.125 passos, sem erro). Os objetos antigos (`build/port/External`, `build/port/Port`)
      foram para a pasta de removidos.
    - Clone novo sem o `tools/ps2_recomp.exe` (fora do git): `python build.py generate` compila o recompilador antes.
    - `.recomp.json` na raiz (formato do recomp.fyi): status `playable`; selo dos READMEs em **beta**. Mapa de status
      atualizado (bibliotecas 99,3%, hardware 97,4%; só o multitap pendente). Créditos com os links do port do SotC
      (LightVelox) e da recompilação do Ace Combat 5 (sal063).
    - **Verificado:** o jogo acha a ISO em `game/`; fight8 60,0 fps (59-61 por segundo, pior quadro 19 ms), seleção de
      4 60,0; texturas HD da pasta do PCSX2 carregam (`PS2X_PCSX2_TEXTURE_DIR`, 60,0 fps); imagens conferidas.
      Git: 746 arquivos, 23 MB.

91. **Bolinhas do round no HUD: `PMULTH` do recompilador estava errado (2026-10-12).** Faltavam também com o GS antigo,
    então o problema vinha de antes do GS. Caminho até a causa (testado só com o GS do PCSX2):
    - `PS2X_GIF_DUMP=<s>:<fields>:<arquivo>` (novo, no back end do PCSX2) grava o fluxo GIF enviado ao GS;
      `tools/gif_dump.py` lista cada desenho (primitiva, textura, posição na tela, cor) com o offset no fluxo. Na luta
      de 8 não havia nada onde ficam as bolinhas (fora o painel, nome, retrato, boneco).
    - O HUD é feito de tarefas do jogo (lista em `0xac0a80`, atualizada e desenhada por `0x1236c8`);
      `tools/task_list.py` percorre a lista numa imagem da RAM ou no PCSX2 pelo PINE (só leitura; o PCSX2 do usuário
      aberto no savestate dele, slot 1). Com uma luta 1v1 do port (savestate do usuário, `bench/states/fight1v1.state`)
      contra a mesma luta no PCSX2, os tipos de tarefa são os mesmos; as bolinhas são elementos da tarefa de
      partículas `0x386` (métodos `0x6a7d48`, desenho `0x10dbc8`) em posição de tela fixa (x 180/204, y 61-62). O port
      tem 2 por lutador (a regra da partida dele pede 2 rounds), o PCSX2 3.
    - O desenho chegava ao GS (`tstrip` com a textura `2240`), mas com cor `00000101` (quase preto, alfa 1/128):
      invisível. A cor sai de `PEXTLB`/`PADDUH`/`PEXTLH`/`PMULTH`/`PSRLW` sobre a cor do elemento e o vetor de brilho
      global (`0x80` em cada canal). O `PMULTH` gerado somava os 8 produtos num valor de 64 bits (estilo produto
      escalar) em vez de `rd = {P0, P2, P4, P6}`, `LO = {P0, P1, P4, P5}`, `HI = {P2, P3, P6, P7}`.
    - Correção em `runtime/ps2xRecomp/src/lib/mmi_translation_helpers.cpp`: `PMULTH` certo (inclusive LO/HI e
      `lo1`/`hi1`), e `PADDUH`/`PSUBUH` com saturação sem sinal (somavam com estouro). Código regenerado: 8 arquivos
      mudaram (`0x10d6c0`, `0x10dbc8`, `0x10e260`, `0x10ebe8`, `0x10f558`, `0x10f9a8`, `0x10fd18`, `0x133710` - as
      funções de sprite/partícula/efeito). Não tem chave de ambiente (é código gerado); voltar = regenerar com o
      gerador antigo.
    - **Resultado:** bolinhas aparecem (1v1 e nos 4 HUDs da luta de 8); o efeito de hit (fogo e faíscas) também
      aparece. fight8 59,9 fps (59-60), seleção de 4 60,0.
    - Ainda errados no gerador, mas o Urban não usa: `PMADDH`, `PMSUBH`, `PHMADH`, `PHMSBH`, `PMULTW`, `PMADDW`,
      `PMSUBW`, `PMADDUW` (somam tudo num escalar), `PDIVW`/`PDIVUW`/`PDIVBW` (só o primeiro elemento), `PMFHI`/`PMFLO`
      (só 64 bits). Contagem no código gerado: `pmulth` 7, `padduh` 10, nenhuma das outras.
    - Falta conferir: estrelinhas de stun, cores do seletor "1P VS 1C" (as funções de sprite usam o mesmo cálculo).

92. **FMVs deslocados com faixa preta, e estalos no áudio dos vídeos (2026-10-12).** Os vídeos depois do logo da Namco
    (Dolby, abertura) saíam com a imagem "rolada": o começo do quadro embaixo, uma faixa preta no meio, sem o
    letterbox. O "mission info" estava certo (é menor, um CSC só).
    - **Achado:** `tools/gif_dump.py --frames DIR:2bc0` remonta a textura do vídeo a partir dos uploads 16x16: já
      vinha partida. Na RAM, o buffer YCbCr do quadro (`0xeda580`, 1120 macroblocos em colunas) estava certo e o RGB
      (`0x109a5c0`) não: RGB[k] = CSC(Y[k+97]). `PS2X_IPU_PICLOG=<n>` (novo: start codes do FDEC, BDECs por slice,
      CSC/BCLR com as DMAs do IPU; `PS2X_IPU_PICLOG_ALL=1` todos os comandos com resultado) mostrou que, do segundo
      quadro em diante, o CSC de 97 macroblocos saía **antes** do de 1023.
    - **Causa:** o CSC dividido da libmpeg (`0x322148`) faz `AddDmacHandler(3, 0x322000)`, `D_STAT = 8` (limpa o status
      do fromIPU), `EnableDmac(3)`, e só então o CSC de 1023; o handler emite o resto (97) quando o fromIPU termina.
      Entre um quadro e outro a libmpeg deixa o canal 3 desligado, e as transferências fromIPU de cada BDEC terminam
      com ele desligado: o `EeScheduler` guardava essa conclusão (`g_pendingMaskedDmac`) e a entregava no
      `EnableDmac` - mesmo depois do `D_STAT = 8`, que no hardware apaga exatamente esse bit. O handler rodava cedo e
      o CSC de 97 convertia os macroblocos 0-96 para o fim da imagem.
    - **Correção:** escrever 1 num bit de status do `D_STAT` limpa também a conclusão guardada desse canal
      (`ps2xEeClearLatchedDmac`, chamada do `ps2_memory.cpp`; vale para todos os canais, como no hardware).
      `PS2X_DMAC_CLEAR_LATCH=0` desliga. Os vídeos saem com letterbox e legendas no lugar, do começo ao fim.
    - **Estalos no áudio dos vídeos:** a gravação da saída do SPU2 (`PS2X_AUDIO_DUMP`) tinha uma amostra 0 isolada a
      cada 5121 amostras (-2208, 0, -2255), nos dois canais. É o ADMA (por onde a CRI manda o PCM do vídeo): quando o
      bloco seguinte chega uma amostra atrasado, `admaSample` devolvia 0. Agora repete a última amostra do bloco
      anterior (o hardware continua tocando o que está no buffer); `PS2X_ADMA_HOLD=0` volta ao silêncio. Zeros
      isolados nos dois canais: 588 -> 0 (os 98 que sobram num canal só são a própria onda); o saída do host já não
      tinha underruns (`PS2X_AUDIO_STATS=1`: 0 em todo o vídeo).
    - **Regressão:** fight8 60,0 fps, seleção de 4 59,9, luta 1v1 60,1, luta de 4 60,0; boot até a tela de perfil
      (checagem do memory card) normal.

93. **Multitap (2026-10-12).** O jogo carrega o MTAPMAN original; faltava o multitap no SIO2 emulado. No Urban os
    controles extras só funcionam com o multitap na porta 2 (como no PCSX2 do usuário, `MultitapPort2 = true`).
    - **SIO2** (`iop_sio2.cpp`), como o `MultitapProtocol` do PCSX2: comandos com o primeiro byte `0x21` respondem
      `00 80 5a 04 00 5a` aos testes de suporte (`0x12` pad, `0x13` memory card) e `00 80 5a 00 00 <slot> 5a` à
      seleção de slot (`0x21` pad, `0x22` memory card; slot inválido: `ff`/`66`). O MTAPMAN manda esses comandos pelo
      endereço do memory card da porta (`card 1` no trace), como no console. Os comandos de pad da porta vão ao slot
      selecionado; os slots B-D têm pads próprios (`m_tapPads`, com modo e motores); memory card só no slot A.
    - **Host:** `IopHost::readPadSlot`/`padVibrationSlot` (o `DeferringHost` da thread do IOP repassa) e
      `ps2_host_input::padState(port, slot)`. Jogadores na ordem do PCSX2: 1A, 2A, 1B-1D, 2B-2D (só as portas com
      multitap); jogador 1 = teclado + primeiro gamepad, jogador n = n-ésimo gamepad. O raylib lê 4 gamepads; os
      jogadores 5-8 leem os joysticks 4-7 direto pelo GLFW (que o raylib já liga).
    - **Configuração:** `config.ini` `[Controls] Multitap=2` (0, 1, 2, 12), anexado aos arquivos antigos; agora uma
      chave anexada também vale na mesma execução. Variável: `PS2X_MULTITAP`. Teste sem controles:
      `PS2X_FAKE_PADS=<n>` (jogadores 1..n conectados, parados).
    - **Savestate:** os pads B-D e os slots vão numa seção opcional `IMTP` depois da `ISIO`: states antigos carregam
      (o leitor pula com `nextSectionIs`), e salvar/carregar com o multitap funciona (+162 bytes).
    - **Verificado:** com `PS2X_FAKE_PADS=5` o PADMAN acha e configura os 5 pads (1A, 2A, 2B, 2C, 2D: modos 0x41 -> 0xf3
      -> 0x73). fight8 60,0 fps com e sem multitap; com 5 pads conectados 60,0 (IOPThread ~40%). Falta o usuário testar
      com controles de verdade no menu multiplayer.

94. **Chiado no canal direito da música (2026-10-12).** O usuário ouviu um chiado só no lado direito, vindo da música,
    numa partida (não numa nova); deixou um quick state dela (`bench/states/user_pacing.state`).
    - **Medido:** a saída e o PCM do ADMA (`PS2X_ADMA_DUMP=<arquivo>`, novo: cada bloco de ADMA como é tocado) tinham
      degraus no canal direito concentrados em duas posições fixas de cada bloco de 256 amostras: 0 e 96 (~540 cada,
      média 23). O esquerdo e uma luta normal não. Save + load de um state limpo não causa (não é estado salvo).
    - **Causa:** o ADMA guardava o ponteiro para o buffer da CRI na RAM do IOP e lia cada bloco na hora de tocar.
      Cada transferência tem 2048 bytes (2 blocos, um a cada ~10,7 ms); no hardware a DMA põe os dois no buffer de
      ADMA do SPU2 na hora, e a CRI já reusa a memória. Aqui o segundo bloco era lido 5 ms depois, com a CRI
      escrevendo nele: quando as duas fases se encontram, a metade direita sai com 96 amostras novas e 160 antigas, e
      isso fica até a música recomeçar.
    - **Correção:** `admaStart` copia a transferência na hora (`iop_spu2.cpp`); `PS2X_ADMA_COPY=0` volta ao antigo.
      Com o state do usuário: degraus nas posições 0/95/96/255 de ~540 para ~10 (abaixo da média). O canal direito
      continua com mais agudos que o esquerdo nessa música: é a mixagem dela (os números não mudaram, só as emendas).
    - **Ritmo de quadros:** não reproduzido com o state (60 fps, apresentações p99 18 ms, também com 3 pads
      conectados). fight8 59,9, seleção de 4 60,0.
    - **Depois (mesmo dia):** o modo final lê cada bloco **um bloco à frente**, como o hardware (o SPU2 tem um buffer de
      duas metades e a DMA traz cada bloco quando o anterior começa a tocar); `PS2X_ADMA_COPY=all` copia a
      transferência inteira no início, `0` lê na hora de tocar. O state do usuário continua sem as emendas. O usuário
      ainda ouve um pouco de chiado à direita nesse state e na seleção (não na luta de 8 do heliporto): anotado.
    - **Em aberto: o canal direito "escorrega" no vídeo de abertura.** Às vezes (como antes destas mudanças), a partir
      de um momento pesado do vídeo, o canal direito passa a tocar o que o esquerdo toca 32 blocos depois (8192
      amostras, 171 ms: a fala sai na direita e repete na esquerda) até o vídeo acabar. 32 blocos é o anel de PCM da
      CRI; o buffer de ADMA em si é regular (duas metades de 2048 bytes em `0x62d10`/`0x63510`). Medido com
      `PS2X_ADMA_DUMP` e a correlação por bloco (`R[k]` ~ `L[k+32]`):
      - com o catch-up dos vblanks durante o IPU (§89): 5 de 9 execuções; sem (`PS2X_IPU_CATCHUP=0`): 0 de 3, mas o
        vídeo roda a ~82%; limitando o catch-up a 8 ou 16 ms: continua;
      - com o IOP na thread do jogo (`PS2X_IOP_THREAD=0`): 0 de 6 e velocidade normal, mas a luta de 8 cai para 50,6
        fps; pôr o IOP inline só durante o vídeo (mesmo com 10 s de folga) não resolve (3 de 5, 4 de 5) - testado e
        desfeito.
      - Próximo passo: achar no CRI_ADXI (ou no Sofdec do EE) o que avança o ponteiro do anel do canal direito.

95. **Controles pelo SDL3, mapeamento no formato do PCSX2 e vibração (2026-10-12).** No teste online do usuário pelo
    Parsec, o controle virtual do Parsec virou o jogador 1 (junto com o teclado) e o controle dele o 2: o jogador n
    era o n-ésimo gamepad do GLFW e o teclado sempre do jogador 1. Pedido: configurar como no PCSX2 (portas, multitap,
    mapeamento), e vibração.
    - **SDL3 só para os controles** (`ps2_host_gamepads.cpp`; a janela continua do raylib). SDL 3.4.10 por
      FetchContent (URL + SHA-256), como `SDL3.dll` ao lado do `urban.exe` (outros programas podem carregar a mesma DLL para
      listar os controles); sem áudio/GPU/render/câmera. Dicas: eventos de controle em segundo plano (o SDL não tem
      janela) e sem tratador de sinais. Numeração como o `SDLInputSource` do PCSX2: o índice de jogador que o SDL dá
      (o número XInput de um controle de Xbox) se estiver livre, senão o menor livre; o console mostra
      `[input] SDL-<n>: <nome>` ao conectar e desconectar.
    - **`controls.ini` no formato do PCSX2** (`[InputSources]`, `[Pad]`, `[Pad1]`-`[Pad8]`): uma linha
      `Botão = Fonte/Elemento` por ligação (`Keyboard/F`, `Pointer-0/LeftButton`, `SDL-0/FaceSouth`,
      `SDL-0/+LeftTrigger`, `SDL-0/-LeftY`; `a & b` = as duas juntas), `LargeMotor`/`SmallMotor = SDL-<n>/...`,
      `Deadzone`, `AxisScale` (1,33 como o PCSX2), `ButtonDeadzone`, `InvertL/R`, motores com escala, `Type = None`.
      As seções do `PCSX2.ini` podem ser copiadas. O `urban.exe` passa o arquivo para variáveis (`PS2X_PAD<n>=1`,
      `PS2X_PAD<n>_<TECLA>`, `PS2X_MULTITAP`, `PS2X_SDL_INPUT`); sem a seção vale o padrão do runtime
      (`defaultPadSettings`, o mesmo que escreve o arquivo): jogador 1 = teclado/mouse antigos + `SDL-0`, jogador n =
      `SDL-<n-1>`. Um jogador conta como conectado quando alguma ligação dele pode ser apertada (teclado sempre,
      controle se conectado). Botões analógicos (gatilhos) dão pressão de verdade.
    - **Conversão:** um `controls.ini`/`controles.ini` antigo (linhas `PS2X_BIND_`) vira o novo, com as teclas do
      jogador 1 trocadas pelas do usuário (`NONE` = sem tecla); as chaves de execução (`PS2X_SHOW_FPS`,
      `URBAN_PROGRESSIVE`, `#PS2X_PNACH=...`) vão para a seção nova `[Runtime]` do `config.ini` com os mesmos valores;
      o `[Controls] Multitap` sai do `config.ini` e vira `[Pad] MultitapPort1/2`. O antigo fica como
      `controls.ini.old`.
    - **Vibração:** o DualShock 2 emulado já decodificava os motores; `PS2IopHostAdapter::padVibrationSlot` passa
      para `ps2_host_input::setVibration` (thread do IOP, atômico por jogador) e a thread da janela toca nos controles
      das linhas `LargeMotor`/`SmallMotor` (motor grande 0-255, pequeno liga/desliga, como no PCSX2), renovando a
      cada 0,5 s enquanto roda (duração de 1 s: para sozinho se o jogo parar de mandar). `PS2X_LOG_VIBRATION=1` mostra
      o que o jogo manda.
    - **Chave:** `[InputSources] SDL = false` / `PS2X_SDL_INPUT=0` lê os gamepads pelo GLFW como antes (sem vibração;
      `SDL-<n>` = gamepad n do GLFW), com o mesmo mapeamento.
    - **Verificado:** o `controls.ini` do usuário converteu certo (△○✕□ nas setas, direcional do teclado sem tecla,
      R1 = E, L2 = 1, R2 = 3, multitap na porta 2; `PS2X_SHOW_FPS=1` e o caminho do pnach comentado no `[Runtime]`).
      O SDL achou o controle do usuário (`SDL-0: PS4 Controller`, também pelo GLFW no modo antigo). **O jogo usa
      vibração:** na luta 1v1 o motor grande vai a 127 a cada golpe e cai de 3 em 3, o pequeno liga/desliga. fight8
      59,9 fps, 1v1 60,0 (SDL e GLFW). Corrigido junto: o `parseKeyLine` lia um comentário como `# 0 = ...` como
      chave desligada e a conversão o reescrevia como `#0=...`; agora uma chave começa com letra ou `_`.
    - **Falta o usuário testar:** a vibração no controle, e o Parsec (escolher `SDL-<n>` em cada `[PadN]`).
      **Depois:** a vibração funcionou no controle do usuário.

96. **Ritmo de quadros na tela: a fase do vblank (2026-10-12).** O usuário achava que não estava liso, apesar de 60 fps.
    As medidas do lado do jogo (`pacing_summary`, `image_cadence`) davam 60 imagens distintas/s, 100% uma vez só:
    medem quando o jogo entrega o quadro, não o que o monitor mostra.
    - **Medida nova:** `PS2X_PRESENT_STATS=<arquivo>` (no `pcsx2gs.dll`, `Bridge.cpp`): depois de cada present, as
      estatísticas do swap chain (`GetFrameStatistics`: qual present está na tela e desde qual atualização do
      monitor). `tools/present_stats.py` conta as atualizações em que uma imagem ficou de novo (R) e os presents nunca
      mostrados (S). O `PS2X_FRAME_TIMES` do GS do PCSX2 ganhou `start_phase_ms`/`end_phase_ms`: onde o present cai
      na atualização do monitor (ms depois do vblank). `GSDevice11::GetSwapChain()` acrescentado no PCSX2 vendorizado.
    - **Achado:** 1v1 com a config do usuário (3x, maximizado, monitor de 60 Hz): só 71,9% dos presents mostrados uma
      vez, 5,3 repetições + 5,3 pulos por segundo, sempre em par ("R.S"). fight8: 73,6%. Os presents caíam 1,4-3,5 ms
      depois do vblank (o lock do §67 mira 1 ms). O swap chain em janela é mailbox (flip discard, `Present(0)`): o
      compositor pega a última imagem pronta logo depois de cada vblank, no mesmo intervalo; quem chegava depois ficava
      para a próxima e a seguinte a substituía.
    - **Correção:** o alvo da fase virou `PS2X_VSYNC_PHASE_MS` (padrão **7 ms** depois do vblank do monitor; antes 1).
      Varredura na 1v1: 4 ms 88,1%, 6 ms 99,7%, 8 ms 99,8%, 10 ms 95,5%, 12 ms 95,1% (de 10 em diante a imagem
      encosta na próxima coleta). Com 7: 1v1 99,8% (0,03 repetição/s), fight8 99,6% (0,1/s), 60 fps, pior quadro do
      fight8 26 -> 21 ms. O usuário confirmou: ficou liso.

97. **Lock do vblank também em 120/180/240 Hz (2026-10-12).** Pergunta do usuário: e com monitor de 144/240 Hz? O lock
    só valia com o monitor a ~60 Hz (2%); fora disso o relógio emulado corria solto.
    - **Agora** (`lockToHostVsync`, `EeScheduler.cpp`): o lock acha o múltiplo inteiro (1 a 8) cuja atualização vezes n
      fica a 2% de 59,94 Hz; com 120/180/240 Hz cada campo emulado ocupa 2/3/4 atualizações exatas. A fase alvo vira
      a mesma fração da atualização mais curta (7 ms de 16,68 -> 3,5 ms a 120 Hz, 1,75 ms a 240 Hz). O log diz
      `N refresh(es) per field`, e avisa de novo se a janela muda para um monitor de outra taxa.
    - **75/144/165 Hz** continuam sem lock: 60 imagens não cabem por igual (144/60 = 2,4 -> 2 ou 3 atualizações por
      imagem). Só VRR (G-Sync/FreeSync) resolve; isso vale para qualquer jogo de 60 fps e para o PCSX2.
    - **Teste:** `PS2X_VSYNC_TEST_MULTIPLY=<n>` finge um monitor n vezes mais rápido (vblanks intercalados). 1v1 com
      2 e 4: detectou 119,7 Hz/2 e 239,5 Hz/4, erro de fase médio 0,13 ms (pior 0,25-0,63), velocidade 100%, 60 fps.
      Em 60 Hz de verdade nada mudou (99,6% das imagens uma vez). Nesse modo a lisura na tela não vale (o monitor
      real é de 60 Hz); a fase ideal a 120/240 Hz só dá para conferir num monitor desses com `PS2X_PRESENT_STATS`
      (`PS2X_VSYNC_PHASE_MS` ajusta). Errar a coleta ali custa uma atualização curta (8,3 ou 4,2 ms), não 16,7.

98. **Tela preta em tela cheia (2026-10-12).** O usuário: em tela cheia (F11) tudo preto (boot, vídeos, jogo), volta
    ao sair; e com a janela sem foco a imagem aparece, ao clicar nela fica preta de novo.
    - **Causa:** com o GS do PCSX2 a imagem é da janela filha (swap chain DX11); a janela do raylib (OpenGL) continuava
      apresentando um quadro preto por volta (`ClearBackground(BLACK)` + `EndDrawing`). Em janela o DWM compõe as duas.
      Uma janela em primeiro plano do tamanho do monitor tem o swap chain virado direto para a tela (sem composição):
      só o preto dela aparece. Sem foco o Windows volta a compor, por isso a imagem voltava.
    - **Medido:** `PS2X_PRESENT_STATS` em tela cheia: o present mostrado ficou parado no 147 a execução toda (os
      quadros do GS nunca chegavam à tela). A captura de tela (GDI) mostra a imagem composta e não serve para ver
      isso.
    - **Correção** (`ps2_runtime.cpp`): com o GS do PCSX2 o laço do host não apresenta nada (`PollInputEvents()` no
      lugar de `BeginDrawing`/`EndDrawing`), exceto com o depurador F1 aberto (`GSPcsx2Backend::HostUiVisible()`, que
      esconde a janela filha). O ritmo continua vindo da espera pelo campo novo. `PS2X_HOST_PRESENT=1` volta a
      apresentar sempre.
    - **Junto:** `config.ini` `[Video] StartFullscreen=1` (`URBAN_START_FULLSCREEN`) abre em tela cheia (útil para atalhos
      e para testar); `tools/win/capture_monitor.ps1` captura o monitor inteiro.
    - **Verificado:** tela cheia com foco 99,8% das imagens mostradas uma vez (antes: nenhuma); fight8 em janela 99,65%,
      59,9 fps.

99. **Release 1.0.0 (2026-10-12).**
    - **No `urban.exe`:** `config.ini` ganhou `[Game] Iso` (`URBAN_ISO`: a imagem do disco em qualquer lugar),
      `[Mods] Enabled` (arquivos `.pnach` da pasta `mods` ao lado do `urban.exe`, separados por `;`, que viram
      `PS2X_PNACH` se ele não estiver definido) e a opção `--write-settings` (escreve, completa ou converte o
      `config.ini` e o `controls.ini` com os comentários e os padrões, e sai).
    - **`python build.py release`** (`tools/release.py`): `build/release/UrbanReignPC-1.0.0.zip` com `game\` (o
      `urban.exe`, só as DLLs que ele carrega, lidas da tabela de imports: 17 das 31 da pasta de build; as outras são
      sobras de builds antigos), o `pcsx2gs.dll` com `resources` e `shadercache_pcsx2`, `mods\` e `textures\` vazias
      com nota, README em inglês e português, LICENSE e THIRD-PARTY. Nada do disco nem nada que o jogo grava ao rodar
      (`.ini`, memory cards, states, `game_data`).
    - **Testado:** a pasta `game\` do release roda sozinha: acha a ISO pelo `config.ini`, confere o hash, abre o GS
      do PCSX2 e lê os controles pelo SDL.

100. **Esticar para a janela (2026-10-12).** `[Video] Stretch=1` continuava 4:3. A apresentação do GS do PCSX2
    (`CalculateDrawDstRect`) lê `EmuConfig.CurrentAspectRatio`, que no PCSX2 o VMManager copia de `GS.AspectRatio`
    ao iniciar; a nossa ponte (`Bridge.cpp` `Open`) só preenchia `GS.AspectRatio`, e o padrão de `CurrentAspectRatio`
    é Auto 4:3/3:2. Agora a ponte copia. O usuário confirmou.

101. **O release não apaga mais a pasta de jogar (2026-10-12).** O `python build.py release` apagava e recriava
    `build/release/UrbanReignPC-<versão>`; quem jogava por ela perdia `config.ini`, `controls.ini`, memory cards e
    states a cada geração. Agora `tools/release.py` monta tudo em `build/release/staging`, gera o ZIP de lá (sempre
    limpo) e só atualiza a pasta de jogar: copia os arquivos do release por cima, remove só os que um release
    anterior enviou e este não (lista em `.release-files`, apagando pastas que ficam vazias) e não toca no resto.
    Testado com config, memory card e mod de mentira: continuam lá, e o ZIP segue sem eles.

102. **Conferência antes de publicar no GitHub (2026-10-12).** ~748 arquivos no primeiro commit, o maior com 1,6 MB.
    Sem segredos (os alertas eram "password" e "secret" dentro do imgui e do xxhash), sem dados do jogo (nenhuma
    tabela de bytes, ELF, IRX, ISO, state ou memory card; `recomp/urban_functions.csv` só tem nomes e endereços). O
    `.gitignore` segura a ISO, `build/`, `generated/`, `bench/`, o pacote HD em `textures/SLUS-21209` e
    `tools/ps2_recomp.exe`; entrou nele `runtime/ps2xRuntime/vita/module/*.suprx` (5 módulos de GPU do PS Vita, da
    Sony/PowerVR, vindos do PS2Recomp; o port é só Windows). Links internos dos READMEs e docs conferidos.

103. **Teclado padrão novo, menu do build em passos e README para jogador (2026-10-12).**
    - **Teclado padrão** (jogador 1; `defaultPadSettings` em `ps2_host_input.cpp`): ✕ ○ □ △ nas setas (↓ → ← ↑),
      L1 Q, R1 E, L2 1, R2 3, L3 F, R3 G, START Enter, SELECT Tab, analógico esquerdo WASD; o direcional fica só no
      controle (as setas são dos botões) e o mouse não tem mais botão. Conferido com `urban.exe --write-settings` numa
      pasta limpa. O `controls.ini` do usuário já era quase isso (só o L3 foi de H para F).
    - **`build.py`:** o menu virou "Step 1) gerar o código do jogo, Step 2) compilar, Step 3) jogar", com `[done]`
      nos passos feitos e um aviso se não houver imagem em `game/`; os utilitários vêm embaixo, numerados de 4 em
      diante (compilar e jogar, CMake, só o pcsx2gs, benchmark, abrir a pasta, release).
    - **README:** o `README.md`/`README.pt-BR.md` ficou curto e para jogador (o que é, baixar e jogar com o link
      `../../releases/latest`, texturas HD do GBAtemp, mods, requisitos, controles, compilar do zero em 3 passos,
      créditos); o conteúdo técnico de antes foi para `docs/ADVANCED.md` e `docs/ADVANCED.pt-BR.md` (links
      corrigidos, teclado e menu novos). Os READMEs antigos ficaram em `README.backup.md` e `README.pt-BR.backup.md`
      (no `.gitignore`, só locais).

104. **Mapa de status só em inglês e português (2026-10-12).** `tools/status/generate.py` gera só `en` e `pt-BR`
    (o texto em espanhol do `data.toml` fica só como último recurso); `details.es.md` e `map.es.svg` saíram. Notas
    atualizadas: multitap no `controls.ini [Pad] MultitapPort2`, controles do PC pelo SDL3 com vibração, quadros
    sincronizados com monitores de 60/120/240 Hz, e o problema em aberto do canal direito no vídeo de abertura
    (§94). Bibliotecas 100% (983/983), hardware 100%. Os links do guia avançado para o mapa conferidos.

105. **Publicado no GitHub (2026-10-12).** Repositório público https://github.com/thiagordsfx/urban-reign-recomp,
    branch `main`, primeiro commit com 748 arquivos (autor com o e-mail noreply do GitHub, configurado só neste
    repositório). Release v1.0.0 publicado com `UrbanReignPC-1.0.0.zip` (48,3 MB, SHA-256 `ed3cfecd...1582e8`) e as
    notas em `docs/releases/v1.0.0.md`; o link de download do README (`../../releases/latest`) aponta para ele. A
    pedido do usuário, commits, releases e PRs não levam link de sessão nem rodapé do Claude; as mensagens dos
    primeiros commits foram reescritas sem essas linhas (mesmos arquivos) e reenviadas.

106. **Pnach completo e recarregado com o jogo rodando (2026-10-12).** Primeiro passo dos mods do usuário (o Coop
    Tool dele gera pnachs de times, IA, P5-P8, personagens e atributos).
    - **Tipos de código** (`ps2_pnach.cpp`, a lógica do `handle_extended_t` do PCSX2): além das escritas 0/1/2,
      incremento/decremento (3), escrita em série (4), cópia (5), ponteiro (6), OR/AND/XOR (7) e as condicionais `D`
      (próxima linha) e `E` (próximas n linhas), em ordem, com o estado de linha do PCSX2. O pnach principal do usuário
      tinha 6 linhas `E` que eram puladas; agora carrega inteiro (114 linhas, 0 puladas).
    - **Recarregar ao vivo:** uma thread confere a lista e as datas dos arquivos a cada segundo; quando muda, monta a
      lista nova e troca de uma vez (a thread do EE lê a lista a cada vblank), e as linhas `patch=0`/`2` rodam no
      vblank seguinte. A lista vem do `config.ini [Mods] Enabled` relido do disco (ou fica fixa se `PS2X_PNACH` foi
      definido à mão; aí só as datas dos arquivos são vigiadas). F4 recarrega na mão. Mensagem na tela
      "Patches reloaded" por 3 s. `PS2X_PNACH_RELOAD=0` desliga.
    - **Testado** numa cópia da pasta do jogo: ligar o pnach no `config.ini` com o jogo aberto recarregou (114
      linhas, aplicadas a cada vblank), editar o arquivo recarregou, desligar voltou a 0.
    - Desligar um pnach não desfaz o que ele já escreveu (como no PCSX2): vale até o jogo regravar o valor.

107. **Menu dentro do jogo (F2) com a aba Mods (2026-10-12).** Base para os mods do usuário (o Coop Tool vem depois,
    refeito do zero com ele). O jogo continua rodando com o menu aberto.
    - **Desenho:** pelo ImGui de dentro do `pcsx2gs.dll` (o mesmo do texto do F3), em `ImGuiManager::RenderOSD`, por
      cima da imagem. API do GS na versão 4: `set_menu(draw, user)` e `menu_input(...)`, e uma tabela C de interface
      (`Pcsx2GsUi`: janela, abas, textos, título, checkbox, botão, área com rolagem...) que o desenho usa sem depender
      da versão do ImGui; os mods em DLL vão usar a mesma tabela. Estilo escuro com o vermelho do jogo; a letra e a
      janela escalam com a altura da tela e nunca passam do tamanho dela.
    - **Runtime** (`ps2_game_menu.cpp`, `ps2x::menu`): F2 abre/fecha (e o X da janela), os painéis registrados viram
      abas; com o menu aberto o mouse, o teclado (setas, Enter, Espaço, Esc, Tab) e qualquer controle (direcional ou
      analógico, ✕/A confirma, ○/B volta, L1/R1) vão para o menu, e os controles do jogo ficam parados
      (`ps2_host_input::setBlocked`).
    - **Aba Mods** (`src/urban/mods.cpp`): os `.pnach` da pasta `mods` com nome (grupo `[...]` ou nome do arquivo),
      descrição (`comment=`) e número de linhas (lido de novo só quando o arquivo muda), checkbox que grava o
      `config.ini [Mods] Enabled` e recarrega na hora, "Recarregar (F4)" e "Abrir pasta de mods". Idioma pelo
      `[Launcher] Language` (`URBAN_LANGUAGE`). As funções do `[Mods]` saíram do `main.cpp` para esse módulo.
    - **Testado** numa cópia da pasta do jogo: F2 abre por cima da abertura; clicar no 4x4 gravou
      `Enabled=4x4.pnach`, recarregou (74 linhas, aplicadas a cada vblank) e mostrou "On: 4x4.pnach"; o jogo seguiu
      rodando. A navegação por controle não foi testada aqui (sem controle conectado no teste).
    - `PS2X_GAME_MENU=0` desliga.

108. **OBS: captura de janela preta (2026-10-12).** O usuário: o OBS só grava o jogo capturando o monitor; pela
    janela fica preto. Reproduzido com os dois métodos da captura de janela do OBS: BitBlt (o que o "Automático" usa
    nessa janela) sai vazio, porque a imagem é um swap chain DX11 em modo flip (como em muitos jogos atuais), que o
    GDI não enxerga; o Windows Graphics Capture ("Windows 10 (1903 e posterior)") sai com a imagem certa. Fazer o
    BitBlt funcionar exigiria copiar cada quadro para o GDI (custo por quadro): fica a orientação nos READMEs.

109. **Mods em DLL e o menu "MODS / PATCHES" (2026-10-12).** Pedido do usuário: a janela do F2 se chama
    "MODS / PATCHES" e tem uma aba por mod em DLL ligado; cada mod é uma DLL pronta, com os patches embutidos, ligada no
    launcher. Os `.pnach` soltos continuam (`config.ini`, F4), mas saíram do menu.
    - **Kit** (`modsdk/`): `include/urban_mod_api.h` (C, versão 1) e `example/` (ExampleMod: dois grupos de patches,
      aba própria com a vida ao vivo e um botão, código por quadro). O exemplo compila junto com o jogo
      (`URBAN_BUILD_MOD_EXAMPLE`) para `bin/mods/ExampleMod`.
    - **API:** `add_patches` (texto pnach embutido; com `show` vira checkbox salvo no `settings.ini` do mod, por
      posição, não pelo nome traduzido), `set_patches_enabled`, `read`/`write` na RAM do EE, `set_tab` (desenha com
      `UrbanModUi`, a mesma tabela `Pcsx2GsUi` da DLL do GS), `on_frame` (thread do EE, depois dos patches),
      `get_setting`/`set_setting`, `log`, `language`.
    - **Motor de patches:** além dos arquivos, grupos vindos dos mods (`ps2x::pnach::addGroup`), aplicados enquanto
      ligados (a lista em uso junta arquivos e grupos); `addFrameHook` para o código por quadro.
    - **Carregador** (`src/urban/mod_loader.cpp`): `config.ini [Mods] Dlls` (`URBAN_MOD_DLLS`), `LoadLibrary` de
      `mods\<Nome>\<Nome>.dll`, `urban_mod_init`; descrição do `mod.json` (`description`/`description_pt`); um
      travamento dentro do mod (SEH) para só o mod e a aba avisa. Sem mod ligado o menu diz como ligar.
    - **Testado** na cópia da pasta do jogo: o ExampleMod carregou ("2 patch options, own tab, frame code"); F2 mostrou
      a aba com a descrição, os checkboxes, a vida ao vivo e o contador de quadros; marcar "Infinite health (P1)"
      ligou o grupo, a vida do P1 foi para 1500 na leitura e a escolha ficou no `settings.ini`.

110. **Release com os mods (2026-10-13).** O release agora leva os mods compilados com o jogo (`game\mods\<Nome>`, dll
    + `mod.json`, desligados) e a nota da pasta `mods` fala só de mods em DLL; os `.pnach` soltos continuam
    funcionando pelo `config.ini` (`[Mods] Enabled`) e pelo F4.

111. **CoopTool como mod em DLL (`mods/CoopTool`, 2026-10-13).** Multiplayer com até 8 lutadores, refeito do zero a
    partir do mapa de memória do Coop Tool do usuário (`pnach_gen.py`): 1–4 humanos, "cada um por si" ou times
    personalizados, os bots (P2–P4 são CPUs da seleção; P5–P8 bots extras com lutador, time, força e vida), opções
    globais "Forçar P4 como Brad" e "Permitir lutador repetido", e uma aba "Como usar" com as limitações. Escreve a cada
    quadro (`on_frame`); KO regional e KO por ataque do time ficam desligados enquanto ele está ligado.
    - **Câmera voando / cena preta:** ligar o mod com uma luta já rodando (teste com `fight1v1.state`) ligava P5–P8 no
      meio da luta; o jogo só monta os lutadores quando a luta carrega, e a câmera tentava enquadrar lutadores sem
      lugar na cena. O trace (`COOPTOOL_TRACE=1`) mostrou a ordem: na carga o jogo zera o `on` e a IA (`-0xB0`) do
      P5–P8 e lê o `on` logo depois (um extra que estava ligado é regravado nesse quadro e entra).
    - **Só pegava na primeira luta:** a 1ª versão usava a missão `0x94D16C = 0xFFFF` como "luta rodando", mas ela
      continua `0xFFFF` na seleção e no setup depois da primeira luta. Comparando a RAM de 3 estados de menu e 4 de luta
      (e um trace ao vivo de boot → seleção → luta → pausa → restart → seleção → setup) o sinal certo é a **cena em
      `0x9449F8`** (32 bits): 0–3 boot/título/menus, 4 seleção/setup/config do multiplayer, 5 luta (também pausada e no
      restart). Regra: fora da luta os extras são ligados à vontade; na luta só os que já estavam ligados antes dela (ou
      que o jogo já montou). Um extra ligado no meio da luta entra na próxima, e o **restart** não conta: ele mantém os
      modelos carregados, e ligar um extra nele também deixava a cena preta.
    - **Save state:** carregar um estado (F9) trocava o momento do jogo e o mod achava que o extra já estava ligado
      antes. A API dos mods foi para a **versão 2** com `state_loads()` (contador de loads,
      `ps2x::savestate::loadCount()`); o CoopTool recomeça o acompanhamento dos extras quando ele muda.
    - **Opções novas (pedido do usuário):** IA de cada bot (Padrão, níveis 1–6 e os perfis da história do Coop Tool),
      força e vida também dos humanos, e por lutador "SPA infinito" (SPA = 1000 a cada quadro) e "Membros não quebram"
      (cabeça/tronco/pernas em `+0x04/+0x06/+0x08` = a resistência HDE/UBE/LBE; o `0xFF` fixo do pnach deixava pela
      metade quem tem resistência 500). Settings: `humanN`/`botN = lutador,time,força,vida,ia,spa,membros`.
    - `COOPTOOL_LIVE=1` liga os extras a qualquer momento (jeito antigo).
    - **Testado** (cópia isolada, settings com 1 humano + 5 bots): `select4` → luta com os extras; pausa → setup →
      seleção → 2ª luta com os extras; restart com os extras; `fight1v1` (luta rodando) → cena normal, restart normal e
      sem extras, seleção → luta com os extras; `fight8_heli` mantém os 8. Na RAM: P1 vida 2250/2250 (x1.5), SPA 1000,
      membros 500 (= resistência), atributos 1000 (Forte); bot P2 com IA `0x0004001B` (nível 5).
    - Depois, a pedido: "SPA infinito" só para os humanos (CPUs e bots sem a opção).

112. **CoopTool: fases do Free Mode no Multiplayer (experimental, 2026-10-13).** O usuário queria lutar no Multiplayer
    nas fases do Free Mode (o menu só tem 7).
    - **Como o jogo escolhe a fase:** o Multiplayer Config guarda o índice do menu em `0x943D98` (0–5, 7 = Random) e o
      ID da fase em `0x94A671` (byte; `0xFF` = sortear). Na carga da luta, `0x2407C0` copia esse byte para o ID da luta
      `0x5E8C88` (via `0x238BB0`; achado com `URBAN_HW_WATCH`). Os IDs são os mesmos do Free Mode (saves do usuário:
      Back Alley 10, Chinatown 0, Abandoned Theater 24; luz `bgNN.lit`, NN = ID + 1). Nomes em ordem na lista
      `$P1...` em `0x4543xx`: 0 Chinatown ... 19 Bordin Bldg. Rooftop ... 30 Chinese Market, 31 Scrapyard. Fases do
      menu: 19, 3, 27, 28, 29, 30.
    - **Por que as do Free Mode travavam no "Now Loading":** com o watchpoint de leitura novo (`URBAN_HW_WATCH_RW=1`)
      a carga forçada parava logo depois de `0x22C0C0`: o Multiplayer pede `afs/safe/ppk/stage%03d.ppk` (ID + 1, string
      em `0x6B9D78`), que só existe para as fases dele; o Free Mode pede `mission%03d.ppk` (`0x6B9D58`). Nenhuma leitura
      de disco acontecia: a busca do arquivo falhava e a carga esperava para sempre.
    - **Solução:** com uma fase escolhida no mod, ele grava o ID em `0x94A671` e troca o nome em `0x6B9D78` pelo pacote
      de uma missão daquela fase (`mission012.ppk` etc.; tabela missão → fase de 100 entradas em `0x60C228`, 16 bits).
      Ao desligar, volta o nome original e o byte da fase do menu.
    - **Todos nasciam no mesmo ponto** (o pacote da missão não tem os pontos do versus). A posição do lutador é
      `obj+0x10` (x, y, z float, metros; `obj` = vida − `0x8E28`, ponteiros em `0x59D168`), direção em `obj+0x20`
      (voltas; 0.25 = olhando +x). Os outros vetores (`-0x518`, `0x2A8`...) são ossos recalculados a cada quadro.
      Quando P1–P4 estão exatamente no mesmo ponto numa luta na fase escolhida (início de round), o mod espalha todos
      pelos pontos da missão (parceiro + inimigos, mm, do `missions_original.json` do Coop Tool), só os até 6 m do
      ponto do meio, completando com pontos nas linhas entre eles, virados para o centro.
    - **Testado:** Back Alley (missão 12) com 6 lutadores espalhados e câmera normal; Bordin Bldg. Top Floor (100);
      Chinatown: a missão 25 trava na carga, a 2 funciona. Abandoned Theater com a missão 75 carrega e fecha o jogo
      segundos depois (chamada virtual por um objeto inválido em `0x1E0D7C`, objeto de missão que o Multiplayer não
      inicializa); trocado para a 87, sem teste. Por pedido do usuário as outras fases não foram testadas uma a uma: a
      opção fica marcada como experimental, com aviso na aba. `COOPTOOL_MISSION=<n>` testa outro pacote.
    - **Fase e opções globais independentes** (pedido do usuário): o "Ligado" virou "Jogadores e bots ligados" e só
      vale para as seções 1–4 (e o KO regional desligado). Fase, "Forçar P4 como Brad" e "Lutador repetido" funcionam
      sozinhos, mas só no Multiplayer: o modo fica em `0x59D0C0` (1 = história/Free Mode, 2–4 = Multiplayer, os modos
      que carregam `stage%03d.ppk`); fora dele a fase e o nome do pacote voltam ao original e o P4 não é tocado (lá ele
      é inimigo). "Lutador repetido" agora vem desligado por padrão. Testado: jogadores/bots desligados + Back Alley →
      luta só com os 4 da seleção, espalhados.
    - **Bug do agarrão** (relatado pelo usuário): num agarrão o jogo cola os dois lutadores no mesmo ponto, e a regra
      "P1–P4 no mesmo ponto = início de round" espalhava no meio do golpe (o agarrado ficava voando). Agora só há uma
      janela de 5 s, aberta quando a luta carrega e quando a vida de todos volta ao máximo depois de alguém apanhar
      (próximo round, restart; vida comparada com sinal, nocaute fica abaixo de 0), e ela fecha depois de espalhar.
      Testado: Back Alley, 30 s de luta com os CPUs, espalhou uma vez só.

113. **Ícone do `urban.exe` (2026-10-13).** O mesmo do launcher: `src/res/urban.ico` + `src/res/urban.rc` (recurso
    `GLFW_ICON`, que o GLFW do raylib usa na janela e na barra de tarefas; o Explorer mostra o mesmo). Conferido no
    Explorer e na barra de título.
    - **Paredes transparentes perto da câmera (só olhado por cima):** o executável tem os menus de depuração dos
      desenvolvedores em `0x6AFD00`–`0x6B0900` (montados em `0x1BDE40` com `func_1B6988`/`func_1B6F90`): "COLLI MENU"
      com WALL DISP, **CAMWALL DISP**, FLOOR DISP; "OBJ MENU" com CULLING e Z SORT; RT/AO/CH MENU com CAMERA,
      THROW CAM, CAMERA SPRING, WALL. A grade de colisão (`kg_colligrid.cpp`) tem um tipo "camwall" separado das
      paredes; o esconder/apagar parede deve vir de dados por fase (por isso só funciona em algumas).
    - **Ligar o menu de depuração (investigado, não feito):** ele é uma tarefa C++ com vtable em `0x6A8018` (init
      `0x1B7828`, update+desenho `0x1B7898`, draw vazio `0x1B7A18`) que nenhum código do jogo final cria. Daria para
      rodá-la pelo callback opcional das tarefas (o método base `0x123678` chama `[obj+0x1C]`; `0x1236A0` → `+0x18`,
      `0x123628` → `+0x10`; objetos vivos o tempo todo com esses campos zerados, ex. `0xADE400`, vtable `0x6A7FB8`).
      Mas o init carrega a fonte do menu do PC dos desenvolvedores, `HST:/team/f090/data/rt/font.p2t` (`func_145A50`,
      handle em `0x451640`), que não existe no disco (o único "font.p2t" da ISO é o texto no executável): sem ela o
      update não desenha nada. Precisaria criar uma fonte nesse formato ou trocar o desenho pela fonte do jogo.

114. **CoopTool: aba Free Mode (2026-10-13).** O "Free Mode até 4 jogadores" do Coop Tool do usuário, sem o modo ondas
    (pedido dele): quantos jogadores (P2 = parceiro no controle 2 ou IA, P3/P4 = controles 3/4 no time do P1), missão
    (1–100, com a fase e os inimigos), quantidade de inimigos, objetivo (original, derrotar todos, um alvo, quebrar
    pernas/tronco/cabeça do alvo) e por lutador personagem (P3+), IA, força, vida, SPA infinito e membros.
    - **Dados:** `mods/CoopTool/gen_missions.py` gera `freemode_missions.h` a partir do `pnach_gen.py` do Coop Tool
      (`missions_original.json` + `freemode_missions.json`): as escritas extras do cheat de 4 jogadores e os 6 registros
      da lista de inimigos (`0x1DA8654`, `0xAC` bytes; cópias dos inimigos espalhadas pela arena). Fase de cada missão
      da tabela do jogo em `0x60C228`.
    - **Por quadro**, só com o modo `0x59D0C0` = 1 e a missão `0x94D16C` = a escolhida − 1: lista e quantidade
      (`0x1DA8528`/`0x1DA8650`), vagas sem uso (vida máx. 1500) derrubadas, `0xABFA74` (P2 no controle 2), P3/P4
      humanos (time 0, byte `-0x46` = 0, SPA regain); quando todos os inimigos caem os P3/P4 humanos caem também (nas
      missões de membros, 23/87 pernas e 65 tronco, o membro deles quebra). Vida multiplicada uma vez sobre a vida máx.
      que o jogo deu (no Free Mode não é 1500). Com a aba ligada, a parte de jogadores e bots do Multiplayer fica parada
      nesse modo.
    - **Parceiro:** só é forçado (IA `0x390039` e o trecho `0x1DA848C`–`0x1DA8527`, com `0x1DA84A0 = 0xFFFE`) com a
      opção marcada; desligada, ele vem como no jogo e o menu da missão pode tirar.
    - **Enhanced camera** (a câmera alternativa do Coop Tool, `0x4616C0` = 8 / `0x4614D0` = 5): L3 no controle 1 liga,
      R3 desliga (antes R2+L3 / R2+R3), só no Free Mode (os outros modos já têm essa câmera).
    - Testado pelo usuário no jogo; antes, um programa com host falso (RAM de 32 MB) conferiu as escritas e a aba.

## Ferramentas

Ver a tabela de variáveis de ambiente no README. As mais úteis até agora:
`URBAN_HANG_DUMP` + `URBAN_DUMP_MEM`, `URBAN_TRACE=HOOK`, `URBAN_CALL_TRACE`, `URBAN_HW_WATCH` e `PS2X_SCHED_STATS`.

Para o GS:
- `PS2X_GS_SIGNAL_LOG=1` (runtime): SIGNAL/FINISH/LABEL com o `CSR` encontrado, liberações da fila e interrupções de GS.
- `PS2X_GS_TRACE=primeiro:quantidade:arquivo`: desenhos (`D`) e registradores de exibição (`V`) por tick.
- `URBAN_DUMP_RAM` / `URBAN_DUMP_VRAM` + `tools/vif_chain.py` (chains de VIF1/GIF, códigos VIF, pacotes GIF) e
  `tools/gs_vram.py` (VRAM → PNG; use `PS2X_GS_GPU=0` para a VRAM do host ser a verdadeira).
- `tools/win/capture_window.ps1 -TitleContains "Urban Reign" -OutFile x.png`: captura a janela (também a do PCSX2).
Para o PCSX2: `pcsx2-qt.exe -nogui -fastboot -- "<iso>"` mais `tools/pine.py` (PINE na porta 28011).
