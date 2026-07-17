# Window managers, compositors e display servers — survey de código real

> Pesquisa de fundação para o **dispd** (display server nativo do NTUnix) + **ntwm** (tiling WM
> trocável), lendo o *código-fonte real* dos WMs/compositores mais instrutivos.
> Modelo alvo: split estilo-X11 (server separado do WM) mas com commit **atômico/declarativo**
> estilo-Wayland, ligados por um **named pipe message-mode**.
>
> Casa com `docs/pesquisa/nt-dwm-compositor.md`: no NT o primeiro compositor é o **caminho 3b** —
> uma **swap chain flip-model fullscreen por monitor** onde *nós* desenhamos nossas próprias
> superfícies (terminais nativos do NTUnix), sem depender do dwmcore. Portanto dispd **não**
> herda os problemas de gerenciar HWNDs alheios: ele é dono do buffer, do compositor e do foco.

---

## 0. O que foi lido (fontes com referência de linha)

Clones rasos lidos linha-a-linha:

| Projeto | Arquivo | O que ensina |
|---|---|---|
| **bspwm** | `src/bspwm.c` (556 l.), `src/tree.c`, `src/query.c`, `src/messages.c`, `src/settings.c` | **restart-survival** (re-exec preservando socket + dump JSON), BSP recursivo, socket UNIX IPC |
| **dwm** | `dwm.c` (2165 l.) | master-stack `tile()`, tags (bitmask), `grabkeys()`, `showhide()`, `focus()`, layout via ponteiro de função |
| **tinywl** (wlroots) | `tinywl/tinywl.c` (1101 l.) | compositor de referência: lifecycle de surface xdg-shell, **initial-commit/configure**, loop de frame por output, seat/foco de teclado |
| **dwl** | `dwl.c` (3217 l.) | dwm-sobre-wlroots: visibilidade via **scene-node enabled/disabled**, camadas de z-order, `client_set_suspended` |
| **i3** | `docs/ipc`, `include/i3/ipc.h` (via doc oficial) | protocolo IPC framed (magic+len+type), GET_TREE, evento de shutdown restart |
| **komorebi** | arquitetura (DeepWiki/README) | daemon + `SetWinEventHook` + Unix socket `\\.\komorebi.sock`, subscribe de eventos |
| **NT (contexto)** | `nt-dwm-compositor.md` | por que dispd = flip-chain fullscreen (3b), não leitor de redirection surfaces (3a) |

---

## 1. Núcleo do compositor: surface/buffer, render loop, damage, commit atômico

### 1.1 Surface e buffer — o modelo Wayland (a referência)

No modelo Wayland (tinywl), o cliente e o compositor conversam por **estado duplo-bufferizado
que só vira "verdade" no commit**. O ciclo em `tinywl.c`:

- `server_new_xdg_toplevel` (l.803): quando um cliente cria uma janela top-level, o compositor
  cria um **nó da scene graph** (`wlr_scene_xdg_surface_create`, l.813) e engancha listeners
  `map`/`unmap`/`commit`/`destroy`.
- `xdg_toplevel_commit` (l.694): **cada commit entrega um estado coerente** (buffer + geometria +
  damage) de uma vez. No *initial commit* o compositor **é obrigado a responder com um configure**:

  ```c
  if (toplevel->xdg_toplevel->base->initial_commit) {
      /* compositor MUST reply with a configure so the client can map */
      wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0); /* 0,0 = cliente escolhe */
  }
  ```

- `xdg_toplevel_map` (l.673): só depois do handshake a surface é "mapeada" (pronta pra tela);
  aí entra na lista e ganha foco.

**A lição-chave:** o protocolo é **declarativo e atômico**. O cliente nunca desenha "ao vivo" no
scanout; ele preenche um buffer, anexa, e **commita** — o compositor recebe uma transação
completa. Nunca existe um frame meio-desenhado. Isso é exatamente o que queremos no pipe do dispd:
o ntwm não manda "mova 3px, depois redimensione" — manda um **layout completo** que o dispd aplica
atomicamente.

### 1.2 Render/frame loop — dirigido pelo output, throttled por frame-done

`output_frame` em `tinywl.c` (l.573) é chamado **uma vez por vblank** (ex. 60 Hz):

```c
static void output_frame(...) {
    struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);
    wlr_scene_output_commit(scene_output, NULL);      /* renderiza SE necessário e apresenta */
    clock_gettime(CLOCK_MONOTONIC, &now);
    wlr_scene_output_send_frame_done(scene_output, &now); /* libera clientes p/ próximo frame */
}
```

Dois pontos:
1. **O loop é dirigido pelo display**, não pelos clientes. O output diz "estou pronto pra um
   frame" e o compositor compõe. Isso é o ritmo natural do scanout.
2. **`frame_done` é o throttle.** O cliente só desenha o próximo frame quando recebe frame-done.
   Isso pauta os clientes ao refresh e evita render desperdiçado (um terminal ocioso não gera
   frames). dwl faz igual em `rendermon` (l.2152→`wlr_scene_output_send_frame_done`, l.2173).

Contraste com X11 (dwm): **não existe render loop no WM**. As janelas se auto-desenham direto no X
server; o dwm só move/redimensiona (`XMoveResizeWindow`) e o X server compõe. dwm nunca toca em
pixel de cliente. Isso é o modelo "server desenha, WM só posiciona" — e é literalmente o dispd:
**dispd compõe/apresenta; ntwm só decide retângulos.**

### 1.3 Damage tracking

wlroots (usado por tinywl/dwl) faz damage-tracking *dentro* da scene graph: `wlr_scene_output_commit`
só re-renderiza as regiões que mudaram desde o último frame; se nada mudou, o commit é barato ou
nulo. O cliente reporta seu damage no commit (`wl_surface.damage`), o compositor acumula por nó, e a
composição final só toca os pixels sujos. **Sem damage, todo frame recompõe a tela inteira** — caro
e desnecessário para um desktop majoritariamente estático (que é o caso de terminais).

Para dispd: cada surface hospedada carrega uma **região suja**; a composição por vblank só
re-blita retângulos sujos na back-buffer da flip-chain. Um terminal parado = zero trabalho de GPU.

### 1.4 Commit atômico no output (KMS/DRM ↔ D3DKMT)

No Linux o `wlr_output_commit_state` empurra um **atomic modeset/pageflip** ao KMS (todo o estado —
buffer + modo + plano — numa transação; ou aplica tudo ou nada). No NT o análogo é
`D3DKMTPresent` sobre a flip-chain (ver `nt-dwm-compositor.md` §1.4/§3d): um present por vblank,
troca back↔front buffer. **A propriedade "atômico ou nada" vale nos dois mundos** — e é o que
elimina tearing (§6).

---

## 2. Split WM↔server e IPC: como swappability + restart-survival funcionam

### 2.1 Por que o WM é trocável no X11 (o modelo que queremos)

No X11 o **X server** é dono do display, dos inputs e das janelas; o **window manager é só mais um
cliente X**. Ele se torna "o WM" ao pedir **SubstructureRedirect na janela root**. dwm faz isso
implicitamente via `ROOT_EVENT_MASK`; bspwm explicitamente em `register_events` (`bspwm.c` l.465):

```c
uint32_t values[] = {ROOT_EVENT_MASK};
xcb_generic_error_t *e = xcb_request_check(dpy,
    xcb_change_window_attributes_checked(dpy, root, XCB_CW_EVENT_MASK, values));
if (e != NULL) { ...; err("Another window manager is already running.\n"); }
```

**Só um cliente pode ter SubstructureRedirect na root** → só um WM por vez. Se falhar, "outro WM já
está rodando". Consequências que importam pra nós:

- **Trocar de WM = matar um cliente e subir outro.** As janelas **não pertencem ao WM** — pertencem
  ao X server. Mate o dwm, suba o bspwm: nenhuma janela morre, elas só são re-geridas. Essa é a
  razão da swappability. **dispd deve ser o dono das surfaces; ntwm nunca.**
- O contrato WM↔cliente é o **ICCCM** (foco, WM_STATE, tamanho-hint) e o WM↔painel/pager é o
  **EWMH** (`_NET_*`). bspwm anuncia dezenas de átomos EWMH em `setup` (`bspwm.c` l.379-406). Esses
  contratos são *padronizados e versionados* — é o que deixa barras/pagers de terceiros funcionarem
  com qualquer WM. **dispd precisa de um protocolo estável equivalente** (nosso "ICCCM/EWMH" no
  pipe) pra que ntwm seja substituível por outro layout-engine sem quebrar barra/terminais.

### 2.2 Restart-survival do bspwm — o padrão-ouro (documentado com precisão)

Este é o mecanismo exato que o ntwm deve copiar. bspwm reinicia **sem perder estado nem derrubar
clientes conectados**, re-executando a si mesmo com o mesmo PID. Sequência real em `bspwm.c`:

**(A) Disparo.** `bspc wm --restart` chega pelo socket; o handler em `messages.c` (l.1317):
```c
} else if (streq("-r", *args) || streq("--restart", *args)) {
    running = false;   /* sai do loop select() */
    restart = true;    /* mas marca reinício */
}
```
`running` é `volatile sig_atomic_t` (também vira false em SIGINT/TERM/HUP via `sig_handler`, l.535).

**(B) Dump do estado como JSON.** Ao sair do loop (`bspwm.c` l.275-285):
```c
if (restart) {
    char state_path[MAXLEN];
    /* caminho derivado de host/display/screen (STATE_PATH_TPL) */
    snprintf(state_path, sizeof(state_path), STATE_PATH_TPL, host, dn, sn);
    FILE *f = fopen(state_path, "w");
    query_state(f);    /* serializa a ÁRVORE INTEIRA em JSON */
    fclose(f);
}
```
`query_state` (`query.c` l.38) despeja tudo: `focusedMonitorId`, `primaryMonitorId`,
`clientsCount`, `monitors[]` (cada um com `windowGap`, `borderWidth`, `focusedDesktopId`,
`rectangle`, `padding`, `desktops[]` → `nodes` da árvore BSP com `splitType`/`splitRatio`/`client`),
`focusHistory`, `stackingList`, `eventSubscribers`. É um **snapshot declarativo completo do WM**.

**(C) Preservar o socket através do exec — o truque crítico.** Normalmente o socket de escuta é
`FD_CLOEXEC` (fecha no exec) — setado em `bspwm.c` l.196. Antes de re-exec, o bspwm **limpa** esse
flag pra o fd sobreviver (`bspwm.c` l.297):
```c
if (restart) {
    fcntl(sock_fd, F_SETFD, ~FD_CLOEXEC & fcntl(sock_fd, F_GETFD));  /* fd sobrevive ao exec */
```
**Por quê isso é ouro:** o socket de escuta continua aberto e no *mesmo número de fd*. Clientes
`bspc` conectados/reconectando **não percebem interrupção** — não há janela de "connection refused"
porque ninguém fechou/re-bindou o socket.

**(D) Re-exec de si mesmo, injetando estado + fd.** (`bspwm.c` l.299-322):
```c
/* acha onde está "-s" no argv atual pra não duplicar */
for (rargc = 0; rargc < argc; rargc++)
    if (streq("-s", argv[rargc])) break;
/* reconstrói argv: [argv originais até -s] + -s <state_path> -o <sock_fd> */
rargv[rargc]     = "-s"; rargv[rargc+1] = state_path;   /* onde ler o estado */
rargv[rargc+2]   = "-o"; rargv[rargc+3] = sock_fd_arg;  /* fd do socket já aberto */
rargv[rargc+4]   = 0;
execvp(*rargv, rargv);   /* mesmo PID, novo binário (pega bugfix/config novo) */
```

**(E) Lado da subida — reconexão do estado.** No `main` (l.109-157):
```c
case 's': run_level |= 1; snprintf(state_path, ...); break;  /* tem estado */
case 'o': run_level |= 2; sock_fd = strtol(optarg, ...); break; /* herda socket */
...
if (state_path[0] != '\0') { restore_state(state_path); unlink(state_path); }
if (sock_fd == -1) { /* só cria+bind+listen SE não herdou */ ... }
fcntl(sock_fd, F_SETFD, FD_CLOEXEC | fcntl(...));  /* re-arma CLOEXEC no herdado */
run_config(run_level);  /* run_level!=0 → NÃO re-roda o bspwmrc do usuário */
```
`restore_state` (`restore.c` l.44) re-parseia o JSON (parser `jsmn` vendorizado) e reconstrói
monitores/desktops/árvore/clientes/histórico/stack. E `run_config` recebe o `run_level`
(`settings.c` l.76) pra **não re-executar o script de startup** num restart (senão abriria tudo de
novo).

**Resumo do padrão-ouro (5 invariantes):**
1. Estado do WM é **serializável por completo** (declarativo, JSON) — nada de estado escondido só na
   memória.
2. O canal de IPC (socket) **sobrevive ao exec** limpando FD_CLOEXEC → clientes não veem gap.
3. **Re-exec do próprio binário** (mesmo PID) pega código/config novos sem derrubar sessão.
4. Estado passa por **arquivo + argv** (`-s path`), socket passa por **fd herdado** (`-o fd`).
5. Um **run-level** distingue cold-start de restart pra não re-rodar side-effects (startup script).

### 2.3 IPC framed do i3 (o formato que o pipe do dispd deve imitar)

i3 usa um **socket UNIX com protocolo binário framed** (não texto solto). Header de **14 bytes**:
`"i3-ipc"` (6) + `length` u32 LE (4) + `type` u32 LE (4), seguido de payload JSON. Requisições têm
tipo (ex. `GET_TREE`=4, que serializa a árvore inteira de containers como JSON — igual ao
`query_state` do bspwm). **Eventos assíncronos** têm o bit alto do `type` setado, então o cliente
distingue "resposta ao meu pedido" de "notificação espontânea" no mesmo canal. Há um evento
`shutdown` com `{"change":"restart"|"exit"}` avisando antes de reiniciar.

komorebi (Windows) confirma o mesmo padrão num daemon: `komorebi.exe` mantém o `WindowManager`
central; comandos chegam por **Unix Domain Socket** (`\\.\komorebi.sock` — o Windows moderno tem
AF_UNIX) vindos do `komorebic` CLI; eventos do OS chegam por `SetWinEventHook`; e clientes podem
**subscrever** todo `WindowManagerEvent`/`SocketMessage`. Ou seja: **daemon dono do estado + CLI/lib
que fala comandos tipados + stream de eventos** é o consenso de três ecossistemas (i3/bspwm no
Linux, komorebi no Windows).

### 2.4 Mapeando pro pipe message-mode do dispd/ntwm

Named pipe **message-mode** (`PIPE_TYPE_MESSAGE|PIPE_READMODE_MESSAGE`) já dá enquadramento de
mensagem no nível do kernel NT — cada `WriteFile` é um datagrama lido inteiro por um `ReadFile`,
então **não precisamos do campo `length` manual** que o i3 usa sobre stream sockets. Ainda assim,
copie a estrutura:

- **Cabeçalho fixo por mensagem:** `{magic u32, type u16, flags u16, seq u32}` + payload. `flags`
  carrega o bit "é-evento" (como o bit alto do i3) e "é-atômico/fim-de-transação".
- **dispd→ntwm (eventos, estilo `SetWinEventHook`/wlroots signals):** `surface_created`,
  `surface_destroyed`, `surface_title`, `output_added/removed/resized`, `input_key`
  (só as teclas que o ntwm registrou — §4), `focus_request`.
- **ntwm→dispd (comandos declarativos, estilo configure do Wayland):** um **`apply_layout`** que
  descreve *o frame inteiro* — lista de `{surface_id, rect, z, visible, border, focused}`. dispd
  aplica **atomicamente** no próximo vblank. Nunca comandos incrementais "mova/redimensione".
- **Restart-survival portado:** o **pipe é dono do dispd** (server), não do ntwm. Ao reiniciar o
  ntwm, dispd **segura o estado das surfaces** (elas são dele). ntwm ao subir manda um
  `query_surfaces`; dispd responde a lista; ntwm re-deriva o layout. Não precisamos "passar fd de
  socket" como o bspwm porque **quem sobrevive é o server (dispd), e o ntwm é o descartável** — o
  inverso do X (onde o X server é o processo pesado e estável, e o WM é o leve/trocável). Perfeito:
  o modelo do dispd é literalmente "X server (dispd) + WM cliente trocável (ntwm)".
- Opcional (paranoia estilo bspwm): ntwm também pode dumpar seu próprio JSON de layout
  (nmaster/mfact/tags por workspace) num arquivo antes de sair, e recarregar — barato e robusto.

---

## 3. Algoritmos de tiling/layout

### 3.1 Master-stack (dwm `tile()`)

`dwm.c` l.1688 — o layout tiling clássico, ~25 linhas:

```c
for (n=0, c=nexttiled(m->clients); c; c=nexttiled(c->next), n++);  /* conta tiled */
if (n == 0) return;
if (n > m->nmaster) mw = m->nmaster ? m->ww * m->mfact : 0;  /* largura do master */
else                mw = m->ww;                               /* só masters → tela toda */
for (i=my=ty=0, c=nexttiled(m->clients); c; c=nexttiled(c->next), i++)
    if (i < m->nmaster) {                       /* coluna master */
        h = (m->wh - my) / (MIN(n, m->nmaster) - i);
        resize(c, m->wx, m->wy + my, mw - 2*c->bw, h - 2*c->bw, 0);
        if (my + HEIGHT(c) < m->wh) my += HEIGHT(c);
    } else {                                    /* coluna stack */
        h = (m->wh - ty) / (n - i);
        resize(c, m->wx + mw, m->wy + ty, m->ww - mw - 2*c->bw, h - 2*c->bw, 0);
        if (ty + HEIGHT(c) < m->wh) ty += HEIGHT(c);
    }
```

Parâmetros: **`nmaster`** (quantas janelas na coluna esquerda, ajustável em runtime via `incnmaster`)
e **`mfact`** (fração 0..1 da largura pro master, via `setmfact`). Divisão de altura é **igual entre
os restantes** (`(wh - usado) / (faltam)`) — auto-balanceada. Borda subtraída (`2*c->bw`). `monocle`
(`dwm.c` l.1114) é o caso trivial: toda janela recebe o retângulo inteiro. Layout é um **ponteiro de
função** (`m->lt[sellt]->arrange`, chamado por `arrangemon` l.396), então trocar de layout = trocar
o ponteiro. **Simplicidade extrema: ~25 linhas cobrem o tiling que 90% dos usuários querem.**

### 3.2 BSP (bspwm `apply_layout()`)

`tree.c` l.73 — cada desktop é uma **árvore binária**; folhas = janelas, nós internos = divisórias.
`arrange` (l.43) calcula o retângulo raiz (descontando padding do monitor/desktop e `window_gap`),
depois `apply_layout` desce recursivamente:

```c
if (is_leaf(n)) {
    /* folha: aplica geometria ao cliente, descontando gap + 2*border */
    int bleed = wg + 2*bw;
    r.width  -= bleed;  r.height -= bleed;
    window_move_resize(n->id, r.x, r.y, r.width, r.height);
} else {
    unsigned int fence;
    if (n->split_type == TYPE_VERTICAL) {
        fence = rect.width * n->split_ratio;          /* posição da divisória */
        /* clamp por min-width dos dois filhos (constraints) */
        first_rect  = {rect.x,         rect.y, fence,             rect.height};
        second_rect = {rect.x + fence, rect.y, rect.width-fence,  rect.height};
    } else { /* TYPE_HORIZONTAL: divide altura */ ... }
    apply_layout(m, d, n->first_child,  first_rect,  root_rect);
    apply_layout(m, d, n->second_child, second_rect, root_rect);
}
```

Parâmetros: **`split_type`** (vertical/horizontal por nó), **`split_ratio`** (fração da divisória,
default 0.5, ajustável por nó → resize granular), **`window_gap`** (por desktop), padding (por
monitor e por desktop), constraints `min_width/min_height` que **fixam** a divisória se o conteúdo
não couber. `presel` permite pré-selecionar onde a próxima janela entra. MONOCLE colapsa: todos os
filhos recebem `rect` inteiro (l.146). **BSP é mais flexível que master-stack** (qualquer subdivisão)
ao custo de mais estado por nó — e é exatamente o que serializa bem no JSON de restart (§2.2).

### 3.3 Multi-monitor

Ambos modelam **monitor → desktops/tags → janelas** como listas encadeadas. dwm: `Monitor` tem
`clients`, `stack`, `tagset[2]`, `mfact`, `nmaster` **próprios** (cada monitor é independente).
bspwm: `monitor_t` tem lista de `desktop_t`, cada um com sua árvore. Descoberta de monitores via
**RandR** (preferido) com fallback **Xinerama** (`bspwm.c` l.415-452). No NT o análogo é enumerar
outputs do WDDM/DXGI (`IDXGIOutput` por adaptador) — **uma flip-chain por monitor**, como o próprio
DWM faz (ver `nt-dwm-compositor.md` §1.4). ntwm deve tratar cada output como um contexto de layout
independente (nmaster/mfact/workspace-atual próprios).

---

## 4. Input e foco: seats, keybind grabbing, roteamento

### 4.1 Keybind grabbing — WM pega SÓ suas teclas, resto vai pro foco

**X11 (dwm `grabkeys`, l.953):** o WM registra cada atalho como `XGrabKey` na root, pra *cada*
combinação de Lock/NumLock:
```c
XUngrabKey(dpy, AnyKey, AnyModifier, root);          /* limpa */
for (k = start; k <= end; k++)                       /* cada keycode */
  for (i = 0; i < LENGTH(keys); i++)
    if (keys[i].keysym == syms[...])
      for (j = 0; j < LENGTH(modifiers); j++)        /* 0, Lock, NumLock, ambos */
        XGrabKey(dpy, k, keys[i].mod | modifiers[j], root, True, GrabModeAsync, GrabModeAsync);
```
Efeito: **só as combinações registradas** são interceptadas pelo WM (viram `KeyPress` pro WM);
**todo o resto vai direto pra janela focada** pelo X server. O WM nem vê as teclas normais — zero
overhead, zero risco de keylogger acidental.

**Wayland (tinywl):** não há "grab" no server; o *compositor É o server*, então ele vê **toda**
tecla e decide. `keyboard_handle_key` (l.201):
```c
if ((modifiers & WLR_MODIFIER_ALT) && event->state == WL_KEYBOARD_KEY_STATE_PRESSED)
    for (int i = 0; i < nsyms; i++)
        handled = handle_keybinding(server, syms[i]);   /* atalho do compositor? */
if (!handled) {
    wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
    wlr_seat_keyboard_notify_key(seat, ...);            /* senão, manda pro cliente focado */
}
```

**Para o dispd/ntwm** (que é um híbrido: dispd é o server que *vê tudo*, mas o ntwm é quem *decide o
layout*): a melhor jogada é **estilo-X grab, mas pelo pipe**. No boot o ntwm envia ao dispd a lista
de `{mod, keysym}` que ele quer. O dispd filtra o input: se a tecla casa um binding registrado,
manda evento `input_key` **pro ntwm**; senão, roteia pro **surface focado** (o terminal). Isso mantém
o dispd burro/rápido (só uma tabela de match) e o ntwm sem ver o que o usuário digita no terminal —
privacidade + performance, igual ao X.

### 4.2 Seat e foco

Wayland formaliza o **seat** = um conjunto (teclado + mouse + touch) com *um* foco de teclado e *um*
de ponteiro. tinywl `focus_toplevel` (l.113):
```c
/* desativa o anterior (cliente repinta sem realce) */
wlr_xdg_toplevel_set_activated(prev_toplevel, false);
wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);  /* z-order */
wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
wlr_seat_keyboard_notify_enter(seat, surface, keyboard->keycodes, ...); /* teclado entra */
```
Foco tem **duas faces**: (1) *ativação* visual (o cliente muda a borda/título) e (2) *entrega de
input* (o seat direciona teclado ao surface). dwm separa igual: `focus()` (l.789) faz `setfocus(c)`
(`XSetInputFocus`) + borda `SchemeSel` + `attachstack` (vira topo da pilha de foco). Note dwm l.792:
se o alvo é inválido, **cai pro primeiro visível da pilha** (`for (c=selmon->stack; c && !ISVISIBLE(c); c=c->snext)`) — nunca deixa o foco "no nada".

**Para dispd/ntwm:** foco é decisão do **ntwm** (ele conhece o layout), mas execução do **dispd**
(ele entrega input). ntwm manda `focus surface_id`; dispd (a) marca `focused` no compositor pra
desenhar realce e (b) passa a rotear teclado àquela surface. Regra do dwm (nunca focar invisível /
sempre ter fallback) é obrigatória.

---

## 5. Workspaces: tags vs workspaces, show/hide vs virtual desktop

### 5.1 Tags (dwm) vs Workspaces (i3/bspwm)

**Tags (dwm) = bitmask, N-para-N.** Cada cliente tem `unsigned int tags` (`dwm.c` l.93) e cada
monitor tem `tagset[seltags]`. Visibilidade é **AND de bits** (`dwm.c` l.52):
```c
#define ISVISIBLE(C)  ((C->tags & C->mon->tagset[C->mon->seltags]))
```
Uma janela pode estar em **várias tags ao mesmo tempo**; uma "view" pode mostrar **várias tags de
uma vez** (é uma máscara, não um índice). `tag()` (l.1670) seta a máscara do cliente; `view()` troca
`tagset`. Muito mais flexível que workspace clássico.

**Workspaces (bspwm/i3) = container nomeado, 1-para-N.** Uma janela pertence a *um* desktop; o
desktop é um nó com sua árvore. Mais simples de raciocinar, menos flexível. bspwm serializa
`focusedDesktopId` + `desktops[]` no restart.

### 5.2 Show/hide — como esconder um workspace

Três técnicas reais, em ordem de qualidade crescente:

1. **dwm — mover pra fora da tela** (`showhide`, l.1630):
   ```c
   if (ISVISIBLE(c)) XMoveWindow(dpy, c->win, c->x, c->y);        /* mostra */
   else              XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);/* esconde: joga p/ -2*width */
   ```
   A janela continua **mapeada e viva** (não há unmap→map, que causaria flicker e reset de estado).
   Só sai do campo visível. Barato, mas ocupa "espaço" lógico e o cliente continua renderizando.

2. **Windows (komorebi/GlazeWM) — `ShowWindow(SW_HIDE)` / cloaking / virtual desktop.** Escondem
   HWNDs alheios minimizando/ocultando ou movendo pra outro virtual desktop do Windows. Frágil:
   depende do cooperativo do app, luta com o DWM, e alguns apps repintam/reposicionam sozinhos.

3. **Wayland (dwl) — desabilitar o nó da scene graph** (`arrange`, l.517):
   ```c
   wlr_scene_node_set_enabled(&c->scene->node, VISIBLEON(c, m));  /* compositor só não renderiza */
   client_set_suspended(c, !VISIBLEON(c, m));                     /* e AVISA o cliente p/ parar */
   ```
   O compositor simplesmente **não compõe** nós desabilitados — sem mover buffer, sem flicker. E
   `client_set_suspended` diz ao cliente "você está oculto", que pode **parar de renderizar**
   (economia de CPU/GPU). **Esta é a técnica correta pro dispd**: hospedamos nossas próprias
   surfaces, então esconder um workspace = marcar `visible=false` no `apply_layout`; o compositor
   pula esses nós e o terminal oculto para de desenhar. Zero das dores do show/hide do Windows.

---

## 6. Armadilhas / anti-padrões (e correções)

### 6.1 Corridas do X11 assíncrono
O protocolo X é assíncrono: você pede geometria de uma janela que **já foi destruída** entre o
evento e o seu request → erro. bspwm blinda cada request com `check_connection` (`bspwm.c` l.498) e
trata `pending_rule` num fd separado no `select` (l.221-242) pra não bloquear o loop. **dwm** roda um
`xerror` handler que **ignora** erros benignos (BadWindow em janela morta). *Fix pro dispd:* como
dispd é o **próprio server** (síncrono com suas surfaces, sem round-trip de rede), essa classe de
corrida **desaparece** — o grande motivo pra preferir o modelo Wayland/compositor ao modelo
X-client. Ainda assim, o ntwm fala com o dispd por pipe assíncrono: use **IDs de surface com
geração/época** (não reusar id imediatamente) pra o ntwm nunca aplicar layout a um id que o dispd já
reciclou. Ecoe o `seq` do comando na resposta.

### 6.2 Tearing
Present fora do vblank rasga a imagem. **Fix:** commit atômico sincronizado ao vblank — no Linux o
atomic KMS pageflip, no NT o `D3DKMTPresent` sobre flip-chain (um flip por vblank; ver
`nt-dwm-compositor.md` §1.4). O loop `output_frame` do tinywl (§1.2) *já* pauta tudo ao vblank.
**dispd deve compor e apresentar exatamente uma vez por vblank**, e só clientes que receberam
frame-done desenham o próximo — sem present parcial, sem tearing.

### 6.3 Focus stealing
Uma janela nova roubar foco no meio da sua digitação. WMs cooperam com hints (ICCCM
`WM_TAKE_FOCUS`, EWMH `_NET_WM_STATE_DEMANDS_ATTENTION` que o bspwm suporta, l.396). No Windows é
endêmico (`SetForegroundWindow` + `AllowSetForegroundWindow`), e komorebi/GlazeWM gastam código
combatendo isso. **Fix pro dispd:** foco é **política do ntwm**, não do cliente. Uma surface nova
gera evento `surface_created`; o ntwm **decide** se foca (ex.: só marca "urgente" e mantém foco
atual). Como o dispd é o único que entrega input, **um cliente nunca pode se auto-focar** —
eliminamos a classe inteira de focus-stealing por construção. (Grande vantagem sobre a abordagem
Windows-HWND.)

### 6.4 Unmap/map flicker & perda de estado
Esconder workspace via unmap→map causa flicker e pode resetar estado do cliente. **Fix:** técnica do
dwl (§5.2 #3) — desabilitar nó da scene, nunca destruir/re-mapear.

### 6.5 Reentrância no restart / re-rodar startup
Reiniciar o WM e ele reabrir todos os apps do autostart. **Fix:** o **run-level** do bspwm
(`settings.c` l.76) — no restart `run_config` **não** re-executa o script de usuário. dispd/ntwm
precisam da mesma distinção cold-start vs restart.

### 6.6 CLOEXEC no lugar errado
Se o socket/pipe de IPC for CLOEXEC e você re-exec, **os clientes perdem a conexão** (gap de
"connection refused"). bspwm **limpa** CLOEXEC só na hora do re-exec (l.297) e **re-arma** na subida
(l.196). No dispd, como quem reinicia é o *ntwm* (não o dono do pipe), o problema é menor — mas se um
dia o dispd fizer self-restart, replicar exatamente isso.

### 6.7 fork/exec vazando fds e sinais (spawn)
`spawn` do dwm (l.1648) no filho: `close(ConnectionNumber(dpy))` (não vaza o socket X pro app),
`setsid()` (desacopla), **restaura `SIGCHLD` pra `SIG_DFL`** (senão o app herda o handler do WM e
quebra `wait()` dele). dispd/ntwm ao lançar terminais: fechar o handle do pipe no filho, novo grupo
de processo, resetar disposições de sinal herdadas.

---

## 7. Aplicar ao dispd/ntwm — recomendações concretas

1. **dispd é o "X server", ntwm é o "WM cliente" — e é o ntwm que é descartável.** dispd é dono de:
   scanout (flip-chain por output, caminho 3b), input, e o buffer de cada surface. ntwm só decide
   retângulos. Isso dá swappability grátis (mate o ntwm, suba outro; as surfaces do dispd
   sobrevivem) e mata focus-stealing e corridas-X por construção.

2. **Protocolo de pipe declarativo e atômico, não incremental.** ntwm→dispd = um único
   `apply_layout` com a lista completa `{surface_id, rect, z, visible, border, focused}` por output,
   aplicada num vblank. Nunca "mova 3px". Copie o *configure atômico* do xdg-shell (tinywl l.694) e
   o commit-ou-nada do Wayland.

3. **Header framed por mensagem** mesmo em pipe message-mode: `{magic, type, flags, seq}` + payload;
   `flags` marca evento-vs-resposta (bit alto do i3) e fim-de-transação. Ecoe `seq` nas respostas
   pra casar pedido↔resposta.

4. **Restart-survival do ntwm sem passar fd:** como o **dono do canal é o dispd** (não o ntwm), o
   ntwm reinicia trivialmente — sobe, manda `query_surfaces`, dispd devolve o estado das surfaces
   (que são dele), ntwm re-deriva o layout. Complemente com um dump JSON opcional do estado *do
   ntwm* (nmaster/mfact/workspace por output), estilo `query_state` do bspwm (`query.c` l.38).

5. **Se algum dia o dispd fizer self-restart, copie o bspwm ao pé da letra:** serialize tudo em JSON,
   **limpe FD_CLOEXEC do pipe antes do `execvp`** (`bspwm.c` l.297), passe `-s state -o fd`, re-arme
   CLOEXEC na subida (l.196), e use **run-level** pra não re-rodar o autostart (`settings.c` l.76).

6. **Layout via ponteiro de função + params por output.** Comece com **master-stack** (dwm `tile`,
   ~25 linhas: `nmaster`+`mfact`+altura igual entre restantes) e **monocle**. Guarde a **árvore BSP**
   (bspwm `apply_layout`: `split_type`/`split_ratio`/constraints/`window_gap`) como layout avançado —
   ela serializa lindamente pro restart. Cada output tem seu próprio `nmaster/mfact/workspace`.

7. **Keybind grabbing estilo-X, executado no dispd.** No boot o ntwm registra `{mod, keysym}` no
   dispd. O dispd filtra: casou binding → evento `input_key` pro ntwm; senão → roteia pro surface
   focado (o terminal). Mantém o dispd rápido e o ntwm sem ver o que se digita no terminal (privacidade,
   como o `XGrabKey` seletivo do dwm em l.972).

8. **Foco = política do ntwm, entrega do dispd, com o fallback do dwm.** ntwm manda `focus id`;
   dispd realça + roteia teclado. **Nunca focar surface invisível; sempre ter fallback pro topo da
   pilha visível** (dwm l.792). Separe *ativação visual* de *entrega de input* (tinywl `focus_toplevel`
   l.113). Surface nova **não** rouba foco: gera evento, ntwm decide.

9. **Esconder workspace = `visible=false` no layout, não mover/unmapar.** Copie o dwl
   (`wlr_scene_node_set_enabled`, l.517): o compositor pula nós invisíveis (sem flicker) **e avisa a
   surface** (`client_set_suspended`, l.518) pra o terminal oculto parar de renderizar. Modele
   visibilidade como **tags bitmask** (dwm l.52) — mais poderoso que workspace-índice, e um único
   AND decide o que compor.

10. **Render/present travado no vblank + damage por surface.** Loop dirigido pelo output (tinywl
    `output_frame` l.573): componha **uma vez por vblank**, só os retângulos sujos, e emita
    `frame_done` — clientes só desenham o próximo frame quando liberados. Elimina tearing (§6.2) e
    faz terminal ocioso custar ~zero GPU. Present via `D3DKMTPresent` na flip-chain (um flip/vblank,
    `nt-dwm-compositor.md` §1.4).

---

## Apêndice — mapa rápido projeto→lição→arquivo

- **bspwm** `bspwm.c:275-326` — restart: dump JSON + limpar CLOEXEC + `execvp` `-s/-o`. **Padrão-ouro.**
- **bspwm** `query.c:38`, `restore.c:44` — serialização/reidratação declarativa do estado do WM.
- **bspwm** `tree.c:73` (`apply_layout`) — BSP recursivo, `split_ratio`/`split_type`/gap/constraints.
- **bspwm** `bspwm.c:212-266` — loop `select()` sobre `sock_fd`+`dpy_fd` (IPC e display no mesmo loop).
- **dwm** `dwm.c:1688` (`tile`) — master-stack em ~25 linhas (`nmaster`,`mfact`).
- **dwm** `dwm.c:1630` (`showhide`) — esconder movendo pra `-2*width` (mapeado, sem flicker).
- **dwm** `dwm.c:953` (`grabkeys`) — WM pega só suas teclas; resto vai pro foco.
- **dwm** `dwm.c:52` (`ISVISIBLE`) — tags como bitmask N↔N.
- **dwm** `dwm.c:789` (`focus`) — fallback pro topo da pilha; ativação vs input.
- **tinywl** `tinywl.c:573` (`output_frame`) — loop por vblank + `frame_done` throttle.
- **tinywl** `tinywl.c:694` (`xdg_toplevel_commit`) — initial-commit→configure, transação atômica.
- **tinywl** `tinywl.c:113` (`focus_toplevel`) — seat: ativar + `keyboard_notify_enter`.
- **dwl** `dwl.c:508` (`arrange`) — visibilidade via `scene_node_set_enabled` + `set_suspended`.
- **i3** `docs/ipc` — header 14 bytes `i3-ipc`+len+type; eventos com bit alto; GET_TREE.
- **komorebi** — daemon + Unix socket `\\.\komorebi.sock` + `SetWinEventHook` + subscribe de eventos.
