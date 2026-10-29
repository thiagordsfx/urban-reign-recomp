# Roadmap de desempenho — Urban Reign (PS2) recompilado

Este arquivo é para um agente que chega **sem contexto**. Leia inteiro antes de mexer em qualquer coisa. O histórico
detalhado está em `docs/PROGRESS.md` (os itens §50-§57 são os mais recentes); este roadmap diz o que fazer a seguir, em
que ordem e como provar cada passo.

Escrito em 2026-10-08, depois da primeira medição com 8 lutadores (PROGRESS §57).
**Andamento (2026-10-11):** A1-A4, C1-C5, C3b (leve), a trava de vblank no monitor, a **resolução interna** e a
**B1** feitos (PROGRESS §58-73). Decisões novas no §7.2: troca do GS pelo do PCSX2; agora folga de CPU (C7, C6, C8).
**fight8_heli passa nos 4 itens do critério de 60 liso:** 60,0 quadros/s, 59,8-59,9 distintas/s, 100% mostradas 1×,
velocidade 100%, pior quadro 19,2 ms. Era 47-49,5 quadros/s. Itens novos C3b/C6/C7/C8 e a comparação com o PCSX2 no §7.1.
Revisado em 2026-10-10: o requisito de CPU passou a ser o mínimo do PCSX2 (SSE4.1), com **um executável só** (§4,
A4); entraram a Fase E (renderizador nativo, só se o plano não bastar), o FMV pelo IPU (§9) e os projetos de
referência (§10).

---

## 1. O projeto em 1 minuto

- Port nativo de Urban Reign (PS2, SLUS-21209) por **recompilação estática**: o ELF do EE (MIPS R5900) vira C++
  (`generated/`, ~10 mil arquivos), compilado com MSVC. Não é decompilação.
- Runtime próprio: o PS2Recomp (fork do port do SotC) dentro do repositório, em `runtime/`, com as mudanças do port
  (`runtime/README.md`; desde 2026-10-12, §90, não é mais submódulo nem patch).
  - `ps2xRuntime/`: EE e kernel HLE, GS, VIF, VU, DMA, SIF;
  - `ps2xIOP/`: emulador do IOP, que roda os IRX reais do jogo.
- O código do port fica em `src/` (hooks, HLE, overlay, profiler); o GS do PCSX2 em `thirdparty/` (só os arquivos usados).
- **Threads:**

  | thread | o que faz |
  |---|---|
  | `GameThread` | código do EE recompilado + escalonador + VU0 + handlers |
  | `VUThread` | VIF1 + microprogramas do VU1 compilados AOT (`generated/vu1`) — "MTVU" |
  | `GIFThread` | decodifica pacotes GIF (front end do GS) |
  | `GSThread` | GS do PCSX2 (DX11; com `Renderer=native`, o backend OpenGL próprio) |
  | `IOPThread` | emulador do IOP em lotes |
  | thread principal | janela/present (raylib) |

- **Usuário:** fala PT-BR, testa à mão, é modder do Urban (mods de 4-8 jogadores via .pnach). Máquina: Ryzen 5 3600
  (6 núcleos/12 threads, 2 CCX de 3) + GTX 1660 Super. **O port tem que rodar em qualquer PC que rode o PCSX2**
  (CPU com SSE4.1, §4).

## 2. Regras de trabalho (do usuário — obedecer)

- **Sem commits.** Documente em `docs/PROGRESS.md` (novo item numerado). (Não há mais patch do runtime para
  reexportar: o `runtime/` faz parte do repositório, §90.)
- **Toda mudança tem chave para desligar** (variável de ambiente) e é medida antes/depois isoladamente.
- **Não compile com o jogo aberto** (o exe fica travado e o link falha calado). Confira a data de
  `build/port/bin/urban.exe` depois de cada build. O build pode filtrar a saída com
  `grep -aE "error C|error LNK|Linking CXX executable"`.
- PCSX2 é só leitura (não mexer em ini, cheats ou memory cards).
- Não reverter edições do usuário no `build/port/bin/controls.ini` (antes `controles.ini`) nem no `config.ini`.
- **Depois de cada passo do plano, reporte os 4 itens do critério (§4) e a tabela por thread (§5.2).**

## 3. Como compilar e rodar

- Build: `python build.py build` (Ninja, `build/port`, RelWithDebInfo, `-j 8`); `python build.py` sem argumentos abre
  o menu numerado. Benchmark: `python build.py bench` (savestates em `bench/states`, resultados em `bench/runs`).
  Mudar headers incluídos pelo código gerado (`ps2_runtime.h`, `ee_scheduler.h`, `ps2_memory.h`, headers do IOP
  incluídos por eles) recompila os ~10 mil arquivos: mais de 30 min. Prefira estado `static` em `.cpp` ou headers novos.
- Exe: `build/port/bin/urban.exe`. O `controls.ini` ao lado dele define variáveis de ambiente (as do ambiente
  real vencem).
- Pnach: `PS2X_PNACH=<arq>[;<arq>]` (PROGRESS §56). O controls.ini do usuário já liga o de 8 jogadores
  (`C:/Users/thiago/Documents/PCSX2/cheats/SLUS-21209_BDD9BAAD.pnach`).

## 4. Meta e critério de "60 liso"

- **Configuração mínima (meta):**
  - **8 lutadores a 60 lisos** no heliporto num **Ryzen 5 3600 + GTX 1660 Super** (a máquina do usuário);
  - **2-4 lutadores a 60** numa máquina de **4 núcleos/8 threads**.
- **Requisito de CPU = o mínimo do PCSX2:** x64 com **SSE4.1** (o PCSX2 pede SSE4.1 no mínimo e recomenda AVX2).
  Isso inclui Pentium/Celeron recentes sem AVX. Não exigir AVX nem AVX2 para abrir o jogo.
- **Um executável do jogo só** (`urban.exe`), sem builds separados por processador. O que usar AVX2 escolhe o caminho
  em tempo de execução por CPUID, como o PCSX2 faz (A4).
- **Passa se**, medido por ≥30 s a partir do state:
  1. ≥ 59,5 imagens distintas/s;
  2. ≥ 99% das imagens mostradas uma única vez;
  3. velocidade 100%;
  4. pior quadro < 25 ms, sem picos.

## 5. Como medir

### 5.1 Cena e critério
```
python tools/autotest/bench_state.py --state bench/states/fight8_heli.state --measure 32 --tag <tag> \
  --pnach "C:/Users/thiago/Documents/PCSX2/cheats/SLUS-21209_BDD9BAAD.pnach" \
  --env "PS2X_FRAME_TIMES=H:/JOGOS/UrbanRecomp/build/autotest/<tag>.csv" --env PS2X_FRAME_HASH=1
python tools/pacing_summary.py build/autotest/<tag>.csv 18 52     # present, p99, pior, velocidade, distintas/s
python tools/image_cadence.py build/autotest/<tag>.csv 18 52      # % de imagens mostradas 1, 2, 3+ vezes
# o que o monitor mostrou (§96): --env "PS2X_PRESENT_STATS=H:/JOGOS/UrbanRecomp/build/autotest/<tag>_stats.csv"
python tools/present_stats.py build/autotest/<tag>_stats.csv 18 52    # imagens repetidas/puladas na tela
```
- `fight8_heli.state` = 1 humano + 7 CPU no heliporto, salvo pelo usuário com o pnach ativo. O bench liga o pnach,
  confere no log que foi aplicado e falha se não foi.
- Sem `--pnach`, o bench roda com o pnach desligado (`PS2X_PNACH=none`), mesmo que o controls.ini o ligue.
- Outras cenas:
  - `fight4_heavy2.state`: heliporto com 4 lutadores;
  - `fight4_heavy.state`: outro mapa pesado;
  - `select4.state`: seleção de personagens.
- Tudo é sempre medido em progressivo (o bench força `URBAN_PROGRESSIVE=1`).
- **Ruído:** ±1-2 quadros/s entre execuções; a máquina às vezes está mais carregada. Compare antes/depois na mesma
  sessão.
- **Máquina menor:** restringir a afinidade do processo depois de lançado (psutil
  `Process(pid).cpu_affinity([0,1,2,3])`). No 3600, 0-3 = 2 núcleos/4 threads; 0-7 = 4 núcleos/8 threads.
  Reportar os dois. O bench ainda não tem essa opção; adicione `--affinity`.

### 5.2 Tabela por thread
- `python tools/thread_usage.py --process urban.exe --seconds 15 --interval 5`, rodado enquanto o bench mede.
  Dá o % de um núcleo por thread; ms/quadro = % × duração do quadro.
- Profiler nativo: `--env URBAN_NATIVE_PROFILE=21:15 --env URBAN_NATIVE_PROFILE_STACKS=1`. Imprime no
  `urban.err` o tempo próprio e o inclusivo por thread, mais a GameThread por subsistema.
  `URBAN_NATIVE_PROFILE_LINES=<n>` dá as linhas mais quentes.
- Esperas por motivo: `--env PS2X_MTVU_TRACE=24` grava `build/port/bin/mtvu_trace.txt` (200 mil eventos); depois
  `python tools/frame_waits.py <trace>`.
  - Códigos do trace:
    - `F` quadro (ciclo do EE/1024);
    - `V` vblank;
    - `W/w` sync com os workers;
    - `P/p` sono de ritmo;
    - `X/x` invocação de handler (`Y` = ciclo);
    - `H` SIGNAL (label);
    - `K` kick de VIF;
    - `C` troca de render target;
    - `R` present.
  - Thread: `e` jogo, `v` VU, `g` GIF.
- GPU: `--env PS2X_GS_GPU_STATS=1` imprime `[gs-gpu] ... GPU interval X ms/frame`. Atenção: é `TIME_ELAPSED` de
  present a present e **inclui a GPU parada**, então é só um teto.
- IOP: `--env PS2X_IOP_THREAD_STATS=1` (lotes/s, worker ocupado, espera de join do EE).

## 6. Onde estamos (linha de base, PROGRESS §57)

**fight8_heli, sem otimizar:** 47,3 quadros/s, velocidade 79%, 46,5 distintas/s, 98% mostradas uma vez; present
mediana 21,0 / p99 28,4 / pior 40,8 ms. Reprova nos 4 itens.

| thread | CPU | ms/quadro (de 20,8) | nota |
|---|---|---|---|
| **GameThread** | 88% | ~18,3 | **limita**: sono de ritmo 0, espera de sync no VU/GIF 0,05 ms/quadro |
| VUThread | 47% | ~9,8 | folga |
| GSThread | 36% | ~7,5 | +3,6 ms/quadro esperando fences da GPU |
| present | 35% | ~7,3 | leitura GPU→CPU→GPU todo quadro |
| GIFThread | 28% | ~5,8 | folga |
| IOPThread | 98% | ~4 de trabalho | **77% é espera ativa** em `IopSubsystem::Impl::workerLoop` |
| GPU | — | ≤16-18 (teto) | não separado da ociosidade |

**GameThread por dentro** (inclusivo):

| parte | % |
|---|---|
| VU0 (`executeVU0Microprogram`) | 22,5 |
| espera de download de VRAM (`refillVif1DownloadFifo` → `GS::consumeLocalToHostBytes`) | 10,6 |
| memmove | 8 |
| overhead do runtime (`accountCycles` 4,8, `checkpointDue` + `eeCheckpointDue` 3,3, `dispatchGuestBranch` 2,4...) | ~12 |
| decodificador de animação `sub_00292670` | 5,9 |
| handler de GS `0x10bc38` (1,6 ms/quadro) | 5,5 |
| RPC do IOP executado na thread do jogo (`SifCallRpc`, som ~100/s) | 5,3 |
| laços de espera do próprio jogo (`sub_00128DC0`, tempo próprio) | ≤2,6 |

Código do jogo propriamente dito: ~24%.

**Fatos que um agente novo precisa saber:**
- **AVX2:** `src/CMakeLists.txt` compila `urban_game_code`, `urban_game_code_huge` e `urban_vu1_code` com
  **`/arch:AVX2`**. Hoje o exe não roda em CPU sem AVX2 (a A4 resolve: base SSE4.1).
- **LTCG:** o link do `urban` tem `/LTCG` em RelWithDebInfo, mas nenhum alvo compila com `/GL`, então não há LTO de
  fato.
- **Present sem cópia:** existe atrás de `PS2X_GS_DIRECT_PRESENT=1` (`gs_gl.cpp`, `GSSharedPresent`), desligado por
  padrão. Não se sabe por que ficou desligado; descobrir antes de ligar.
- **Nomes de thread:** passam por `ThreadNaming::SetCurrentThreadName` (runtime); o IOP usa `SetThreadDescription`
  em `iop_subsystem.cpp`. É o ponto natural para aplicar afinidade e prioridade.
- **Giro do IOP:** está em `workerLoop` (`iop_subsystem.cpp`, ~linha 320). Gira até 2 ms sem `yield`, de
  propósito: o comentário diz que, com `yield`, o EE esperava o worker voltar em `join()`. O `join()` do lado do EE
  também gira (`PS2X_IOP_SPIN_PAUSE`).
- **Ciclos do EE:** o tempo emulado é cobrado por desvio para trás (`PS2X_EE_LOOP_CYCLES`; o port usa 16) e por
  chamada (8). Mudar isso mexe na fase dos quadros (PROGRESS §52).

## 7. Plano (em ordem)

Cada passo: medir **antes e depois** no fight8_heli (critério + tabela por thread) e também no `fight4_heavy2`/
`select4`, para não quebrar o que já está a 60. Uma chave por passo.

**Entre uma fase e outra (obrigatório):**
1. Critério completo no fight8_heli (§4, §5.1) + tabela por thread (§5.2).
2. Regressão funcional:
   - lutas (1v1, 4 lutadores);
   - modo história (entrar numa missão e jogar até o fim; `tools/autotest/drive.py --route story`, mas as
     referências de tela são entrelaçadas: confira os PNGs);
   - save/load no memory card (salvar num slot novo, fechar, abrir, carregar) numa cópia do cartão (`URBAN_MC_DIR`);
   - F5/F9 de savestate várias vezes (controle e música continuam);
   - áudio sem cortes novos.
3. Anotar tudo num item novo do PROGRESS.

### Fase A — consertos baratos de sistema

**A1. IOP sem espera ativa.**
- **O quê:** no `workerLoop`, girar só alguns µs (`pause`) e depois **dormir com `WaitOnAddress`** sobre o
  contador/flag de lote, com **`WakeByAddressSingle`** chamado pelo EE no instante em que posta um lote
  (`runEeCyclesAsync`).
  - **Não usar `yield`**: o comentário em `workerLoop` explica que um worker que cede pode levar um quantum inteiro
    para voltar, e o EE fica esperando em `join()`.
  - O mesmo vale no `join()` do lado do EE: girar alguns µs e depois `WaitOnAddress` sobre `busy`, com o worker
    chamando `WakeByAddressSingle` ao terminar.
- **Viabilidade:** `WaitOnAddress` existe desde o Windows 8 (`Synchronization.lib`). Se um dia houver porta para
  Linux, o equivalente é o futex.
- **Medir junto:** o RPC do IOP na GameThread (~5,3%: `SifCallRpc` → `IopSubsystem::handleRpc`). Ele chama `join()`
  antes de executar o servidor, então depende da mesma comunicação. Reportar o tempo inclusivo de `handleRpc` e a
  "EE join wait" (`PS2X_IOP_THREAD_STATS=1`) antes/depois.
- **Por quê:** a IOPThread queima ~1 núcleo lógico à toa (77% do tempo); numa máquina de 4 núcleos isso rouba CPU.
- **Chave:** `PS2X_IOP_SPIN=1` (comportamento antigo).
- **Verifica:** IOPThread perto dos ~18% de trabalho real; GameThread igual ou melhor; "EE join wait" não cresce.

**A2. Afinidade e prioridade por topologia** (nada fixo).
- **O quê:** ler a topologia em tempo de execução (`GetLogicalProcessorInformationEx`: núcleos físicos, SMT,
  grupos de L3/CCX). Regra genérica de 4 a 16 núcleos:
  - GameThread num núcleo físico sozinho (sem o irmão SMT ocupado por outra thread pesada) e com prioridade
    acima do normal;
  - VUThread, GIFThread e GSThread em outros núcleos físicos;
  - IOP e present onde sobrar.
- **No 3600:** testar GameThread + VUThread + GIFThread no mesmo CCX (L3 compartilhado) contra espalhado.
- **Timer:** `timeBeginPeriod(1)` só se o sono de ritmo precisar (hoje o sono de ritmo é 0 nessa cena).
- **Chave:** `PS2X_THREAD_POLICY=off|auto|ccx`.
- **Verifica:** critério + tabela; e o teste com afinidade 0-3 e 0-7 (§5.1).
- **Risco:** afinidade rígida piora em PCs com outras cargas. Preferir "ideal processor"
  (`SetThreadIdealProcessorEx`) a máscara rígida, e máscara só com a política `ccx`.

**A3. Present sem a leitura GPU→CPU→GPU.**
- **O quê:**
  - descobrir por que `PS2X_GS_DIRECT_PRESENT` está desligado (testar: tela inteira, overlay F3, screenshots, F6,
    F5/F9, minimizar/restaurar a janela) e ligar por padrão se não houver problema;
  - **no jogo normal, nenhuma leitura GPU→CPU** por quadro.
- **O hash de imagens distintas** (`PS2X_FRAME_HASH`) só existe no modo benchmark, e lá não pode bloquear:
  - ou leitura assíncrona (PBO + fence, alguns quadros de atraso; o mecanismo já existe em
    `GSGpuBackend::Impl::presentAsync`/`harvest`, com anel de readbacks);
  - ou o hash calculado na GPU (compute shader de redução; ver `PS2X_FRAME_HASH_DEVICE` em `gs_gl.cpp`, que pode
    já ser isso), lendo só 8 bytes.
  - O CSV de `PS2X_FRAME_TIMES` tem de associar o hash ao present certo mesmo com o atraso: gravar o número do
    present junto e casar depois.
- **Chave:** a própria `PS2X_GS_DIRECT_PRESENT`; o hash só com `PS2X_FRAME_HASH=1`.
- **Verifica:** a thread de present cai dos ~7,3 ms; `[gs-present] shared GPU textures active` no log; o critério de
  imagens distintas continua dando os mesmos números de antes numa cena conhecida (`select4`: ~59,4 distintas/s).

**A4. Compilação.**
- **Tirar o `/arch:AVX2` global** (requisito SSE4.1, §4). **Não usar AVX como base**: há Pentium/Celeron recentes
  sem AVX.
- **Decisão do usuário (2026-10-10): um executável do jogo só.** A ideia antiga de dois executáveis (AVX2 + SSE),
  um escolhido pelo processador na hora de abrir, saiu do plano: dobrava o build de >30 min e o disco.
- **Como:**
  1. **Base SSE4.1 em tudo** (código gerado, runtime, IOP, port). O código gerado é a maior parte do build e não
     ganha nada que valha um segundo build.
  2. **AVX2 só no código quente que medir ganho**, escolhido em tempo de execução: CPUID uma vez no boot e ponteiro
     de função (ou dois `.obj` do mesmo arquivo compilados com flags diferentes, com nomes diferentes). Candidatos,
     nesta ordem: VU1 AOT (`urban_vu1_code`), VU0, cópias grandes (memmove de DMA), conversões de textura/CLUT no GS.
  3. **Medir antes de manter:** build todo SSE4.1 contra o build AVX2 de hoje no fight8_heli. Se a diferença for
     ruído (±1-2 quadros/s), não vale nem o despacho: fica só SSE4.1.
  4. Log no boot: `cpu: SSE4.1 ok, AVX2 sim/não, caminho escolhido: ...`. Se a CPU não tiver SSE4.1, mostrar uma
     mensagem clara e sair (em vez de crashar com instrução ilegal).
- **Chave:** `PS2X_CPU_PATH=auto|sse41` (força o caminho base numa máquina com AVX2, para testar os dois na mesma
  máquina).
- **Fatos para decidir:**
  - O MSVC instalado é o **14.36** (VS 2022 17.6).
  - O `/arch` do MSVC x64 historicamente só aceita SSE2 (padrão), AVX, AVX2 e AVX512. `/arch:SSE4.2` só aparece em
    versões mais novas do VS 2022. Testar com `cl /arch:SSE4.2`; se não aceitar, a base é o padrão SSE2, e
    intrinsics SSE4 explícitos continuam valendo, porque o MSVC aceita qualquer intrinsic sem `/arch`.
  - O código do VU (`vu1_recomp`, `ps2_vu1_impl.inl`) só usa intrinsics SSE/SSE2 (`_mm_*_ps`, `movemask`, `shuffle`).
    O ganho do AVX2 hoje vem só do compilador (codificação VEX, auto-vetorização), o que pode ser pouco: medir.
  - **Exatidão:** conferir que o build AVX2 não gera FMA contraído (`/fp:contract`), porque mudaria resultados do VU.
    Rodar `PS2X_VU1_VERIFY=obs`/`PS2X_VU0_VERIFY=obs` nos dois builds.
- **LTO de verdade:** `/GL` no runtime, no IOP e no port (não no código gerado: são ~10 mil arquivos, o link com
  LTCG pode estourar memória e tempo). Medir.
- **PGO:** só depois, e talvez só no runtime/IOP (instrumentar o código gerado inteiro é caro). Ganho esperado
  ~4% (Bloodborne); decidir pelos números.
- **Chave:** opção do CMake (`URBAN_LTO=ON/OFF`, `URBAN_ARCH=SSE41|AVX2`). O padrão é `SSE41`; `AVX2` fica só para
  comparar desempenho, não é build de distribuição.

### Fase B — GPU (decide se o back end do GS vira prioridade)

**B1. Tempo real de GPU por quadro e por passe.** ✅ Feito (PROGRESS §73): `PS2X_GS_GPU_PROFILE=1`. Achou e corrigiu
~280 passes de 1 sprite (leitura com REGION_CLAMP) e ~320 dispatches de upload (pendentes por bloco). Luta de 8:
GPU 13,5 → 9,5 ms em 1x, 20,7 → 14,7 ms em 2x; 2x quase liso (59,7 q/s, 58,4 distintas). Falta: raster da cena em 3x+.
- **O quê:** timer queries (`GL_TIMESTAMP` em volta de cada lote/passe: desenho de triângulos, cópias, CLUT,
  composição/present, readbacks) e soma só do tempo ocupado, separando a ociosidade. Hoje só existe o intervalo de
  present a present.
- **Limiar do usuário:** se o nosso back end gastar **> ~8-10 ms reais a 640×448** na 1660 Super, o back end do GS
  passa a ser prioridade. O PCSX2 faz essa cena a 6× a 60 na mesma placa, e em GPUs AMD/Intel com OpenGL pior o
  nosso estouraria primeiro.
- **Pistas:**
  - 354 lotes e 213 cargas de CLUT por quadro;
  - a GSThread espera fences 3,6 ms por quadro;
  - `prepareTarget`, `addTriangle` e `markPages` aparecem no perfil da GSThread.
- **Chave:** `PS2X_GS_GPU_PROFILE=1`.
- **Saída:** tabela "passe → ms de GPU por quadro". Se um passe dominar, é o alvo.

**B2. Teste em GPU AMD (RX 580).**
- **Quando:** depois da B1, quando o usuário trocar a placa (ele tem uma RX 580).
- **Viabilidade:** a RX 580 (Polaris, GCN4) tem OpenGL 4.6 no driver Windows, com compute shaders,
  `ARB_buffer_storage` (buffers mapeados persistentes) e contextos compartilhados. Em princípio, tudo o que o back
  end usa. Riscos:
  - **Desempenho:** o OpenGL da AMD no Windows foi historicamente mais lento (melhorou muito no driver de 2022 em
    diante). É exatamente o caso que justifica o limite de ~8-10 ms da B1.
  - **Cache de shaders:** `PS2X_GS_SHADER_CACHE_DIR` guarda binários de programa da NVIDIA. Na AMD, o cache não serve
    e é refeito; o primeiro boot será lento, o que é normal.
  - **Comportamento:** diferenças de driver (precisão de inteiros em compute, sincronização de fences, formatos de
    imagem) podem mostrar bugs gráficos que não aparecem na NVIDIA.
- **O quê medir:**
  - critério + tabela por thread + `PS2X_GS_GPU_PROFILE` no fight8_heli e no select4;
  - comparar visualmente com a NVIDIA (screenshots das mesmas cenas: `tools/win/capture_window.ps1`);
  - anotar a versão do driver.
- **Resultado:** se a AMD estourar o orçamento de GPU e a NVIDIA não, o back end do GS sobe de prioridade (ou se
  avalia um back end Vulkan/paraLLEl-GS).

### Fase C — dieta da GameThread (a thread que limita)

Meta: de ~18,3 para ≤14 ms por quadro com 8 lutadores.

**C1. VU0 rápido por programa** (~22,5% → alvo −4 ms).
- **O quê:** hoje as imagens de VU0 rodam exatas (sem `fmacFast`). Rodar o caminho rápido, exceto nos programas
  ou pontos em que o EE lê o status do VU0 (flags MAC/status/clip via `CFC2`/`VCALLMS` + leitura).
- **Como:** um detector, igual ao das sticky flags, marca quais programas têm o status lido; esses ficam exatos.
- **Chave:** `PS2X_VU0_FAST=0`.
- **Verifica:** `PS2X_VU0_VERIFY=obs` com 0 divergência observável em seleção, luta e história. O gerador fica em
  `runtime/ps2xTest/tools/vu1_recomp.cpp`; regenerar só a partir de `build/vu_capture_all`, com
  `build/port/bin` no PATH.

**C2. Para que serve o download de cor 8×8?** (~10,6% de espera).
- **O quê:** identificar qual código do jogo lê a VRAM por VIF1 DIR=0, que área e quando. Provavelmente é teste de
  visibilidade de luz ou lens flare; comparar com o PCSX2.
- **Se for teste de visibilidade:** tornar assíncrono com chave. Já existe `PS2X_GS_ASYNC_READBACK=all` (resultado
  com um quadro de atraso; medido: espera 10,0% → 4,4% no heliporto com 4).
- **Verifica:** visual igual (flare aparece e some nos mesmos momentos) e a espera some do perfil.

**C3. Overhead do runtime** (~12%).
- `accountCycles` (por bloco: IOP, timers, checagens de interrupção), `checkpointDue`/`eeCheckpointDue`,
  `dispatchGuestBranch`, `pushDispatchPc`/`pushBranchEvent` (anel de depuração?), `translateAddress`.
- **Ideias:** cobrar ciclos em lote; tirar do caminho quente o que é depuração; inlining.
- **Chave por item.**
- **Cuidado:** ciclos cobrados mexem na fase dos quadros (§52). Manter o mesmo total por iteração de loop.

**C4. memmove evitável** (~8% na GameThread, e mais na GIF/GS/VU).
- Separar o que é cópia de segurança obrigatória (dados de DMA copiados no kick para o VU/GIF lerem depois, o que
  impede o jogo de reescrever o buffer cedo) do que é cópia dupla ou cópia de leitura.
- Só eliminar a segunda categoria.

**C5. (se ainda faltar) RPC de som na GameThread** (5,3%) e handler de GS (5,5%): ver se dá para não executar o
IOP inteiro dentro da chamada do EE (como o RPC NOWAIT assíncrono do §55).

**Itens novos (2026-10-10), depois da comparação com o PCSX2 (§7.1):**

**C3b. Contagem de ciclos inline no código gerado.** *(2026-10-11: feita a versão leve, dentro do `checkpointDue`, sem regenerar, com quase todo o ganho; PROGRESS §71. A versão inline tiraria só ~1% a mais.)*
- **O quê:** o recompilador (`ps2xRecomp`) passa a gerar o desconto de ciclos dentro do código: um contador no
  contexto, uma subtração por desvio para trás ou chamada, e só uma chamada ao escalonador quando o contador chega
  a zero. É o que o JIT do PCSX2 faz.
- **Hoje:** cada desvio para trás chama `eeCheckpointDue → checkpointDue → accountCycles`, e cada JAL chama
  `dispatchGuestBranch → checkpointDue`. São ~12% da GameThread.
- **Custo:** muda header do PCH e exige regenerar o código. Build completo ~35 min com `-j 8`.
- **Cuidado:** o mesmo total de ciclos por iteração (§52). O orçamento do contador = min(próximo prazo, fim da
  fatia de tempo, próximo evento de timer, checagem de ritmo).
- **Chave:** opção do gerador + `PS2X_EE_INLINE_CYCLES=0` no runtime.
- **Feito em 2026-10-11 (PROGRESS §76):** inline no PCH foi pior (+540 KB, GameThread +4 pontos); ficou o teste de
  orçamento no começo do `eeCheckpointDue` (chamada normal): 2 núcleos 53,7 → 54,6 q/s. `PS2X_EE_INLINE_CHECKPOINT=0`.

**C6. VU0 sem cópia de registradores.** ~4.500 VCALLMS por quadro. Cada um ainda copia os 32 VF e o ACC do contexto
do EE para o estado do VU0 e de volta (`loadVu0Registers` + `copyVu0StateToContext`; o VU0 inteiro é ~20% da
GameThread). Fazer o programa AOT do VU0 trabalhar direto sobre `ctx->vu0_vf`, ou manter um estado único, como o
microVU do PCSX2. **Chave:** `PS2X_VU0_ZERO_COPY=0`.
**2026-10-11 (PROGRESS §79):** cópia por máscara testada (análise por entrada, 0 erros) e sem ganho; desfeita. O custo
restante é a normalização dos registradores de entrada e o microprograma em si.

**C7. IOP: HLE do driver de som.** O IOP é um interpretador que roda os IRX reais (42% de um núcleo, mais o RPC na
GameThread). O Ace Combat 5 (§10) tem o NUSOUND em HLE: mesmo driver da Namco, versão de 2004. Portá-lo tira a maior
parte do trabalho do IOP. O streaming ADX (CRI) não tem referência pronta e continua emulado. **Chave:**
`PS2X_IOP_HLE_NUSOUND=0`.
**Revisto em 2026-10-11 (PROGRESS §74):** o NUSOUND é só ~17% das instruções do IOP; ~66% são do CRI_ADXI (servidor do
ADX). O trabalho real do IOP é 15-30% de um núcleo. O C7 sai da frente da fila.

**C8. VU1: alocação de registradores no gerador.** Os itens (b)/(d) do §45, que estavam fora do plano porque a
VUThread tinha folga. Com 8 lutadores ela está em ~51% contra ~18% do MTVU do PCSX2; importa para máquinas de 4
núcleos.
- **Parte 1 feita (PROGRESS §77):** só os tempos de pronto que ainda importam (3798 → 653 gravações), VUThread −6 pontos,
  2 núcleos 54,2 → 55,6 q/s. Falta: manter os registradores entre segmentos (hoje cada segmento grava todos os que
  escreveu, valor bruto + normalizado, e o seguinte os recarrega).

### 7.1 Comparação com o PCSX2 (2026-10-10, PROGRESS §65)

O PCSX2 2.x (DX12, 6× de resolução, MTVU) na luta de 8 no heliporto, no PC do usuário, medido com
`tools/thread_usage.py`; threads identificadas pelo ponto de execução (JIT, driver):

| função | PCSX2 | port (C3) |
|---|---|---|
| EE (+ VU0, IOP e SPU2 no PCSX2) | EmuThread **43%** | GameThread **95%** + IOPThread **42%** |
| VU1 | MTVU **18%** | VUThread **51%** |
| GS | thread GS **25%** (com 6×) | GSThread **82%** + GIFThread **43%** |
| processo | **~86%** | **~320%** |

**Conclusão:** a diferença não está no código do jogo (já nativo, ~25-30% da GameThread), e sim nas camadas de
emulação. Não precisa descompilar o jogo. Alvos realistas:
- **GameThread** ~55-65%: C3b, C4, C5/C7, C6;
- **IOP** ~10-15%: C7;
- **VU1** ~30-35%: C8;
- **GS + GIF** ~60% otimizando o nosso back end (perfil da GSThread/GIFThread: decodificação de GIF, `addTriangle`,
  `markPages`, `prepareTarget`). ~25-30% só trocando o back end pelo renderizador do PCSX2 (GSdx, GPL) ou pelo
  paraLLEl-GS. A troca também traria a resolução interna alta. Decidir depois de otimizar o nosso e da B1.

**Ordem combinada com o usuário:** C5 → C4 → C3b → D → (resolução interna) → B1 → GS (otimizar, depois decidir a troca).
A resolução interna foi feita antes da D, a pedido do usuário (PROGRESS §72): `config.ini` `[Video] ResolucaoInterna`
(1-8, padrão 1). 1x ficou igual e mais leve (GSThread ~80% → ~53% com 8 lutadores). 2x e 3x dão 60 lisos no select
e 2x quase no fight4_heavy2; com 8 lutadores o 2x fica em ~42-45 apresentações/s, limitado pelo GPU (o nosso GS
emula tudo por amostra). Esse custo entra na B1 (medir por passe) e na fase GS.

### 7.2 Decisões de 2026-10-11 (depois da B1)

- **O back end do GS vai ser trocado pelo renderizador de hardware do PCSX2** (GPL-3, compatível). O nosso GS emula
  o GS pixel a pixel num shader; a resolução interna multiplica esse trabalho por amostra (2x = 4 amostras, 4x = 16),
  então 4x com 8 lutadores não cabe (2x ~13 ms de raster, 4x estimado ~50 ms). O do PCSX2 converte os desenhos em
  desenhos nativos de GPU (texturas, Z e mistura do hardware) e traz a substituição de texturas HD pronta. O GS do
  AC5 é feito sob medida para aquele jogo (serve de referência); o ICO não emula GS (port de decompilação).
  O nosso GS atual fica como opção exata em 1x/2x.
- **Ficam para depois da troca do GS:** bugs visíveis (§9: FMV/IPU, gráficos faltando, pontilhado, trava rara na
  abertura), **widescreen de verdade** (renderizar mais área, não esticar o 4:3 como o patch do PCSX2), texturas HD,
  Fase D.
- **Engasgos de loading/história:** acontecem também no PCSX2; fora do plano por enquanto.
- **Agora: folga de CPU (C7, C6, C8), em partes**, para a máquina estar limpa quando o GS novo entrar (ele também
  gasta CPU na GSThread) e para o widescreen de verdade (mais geometria no VU1 e na GameThread). Ordem: medir de onde
  vem o custo do IOP → C7 → C6 → C8. Cada parte com chave para desligar, critério e tabela por thread.

**Troca do GS:** estudo feito (PROGRESS §80, `docs/GS_PCSX2.md`); fase 1 feita (§81: `pcsx2_gs` com DX11 abre e desenha
numa janela de teste em 2x/4x); fase 2 feita (§82: `pcsx2gs.dll` ligada no runtime, `Renderizador=pcsx2`; boot, menu e
luta certos, fight8 54.7 fps em 1x / 56.3 em 4x com a GSThread em ~29%); fase 3 feita (§83: leitura do flare
assíncrona; fight8 60 fps / 60 distintas de 1x a 8x, GSThread ~28%; §84: 2 núcleos 56-57 fps, 4 núcleos 59.5-59.9 em 1x e 4x); fase 4 feita (§85-86: o GS do PCSX2 é o
padrão; filtros no `config.ini`, FXAA ligado; OSD em inglês; shaders, fonte e cache de shaders no repositório).
Fase 5: texturas HD feitas (§87, pacotes do PCSX2, PNG por WIC); widescreen de verdade feito (§117, o modo 16:9 do
próprio jogo); falta Vulkan/DX12. O
código do PCSX2 usado está no repositório desde §90 (`thirdparty/pcsx2`).

### Fase D — validação em máquina menor

Rodar o critério com afinidade 0-7 (4 núcleos/8 threads) com 2 e 4 lutadores (`fight4_heavy2` e um state 1v1, se
existir). Meta: 60 lisos.

### Fase E — renderizador nativo (só se as fases A-D não chegarem nos 60)

**Condição:** o usuário acha que as fases A-D bastam, porque o PCSX2 roda a cena de 8 lutadores. Só começar a Fase E
se, depois delas, o fight8_heli ainda reprovar **e** a tabela por thread mostrar o VU1/GS/GIF como limite.

**Referência:** o port do Ace Combat 5 (§10), em `runtime/src/rn/`. Ele não roda os microprogramas do VU1 que
substituiu: lê os pacotes VIF que iam para cada microprograma, faz a mesma conta em C (`rn_vp_model.c`,
`rn_vp_lit.c`, `rn_vp_terrain.c`...) e desenha malhas de GPU de verdade. O resto (menus, efeitos) continua no GS
emulado, no mesmo quadro.

**E1. Censo dos microprogramas.** Quais overlays `.DVP` rodam numa luta de 8 e quanto tempo de VUThread/GS cada um
custa. Os candidatos já conhecidos: skinning em `0x1D98` e cena em `0x3568` (`docs/GAME.md`).
**E2. Só o skinning dos personagens primeiro** (é o que escala com 8 lutadores). Versão em C do microprograma,
comparada com o AOT vértice a vértice (como o `rn_compare.py`/`rn_vpfuzz.c` do AC5), e desenho como malha.
**E3. Cenário**, se ainda faltar.
- **Chave:** `URBAN_RN=0`.
- **Custo:** alto (semanas por microprograma). O código do AC5 não serve direto (os microprogramas são outros); o que
  se aproveita é o método e as ferramentas de comparação.
- **Bônus:** resolução interna alta e filtro de textura de verdade, de graça, para o que passar por aqui.

## 8. Fora do plano agora (e por quê)

| item | por quê |
|---|---|
| SIGNAL/FINISH no EE em tempo emulado | com 8 lutadores a GameThread espera o VU/GIF só 0,05 ms por quadro: quase nenhum ganho, e é o passo mais arriscado. Reavaliar se outra cena mostrar espera relevante. |
| Contagem de instruções do EE | o código do jogo é só ~24% da GameThread; exige regenerar o código. |
| Trocar o renderer do GS (PCSX2 GS / paraLLEl-GS) | só se a Fase B mostrar o back end acima de ~8-10 ms reais de GPU. A licença não é problema: o projeto será open source. |
| VU1 (b) registradores em XMM, (d) pipeline estático, decomp do bloco 9 | a VUThread está a 47% (~9,8 ms): tem folga. |
| Filas SPSC sem trava | locks ~2%; só se aparecerem depois dos outros passos. |
| Modo determinístico | não afeta a meta de desempenho. (Multitap feito em 2026-10-12, PROGRESS §93.) |
| Códigos de pnach além de 0/1/2 (condicionais, ponteiro, cópia) | o pnach de 8 jogadores só usa 1 e 2; o loader avisa no log quando pula algum. |

## 9. Bugs conhecidos (não são de desempenho, mas estão na fila)

- **Teste do usuário depois do §67 (2026-10-11):**
  - **Save/load no memory card resolvido:** salva sem travar e a música não para; os "pipocos" de áudio sumiram
    (provavelmente o RPC NOWAIT adiado, §66, e o controle de taxa do áudio, §67).
  - **Desempenho:** sem quedas grandes no multiplayer.
  - **Engasgos no modo história** (trocas de câmera etc.): **acontecem também no PCSX2**, na mesma máquina. Muito
    provavelmente são do próprio jogo (carga de dados na troca de cena). Prioridade baixa; se for olhar, comparar o
    tempo de quadro nas mesmas cenas nos dois.
  - **Engasgo nas trocas de tela de loading** e **leve engasgo no fim da missão**, na cena em que a tela pisca com
    efeito (a piscada é do jogo). Não foram comparados com o PCSX2; vale medir quando a Fase D/GS estiver em
    andamento (`PS2X_FRAME_TIMES` + `URBAN_NATIVE_PROFILE` nessas cenas).
- **Texturas com aspecto de *dithering*** (pontilhado). No PCSX2 do usuário não aparece, mas lá roda em 6×, com
  filtro bilinear (`filter = 2`), FXAA e `dithering_ps2 = 2` (padrão: *dithering* sem escala). Investigar:
  - se a nossa emulação do DTHE/DIMX do GS aplica o *dithering* no lugar e na escala certos (o PS2 pontilha na saída
    de 16 bits; em 32 bits não deveria aparecer);
  - se o pontilhado vem da conversão de cor (CLUT/16 bits) e não do DTHE;
  - opção de desligar (`PS2X_GS_DITHER=0`), como o "Dithering: Off" do PCSX2;
  - comparar screenshots da mesma cena nos dois em 1×.
- **Modo história:** às vezes trava com `[mtvu] vblank waited 100 ms for the VU/GIF workers; going ahead`.(nao aconteceu mais, joquei até a missao 32 e nao travou, so as travadinhas mesmo que)
- **Gráficos:** texturas faltando ou com aspecto posterizado em lutas; faltam as estrelinhas de stun na cabeça e falta as bolinhas do HUD no multiplayer de quem enceu o ROUND.
  faltam cores no seletor "1p vs 2p..." do menu.
  - **2026-10-12 (§91):** bolinhas do round **resolvidas** (PMULTH errado no recompilador; a cor saía `00000101`). O
    mesmo cálculo de cor está nas funções de sprite/partícula do HUD e dos efeitos (`0x10d6c0`, `0x10dbc8`,
    `0x10e260`...): o efeito de hit (fogo) já aparece; conferir estrelinhas de stun e as cores do seletor "1P VS 1C".
  - **Vídeos de abertura** com a imagem deslocada e faixa preta, e estalos no áudio: **resolvidos** (§92: conclusão de
    DMA guardada que o `D_STAT` não limpava; ADMA devolvendo 0 com o bloco atrasado).
  - **Áudio do vídeo: canal direito adiantado 171 ms** (fala na direita e depois na esquerda) e estalos nos vídeos
    iniciais: **resolvido** (§120: o decodificador ADX da CRI media só o espaço livre do canal esquerdo e o direito
    perdia 0x400 bytes a cada volta do anel). Falta o usuário confirmar de ouvido. Continua anotado o resto de chiado
    à direita em algumas músicas de luta e na seleção (§94).
- **Save no memory card:** ~~pequenos cortes de áudio e travadinhas durante a gravação~~ resolvido segundo o usuário
  (2026-10-11, acima).
- **Pico raro de 68-79 ms:** sem causa encontrada. (no modo hsitoria esta acontecendo frequente umas travadas entao da pra testar com eu jogando ) → em 2026-10-11 o usuário viu os mesmos engasgos no PCSX2 (acima).
- **FMVs: resolvido (PROGRESS §89)** — IPU do AC5, a libmpeg do jogo no lugar do HLE, `ResumeThread` devolvendo o id
  da thread, DMA de scratchpad em interleave/chain. Abertura, logos e o resto tocam com imagem e som. Falta conferir a
  mini tela do "mission info" no modo história. Antes:
- **FMVs:** pretos (sem emulação do IPU).
  - O vídeo é CRI Sofdec (MWSFD 3.50 + M2V 1.958), que programa o IPU direto; não passa pelo `sceMpegGetPicture`.
  - A mini tela do "mission info", que mostra o mapa da missão, também fica preta. Muito provavelmente é vídeo e
    deve aparecer quando o IPU estiver emulado (é o IPU, não o IOP).
  - **Plano:** portar o `runtime/src/ps2_ipu.c` + `ps2_ipu_dct.inc` do Ace Combat 5 (§10) para o runtime, num `.cpp`
    novo com estado `static` (sem mexer nos headers do PCH). Ele emula os 10 comandos do IPU, os registradores
    `0x10002000-0x10002030`, as FIFOs `0x10007000/0x10007010` e o DMA dos canais 3 e 4 (a ligação com o DMA está no
    `ps2_hw.c` deles). É emulação de hardware, então não depende de o jogo usar libmpeg ou Sofdec.
  - **Chave:** `PS2X_IPU=0`. Testar com a abertura (depois do logo da Namco) e com o mission info.
  - Não é desempenho, então pode ser feito antes, durante ou depois das fases.

## 10. Projetos de referência (clonados em `H:\JOGOS`)

| pasta | o que é | serve para | licença |
|---|---|---|---|
| `AC5-Recomp-ref` | Ace Combat 5 (Namco, 2004), recompilação estática para C, runtime próprio (Vulkan, SDL3, IOP todo em HLE) | IPU completo (`ps2_ipu.c`); renderizador nativo (`rn/`, Fase E); VU1 AOT com modo de verificação contra o interpretador; NUSOUND em HLE (mesmo driver de som da Namco, versão de 2004) | Apache-2.0 (compatível com a GPL-3.0; manter o aviso) |
| `ico-pc-ref` | ICO, port a partir de decompilação (C, Vulkan/D3D12, SDL3) | FMV com `libmpeg2` (`port/fmv`); SPU2/ADPCM no host (`port/audio/spu2.c`) como comparação para o streaming ADX | MIT |

- Nenhum dos dois usa CRI ADX: a música dos menus (streaming ADX) não tem referência pronta.
- O SDK do ICO é de 2001 e o do Urban é o 2.8 (2005): o `sce/` do ico-pc não serve para o Urban.
