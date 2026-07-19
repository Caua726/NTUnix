# Desktop NTUnix (dispd + ntwm) — gaps conhecidos (deferidos)

Este documento lista o que ainda **não** está implementado, com justificativa.
Ele é a fonte honesta de invariantes: nada aqui afirma que algo está pronto sem
estar (a auditoria pegou o doc anterior mentindo — #137).

## O que a auditoria profunda apontou e **já foi corrigido**

Depois da auditoria de 140 defeitos, foram corrigidos nos commits da branch
`feat/desktop-dispd-ntwm` (ver `git log`):

- **Terminal (foco #1).** Emulador VT reescrito como máquina de estados completa
  (`src/dispd/vt.c`): responde a DSR/CPR/DA (write-back no pty) — era a causa-raiz
  do ash preto (busybox lineedit travava no `ESC[6n`); tela alternada, região de
  rolagem, save/restore, IL/DL/ICH/DCH/ECH, SGR 16/256/**truecolor**, UTF-8 em
  codepoints. (#59-64, #105-114 parcial.)
- **Transação de layout atômica.** FRAME-BEGIN..COMMIT agora bufferiza e aplica no
  COMMIT (swap); input/hit-test nunca veem estado parcial; frame travado é
  descartado, não publicado pela metade. (#21, #27-31.)
- **Protocolo robusto.** Fila com geração (descarta comandos de conexão morta),
  resync no overflow, remontagem de fragmentos, validação de versão, guarda de
  HELLO duplicado, snapshot em ordem estável com floating. (#32-33, #58, #65-72,
  #77-78, #81-84.)
- **Input.** Par down/up (sem vazar keyup), fila infalível, mouse encaminhado a
  apps e terminais (modos DEC/xterm), APP-KEY com ação e codepoint completo.
  (#8-14, #24, #26.)
- **Crashes/UAF.** Join garantido no teardown de terminal, OVERLAPPED com espera de
  cancelamento, backbuffer/grade nulos abortam, overflow de geometria/CSI clampado,
  `alive`/`rx` atômicos. (#42, #60, #74, #86-88, #90, #93, #95-96, #105, #108-109.)
- **Apps.** Clipping obrigatório da surface, PID real, close que encerra o processo,
  quotas anti-DoS, deadline de handshake, checagem de colisão de section, lifecycle
  de fila infalível pra create/destroy. (#49, #52-56, #91, #128-131.)
- **Config/layout.** INI por seção, numérico validado, linha longa rejeitada,
  workspaces unificados, gaps sem altura negativa. (#4, #38-39, #46-48.)
- **initd.** `AssignProcessToJobObject` checado (aborta o spawn se falhar). (#133.)

## Ainda deferido — com justificativa

### Apps (fronteira apps↔dispd)
- **Resize negociado (#51).** O compositor já **clipa** a surface ao tile (não
  invade vizinhos), mas ainda não há `APP-CONFIGURE(serial,w,h)` + ack pro app
  redesenhar no tamanho do tile. Sem isso, uma app tilada aparece cortada ou com
  fundo sobrando. Feature real; precisa de protocolo de reconfiguração + recriação
  de section pelo app. Adiado.
- **Double-buffering / fence (#50, #57).** `APP-COMMIT` não sincroniza escritor e
  compositor → pode haver tearing. O repaint periódico (~2×/s) garante que um commit
  perdido acaba renderizado, mas falta front/back surface com swap atômico. Adiado.
- **DACL/nome da section (#126, #130).** Os pipes e sections usam DACL padrão; o
  nome da section agora tem sal (menos previsível) e há checagem de colisão, mas
  não há restrição de ACL por token/sessão. Num live single-user (tudo SYSTEM) o
  risco é baixo; endurecer a ACL às cegas arriscaria quebrar a conexão do WM.
  Adiado até haver como validar numa sessão real.

### Terminal / VT
- **Render Unicode de verdade (#106-107, #116).** A grade já guarda **codepoints**
  (UTF-8 decodificado; multibyte não corrompe mais em várias células), mas o render
  usa a fonte OEM (CP437): ASCII e um punhado de box-drawing saem certos, o resto
  vira `?`. Unicode pleno precisa de fonte TTF embutida + text-out wide. Adiado.
- **Scrollback / reflow (#113).** Resize preserva o canto superior; não há histórico
  nem re-quebra de linha. Adiado.
- **Cursor piscante (#112).** O cursor é bloco fixo. Adiado.

### Render / display
- **Resize de resolução/DPI (#102) e multi-monitor (#103).** Sem
  `WM_SIZE`/`WM_DISPLAYCHANGE`/`WM_DPICHANGED`; um `OUTPUT`. A VM de teste é
  single-monitor. Features; adiadas.
- **Recuperação total de device loss / waitable swapchain (#100, #104).** O DXGI
  agora **detecta** device removed/reset e para de apresentar (não roda num device
  morto) e checa `ResizeBuffers`, mas não recria o device nem migra pro GDI em
  runtime. Só importa em GPU real (a VM roda WARP). Adiado.
- **Damage tracking / perf (#120-125).** Recompõe a tela inteira quando suja (o
  idle-present já limita a frequência). Otimização, não correção. Adiado.

### Input / protocolo
- **Raw Input (#139) e escopo do hook (#15/#20).** Ainda usa `WH_KEYBOARD_LL`
  global; o fallback WM_KEYDOWN/UP agora é robusto (par down/up, sem duplicação),
  mas a migração pra Raw Input buffered fica pra depois. No alvo (o dispd é dono da
  sessão) o hook global não rouba de mais ninguém na prática.
- **Escrita síncrona do ntwm (#76) e heartbeat de liveness (#85).** O ntwm é
  single-thread síncrono; um `WriteFile` sem timeout pode bloquear se o dispd parar
  de ler (o dispd tem uma thread leitora dedicada, então na prática drena rápido).
  Há resync no overflow, mas não um heartbeat que detecte um WM "vivo porém parado".
  Adiado.
- **IDs de 64 bits (#94).** Contador `unsigned`; wrap é irreal num desktop tiling.
  Adiado.

### UX
- **Feedback de erro na tela (#7).** Falhas (spawn de terminal, backend) vão só pro
  log; falta um placeholder/toast visível. Adiado.
- **Barra/decorações interativas (#2, #3).** A barra e as titlebars são visuais; não
  respondem a clique (mover/fechar/trocar ws pela barra). Adiado.
