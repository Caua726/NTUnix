# dwm — análise profunda (quase linha-a-linha) e mapa de porte para o `ntwm`

> Fonte analisada: **dwm 6.8** (`git.suckless.org/dwm`, commit `44dbc68` "buttonpress: fix
> status text click area mismatch"), `dwm.c` = 2166 linhas + `config.def.h` 117 linhas +
> `drw.c`/`drw.h` (a camada de desenho) + `util.c`/`transient.c`. Todas as citações de linha
> abaixo são desse `dwm.c`.
>
> Objetivo desta pesquisa: dwm é **o análogo existente mais próximo do `ntwm`** do NTUnix.
> dwm é, ao mesmo tempo, um curso completo de Xlib — o window manager é o "cliente X" que mais
> exercita as partes cruas do protocolo. Lendo dwm entende-se tanto *o que um WM faz* quanto
> *como o X11 funciona por baixo*. No fim há o mapa de porte função-por-função para
> `ntwm`/`dispd` (protocolo `ntuwm.h`).

---

## 0. Veredito de arquitetura em uma página (leia isto primeiro)

dwm e ntwm resolvem **o mesmo problema de política** (que janela fica onde, quem tem foco,
teclas de atalho, tags/workspaces) mas em **substratos opostos**:

| | **dwm (X11)** | **NTUnix (dispd + ntwm)** |
|---|---|---|
| Quem é dono das janelas | O **X server** (processo separado). Cada app cria a sua janela real; o WM só as *manipula* | O **dispd**. Janelas de app são superfícies lógicas (DIB) internas ao dispd; o ntwm **nunca** toca a janela de ninguém |
| Como o WM "assume" | `XSelectInput(root, SubstructureRedirectMask)` — só 1 cliente por tela pode (dwm.c:464, 1607) | dispd cria a **única** janela real fullscreen; ntwm conecta no pipe `NTU_PIPE_DISPD` e manda `HELLO` (wmproto.c:168) |
| WM ↔ mecanismo | Mesmo socket X que **todos** os clientes usam; o WM é um cliente privilegiado | Pipe dedicado duplex; ntwm é o **único** cliente de política |
| Aplicar layout | `XMoveResizeWindow`/`XConfigureWindow` por janela, assíncrono, sujeito a corridas ICCCM | `FRAME-BEGIN` … `PLACE`/`FOCUS`/`BORDER` … `FRAME-COMMIT` — **atômico por tick** (ntuwm.h:11-15) |
| Se o WM morre | Janelas sobrevivem (são do X server); relançar o WM re-adota via `XQueryTree`+`manage` (scan, dwm.c:1393) | Janelas sobrevivem (são do dispd); relançar o ntwm recebe **snapshot** `WINDOW…`+`SYNC` e re-declara (wmproto.c:143) |
| Entrada | `XGrabKey` no root captura combos; resto vai pro cliente focado (X server roteia) | dispd captura teclado bruto (`WM_KEYDOWN`); combo com `GRAB` vira evento `KEY` pro ntwm, senão vai pro pty (input.c:85) |

**Conclusão central:** a divisão dispd/ntwm é **estilo Wayland** (compositor dono de tudo +
cliente de política), não estilo X11 (server neutro + WM cliente). Isso significa:

1. **Toda a lógica de política de dwm porta quase 1:1 para o ntwm** — tags, monitores,
   listas clients/stack, tile/monocle, foco, keybinds. Essa é a parte "cérebro".
2. **Toda a parte de Xlib de dwm NÃO porta** — ela é substituída pelo protocolo `ntuwm.h`.
   Onde dwm faz `XMoveResizeWindow(win, x,y,w,h)`, o ntwm faz `PLACE wid x y w h ws z`.
   Onde dwm faz `XSetInputFocus`, o ntwm faz `FOCUS wid`. Etc.
3. **O ntwm herda a maior simplificação de graça:** o `FRAME-BEGIN/COMMIT` mata metade da
   complexidade de dwm, que existe só para domar as corridas assíncronas do X (o
   `XSync`/`XCheckMaskEvent(EnterWindowMask)` espalhado, o `c->x + 2*sw` de manage(), o
   `XGrabServer` de killclient/unmanage). Nada disso é necessário no modelo atômico.

O resto do documento destrincha dwm para extrair *exatamente* o que transferir.

---

## 1. A filosofia e o formato do código

O comentário de cabeçalho (dwm.c:1-22) é a tese do projeto e vale ler literalmente:

- *"dynamic window manager is designed like any other X client as well. It is driven through
  handling X events."* — dwm **não** é um servidor; é um cliente X comum que só faz uma coisa
  especial: pede `SubstructureRedirectMask`.
- *"a window manager selects for SubstructureRedirectMask on the root window … Only one X
  connection at a time is allowed to select for this event mask."* — é *isto* que faz de um
  processo "o WM". Detalhado em §3.
- *"The event handlers of dwm are organized in an array which is accessed whenever a new event
  has been fetched. This allows event dispatching in O(1) time."* — o `handler[]` (dwm.c:245-260).
- *"Each child of the root window is called a client, except windows which have set the
  override_redirect flag."* — override_redirect = janelas que o WM **ignora** (menus, tooltips,
  dmenu). O WM não gerencia essas.
- *"To understand everything else, start reading main()."*

**Modelo de configuração:** não há arquivo de config em runtime. `config.h` (copiado de
`config.def.h`) é **incluído como código C** (dwm.c:271) e compilado junto. Trocar keybind =
recompilar. Isso é deliberado (§12). As `keys[]`, `buttons[]`, `rules[]`, `layouts[]`,
`colors[]`, `tags[]` são todas arrays `static const` em config.h.

O binário são ~2200 linhas + `drw` (uma fina camada sobre Xft/Xlib para desenhar a barra).
`util.c` só tem `die()` e `ecalloc()`.

---

## 2. O ciclo de vida: `main()` → `checkotherwm()` → `setup()` → `scan()` → `run()` → `cleanup()`

### 2.1 `main()` (dwm.c:2143-2165)

```c
main:
  argc==2 && "-v"       -> die("dwm-6.8")            // versão
  setlocale(LC_CTYPE,"") && XSupportsLocale()        // i18n p/ títulos UTF-8
  dpy = XOpenDisplay(NULL)                            // conecta ao X server ($DISPLAY)
  checkotherwm()                                      // já tem WM? aborta
  setup()                                             // vira o WM, cria barra/atoms/cursores
  pledge("stdio rpath proc exec")                     // OpenBSD sandbox (no-op no Linux)
  scan()                                              // adota janelas pré-existentes
  run()                                               // loop de eventos (roda até quit)
  cleanup()                                            // desfaz tudo
  XCloseDisplay(dpy)
```

`XOpenDisplay(NULL)` (dwm.c:2152) abre a conexão socket com o X server em `$DISPLAY`
(tipicamente `/tmp/.X11-unix/X0`). Retorna `Display*`, o handle de toda chamada Xlib. Tudo
depois disso é "mande requisição pelo socket, às vezes espere resposta".

**→ ntwm:** `XOpenDisplay` = `CreateFileA(NTU_PIPE_DISPD, ...)` + mandar `HELLO ntwm 1`.
`checkotherwm` some (o pipe só aceita um cliente por vez de qualquer jeito; um segundo
`ConnectNamedPipe` só é atendido quando o primeiro cai — wmproto.c:238-267).

### 2.2 `checkotherwm()` (dwm.c:459-468) — o teste "já existe um WM?"

```c
xerrorxlib = XSetErrorHandler(xerrorstart);      // instala handler que morre no 1º erro
XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask);  // tenta virar WM
XSync(dpy, False);                                // força o flush; erro chega AGORA
XSetErrorHandler(xerror);                         // volta pro handler tolerante
XSync(dpy, False);
```

Truque clássico: pede `SubstructureRedirectMask` no root. Se **já** houver um WM, o X server
responde `BadAccess` (só um cliente pode ter esse bit). `xerrorstart` (dwm.c:2124-2129) chama
`die("another window manager is already running")`. Como `XSelectInput` é assíncrona, o
`XSync(dpy, False)` (dwm.c:465) é obrigatório: ele bloqueia até o server processar e devolver o
erro, disparando o handler. Sem o XSync o erro chegaria "depois", tarde demais.

**→ ntwm:** não precisa. `wmproto.c` já garante 1 cliente. Se quiser paridade, checar retorno
de `CreateFile`/`WriteFile(HELLO)`.

### 2.3 `setup()` (dwm.c:1539-1614) — **como um processo VIRA o window manager**

Ordem exata, com o porquê de cada bloco:

1. **SIGCHLD → SIG_IGN + reap** (dwm.c:1547-1554): `sigaction(SIGCHLD, {SA_NOCLDWAIT})` para
   filhos (terminais lançados via `spawn`) não virarem zumbis, e `waitpid(WNOHANG)` limpa
   zumbis herdados do `.xinitrc`.
   → ntwm: no NT o análogo é `CloseHandle` no `PROCESS_INFORMATION` do spawn (não há zumbis
   NT); dispd já faz `reap_dead()` (dispd.c:134).
2. **Geometria da tela** (dwm.c:1557-1560): `screen = DefaultScreen(dpy)`;
   `sw = DisplayWidth`; `sh = DisplayHeight`; `root = RootWindow(dpy, screen)`. O root é a
   janela-raiz que cobre a tela inteira; é o "pai de todas".
   → ntwm: vem no evento `OUTPUT 0 0 0 W H` do snapshot (wmproto.c:148).
3. **drw** (dwm.c:1561-1565): `drw_create(dpy, screen, root, sw, sh)` cria a camada de desenho
   (§10). Carrega a fonte (`drw_fontset_create`), calcula `lrpad` (padding) e `bh` (bar height
   = altura da fonte + 2).
4. **Monitores** (dwm.c:1566): `updategeom()` (§4.3) descobre monitores via Xinerama.
5. **Átomos** (dwm.c:1568-1581): `XInternAtom` registra/resolve nomes de propriedade
   (`WM_PROTOCOLS`, `WM_DELETE_WINDOW`, `_NET_WM_NAME`, `_NET_WM_STATE_FULLSCREEN`,
   `_NET_ACTIVE_WINDOW`, `_NET_CLIENT_LIST` …). Átomo = inteiro que o server dá pra cada string,
   pra não trafegar strings toda hora. Detalhado em §11.
6. **Cursores** (dwm.c:1583-1585): `drw_cur_create` → `XCreateFontCursor(XC_left_ptr/XC_sizing/
   XC_fleur)`. Ponteiro normal, de resize, de move.
7. **Esquema de cores** (dwm.c:1587-1589): `drw_scm_create` aloca cada cor (fg/bg/border) via
   Xft.
8. **Barra** (dwm.c:1591): `updatebars()` cria a janela da barra por monitor (§10).
9. **Status** (dwm.c:1592): `updatestatus()` lê `XA_WM_NAME` do root (o `xsetroot -name` clássico).
10. **NetWMCheck** (dwm.c:1594-1600): cria `wmcheckwin` (janela 1×1 invisível) e põe
    `_NET_SUPPORTING_WM_CHECK` nela e no root, com `_NET_WM_NAME = "dwm"`. É como pagers/
    barras externas detectam "há um WM EWMH-compliant rodando".
11. **_NET_SUPPORTED** (dwm.c:1602-1603): publica no root a lista de átomos EWMH que dwm honra.
12. **Limpa a client list** (dwm.c:1604): `XDeleteProperty(root, _NET_CLIENT_LIST)`.
13. **★ Vira o WM de verdade** (dwm.c:1606-1611):
    ```c
    wa.cursor = cursor[CurNormal]->cursor;
    wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask
        |ButtonPressMask|PointerMotionMask|EnterWindowMask
        |LeaveWindowMask|StructureNotifyMask|PropertyChangeMask;
    XChangeWindowAttributes(dpy, root, CWEventMask|CWCursor, &wa);
    XSelectInput(dpy, root, wa.event_mask);
    ```
    Este é **o** momento. `SubstructureRedirectMask` no root = "o X server, em vez de executar
    `MapWindow`/`ConfigureWindow` que qualquer cliente pedir para as janelas-filhas do root,
    **redireciona** essas requisições pra mim como eventos `MapRequest`/`ConfigureRequest`".
    É assim que dwm intercepta janelas nascendo e decide onde vão. `SubstructureNotifyMask`
    adiciona os avisos (`CreateNotify`/`DestroyNotify`/`UnmapNotify`). Os outros bits pedem
    cliques na barra/root, movimento do mouse (troca de monitor), enter/leave
    (focus-follows-mouse) e mudança de propriedade.
14. **grabkeys()** (dwm.c:1612): registra os atalhos globais (§6.4).
15. **focus(NULL)** (dwm.c:1613): foco inicial (no root).

**→ ntwm:** o passo 13 é o coração e é **exatamente o `HELLO`**. dispd já é dono da tela
(criou a root fullscreen em dispd.c:164-179 e é ele que decide, no `apply(PLACE…)`, para onde
cada janela vai). O ntwm não pede `SubstructureRedirect` — ele **é** o consumidor
privilegiado do pipe. Os passos 5/10/11/12 (átomos, NetWMCheck, _NET_SUPPORTED) não existem
no NTUnix (não há clientes X observando o root); no lugar deles o ntwm só precisa registrar
seus `GRAB`s (= passo 14) e mandar o primeiro `FRAME`.

### 2.4 `scan()` (dwm.c:1393-1418) — adotar janelas que já existiam

Quando o WM sobe **depois** de já haver janelas (ex.: reiniciou o dwm), ele precisa "encontrar"
essas janelas órfãs e gerenciá-las:

```c
if (XQueryTree(dpy, root, &d1, &d2, &wins, &num)) {   // lista todas as filhas do root
    for (i = 0; i < num; i++) {                        // 1ª passada: janelas normais
        if (!XGetWindowAttributes(dpy, wins[i], &wa)
        || wa.override_redirect || XGetTransientForHint(dpy, wins[i], &d1))
            continue;                                  // pula override_redirect e transientes
        if (wa.map_state == IsViewable || getstate(wins[i]) == IconicState)
            manage(wins[i], &wa);
    }
    for (i = 0; i < num; i++) {                         // 2ª passada: transientes (dialogs)
        if (XGetTransientForHint(dpy, wins[i], &d1)
        && (wa.map_state == IsViewable || getstate(wins[i]) == IconicState))
            manage(wins[i], &wa);
    }
    XFree(wins);
}
```

- `XQueryTree` devolve o array de filhas do root (a lista de todas as top-levels).
- Duas passadas porque um dialog transiente precisa que a janela-mãe já esteja gerenciada
  quando ele for adotado (manage lê `XGetTransientForHint`, dwm.c:1048).
- `map_state == IsViewable` = já está mapeada (visível); `IconicState` = minimizada mas quer ser
  gerenciada. Janelas retiradas (`WithdrawnState`) são ignoradas.

**→ ntwm:** é **literalmente** o snapshot. Ao `HELLO`, dispd manda `WELCOME`/`OUTPUT`, depois
um `WINDOW wid kind pid title` por janela viva, e `SYNC` (wmproto.c:143-156). `scan()` = "para
cada `WINDOW` recebido até o `SYNC`, crie um Client no meu modelo". Sem `XQueryTree`, sem
`override_redirect` (dispd nunca cria surface pra menu de ninguém — não há), sem 2 passadas.

### 2.5 `run()` (dwm.c:1382-1391) — o loop

```c
void run(void) {
    XEvent ev;
    XSync(dpy, False);
    while (running && !XNextEvent(dpy, &ev))     // bloqueia até chegar evento
        if (handler[ev.type])
            handler[ev.type](&ev);               // despacho O(1) pela tabela
}
```

`XNextEvent` **bloqueia** lendo o socket X até chegar um evento, preenche `ev`, e o
`ev.type` (inteiro pequeno, `ButtonPress`, `MapRequest`, …) indexa direto o `handler[]`
(dwm.c:245-260). É um interpretador de eventos. `running` é zerado por `quit()` (dwm.c:1259).

**→ ntwm:** `XNextEvent` = ler uma linha do pipe. O `ev.type` = o verbo (`WINDOW-CREATED`,
`KEY`, `WINDOW-DESTROYED`, `WINDOW-TITLE`, `SYNC`). A tabela `handler[]` vira um `if/else` por
verbo (ou uma tabela verbo→função). **Diferença crucial de altitude:** dwm reage a eventos de
*mecanismo* de baixo nível (mapa/unmapa/configura/expõe/propriedade); o ntwm reage a eventos de
*intenção* já digeridos pelo dispd (janela nasceu/morreu, tecla-atalho foi pressionada). O
ntwm tem **muito** menos tipos de evento pra tratar — dispd absorveu Expose, ConfigureNotify,
MapRequest, PropertyNotify(título), etc.

### 2.6 `cleanup()` (dwm.c:470-496) — desfazer tudo

```c
view(&(Arg){.ui = ~0});                 // mostra todas as tags (pra nada ficar escondido)
selmon->lt[...] = &(Layout){"", NULL};  // layout flutuante (não re-arranja no teardown)
for cada monitor: while (m->stack) unmanage(m->stack, 0);  // devolve cada janela ao "estado livre"
XUngrabKey(dpy, AnyKey, AnyModifier, root);
while (mons) cleanupmon(mons);          // destrói barras
drw_*free / free(scheme);
XDestroyWindow(dpy, wmcheckwin);
XSetInputFocus(dpy, PointerRoot, RevertToPointerRoot, CurrentTime);  // devolve foco ao root
XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
```

`unmanage(c, 0)` com `destroyed=0` (dwm.c:1778-1802) **devolve** cada janela ao mundo:
restaura a border width original, tira os grabs, marca `WithdrawnState`. dwm sai limpo e as
janelas dos apps continuam vivas para o próximo WM.

**→ ntwm:** mandar `FRAME-BEGIN`+`PLACE` de tudo visível se quiser, `UNGRAB` de cada atalho,
`QUIT` opcional. Como o dispd é dono do estado, o ntwm pode simplesmente fechar o pipe — dispd
segue rodando com `builtin_layout()` (dispd.c:135-136). Isso é **mais robusto** que dwm: em
dwm, sair sem `cleanup` deixa janelas com bordas/grabs zoados; no NTUnix, o dispd se recupera
sozinho.

---

## 3. Como dwm "vira" o WM: `SubstructureRedirectMask` a fundo

Este é o conceito mais importante do X11 para WMs, então detalho no nível de protocolo.

No X11, **qualquer** cliente pode chamar `XMapWindow`, `XConfigureWindow`,
`XCirculateWindow` numa janela. Normalmente o server executa direto. Mas se **algum** cliente
tiver selecionado `SubstructureRedirectMask` nas janela-**pai**, o server **não executa** —
em vez disso ele converte a requisição num **evento** (`MapRequest`, `ConfigureRequest`,
`CirculateRequest`) e o entrega a esse cliente. A operação fica *pendente* até o cliente
privilegiado decidir.

Consequências:
- Selecionando esse bit no **root**, dwm intercepta o map/configure de **todas** as top-levels
  (filhas diretas do root). É assim que "toda janela nova passa pelo WM": o app chama
  `XMapWindow(minhajanela)`, o server manda `MapRequest` pro dwm, dwm roda `maprequest()` →
  `manage()` → decide geometria → só então `XMapWindow` de verdade.
- **Só um cliente por janela** pode ter `SubstructureRedirect` (senão dois WMs brigariam pela
  mesma requisição). Daí o "only one X connection" do cabeçalho e o teste do `checkotherwm()`.
- `override_redirect=True` numa janela diz "server, execute meu map/configure **sem**
  redirecionar" — é como menus/tooltips/dmenu escapam do WM. dwm as ignora (maprequest checa
  `wa.override_redirect`, dwm.c:1107).

Contraste com o modelo NTUnix: **não há server neutro que outros clientes acessem.** O dispd é
dono de tudo, então não existe "redirecionar a requisição de outro cliente" — o ntwm simplesmente
recebe `WINDOW-CREATED` quando o dispd cria uma surface (por `SPAWN-TERM` ou por um app que se
registre). O poder que o `SubstructureRedirectMask` dá ao dwm (vetar/reposicionar antes de
exibir) o ntwm tem **de graça**, porque nada aparece na tela sem o dispd compor, e o dispd só
compõe onde o ntwm mandou `PLACE`.

---

## 4. O modelo de dados: Client, Monitor, tags, listas

### 4.1 `struct Client` (dwm.c:86-99)

```c
struct Client {
    char name[256];                 // título (WM_NAME / _NET_WM_NAME)
    float mina, maxa;               // aspect ratio min/max (size hints)
    int x, y, w, h;                 // geometria atual da ÁREA CLIENTE (sem borda)
    int oldx, oldy, oldw, oldh;     // geometria anterior (p/ restaurar de fullscreen/float)
    int basew, baseh, incw, inch, maxw, maxh, minw, minh, hintsvalid;  // WM_NORMAL_HINTS
    int bw, oldbw;                  // border width atual / anterior
    unsigned int tags;              // BITMASK: em quais tags esta janela aparece
    int isfixed, isfloating, isurgent, neverfocus, oldstate, isfullscreen;  // flags
    Client *next;                   // próximo na lista de CLIENTS do monitor (ordem de tiling)
    Client *snext;                  // próximo na lista de STACK do monitor (ordem de foco/Z)
    Monitor *mon;                   // monitor dono
    Window win;                     // ★ o XID da janela real no X server
};
```

O campo-chave é `win` — o identificador de 32 bits que o X server deu à janela. **Todo** o
trabalho de dwm é: mapear eventos → achar o `Client` cujo `win` bate (`wintoclient`, dwm.c:2065)
→ mexer nele → empurrar a geometria de volta via `win`.

**→ ntwm:** o `Window` do dispd (dispd.h:18-41) já é quase esse Client, mas do lado do
*mecanismo*. O ntwm precisa da sua **própria** `struct Client` espelhando o estado de política
(tags, isfloating, mina/maxa, etc.) e guardando `unsigned id` (o `wid` do protocolo) no lugar
de `Window win`. Onde dwm tem `c->win`, ntwm tem `c->id` e o manda em `PLACE <id> …`.

### 4.2 `struct Monitor` (dwm.c:113-132)

```c
struct Monitor {
    char ltsymbol[16];          // símbolo do layout atual ("[]=", "><>", "[M]")
    float mfact;                // fração da largura pro master (0.55)
    int nmaster;                // quantas janelas no master
    int num;                    // índice do monitor
    int by;                     // y da barra
    int mx,my,mw,mh;            // retângulo do MONITOR (tela física)
    int wx,wy,ww,wh;            // retângulo da ÁREA ÚTIL (monitor menos barra)
    unsigned int seltags;       // 0 ou 1: qual dos dois tagsets está ativo
    unsigned int sellt;         // 0 ou 1: qual dos dois layouts está ativo
    unsigned int tagset[2];     // dois conjuntos de tags (atual + anterior, p/ Mod+Tab)
    int showbar, topbar;
    Client *clients;            // lista em ordem de TILING
    Client *sel;                // cliente selecionado (focado) neste monitor
    Client *stack;              // lista em ordem de STACK/foco (mais recente na frente)
    Monitor *next;              // próximo monitor
    Window barwin;              // janela real da barra
    const Layout *lt[2];        // dois layouts (atual + anterior)
};
```

Observe os **dois** de quase tudo (`tagset[2]`, `lt[2]`, `sellt`, `seltags`): dwm guarda o
estado *anterior* para permitir "voltar" com uma tecla (Mod+Tab alterna tagset; Mod+Space
alterna layout). É um padrão barato de undo de 1 nível.

**Duas listas por monitor, e por que são separadas:**
- `clients` (ligada por `next`): **ordem espacial** — a ordem em que o `tile()` empilha as
  janelas na tela. `attach()` insere na frente (dwm.c:403-408). `zoom()`/`pop()` reordena.
- `stack` (ligada por `snext`): **ordem temporal de foco/Z** — o topo é o mais recentemente
  focado. `focus()` faz `detachstack`+`attachstack` pra trazer o focado ao topo (dwm.c:800-801).
  `restack()` usa isso pra ordem Z real no server (dwm.c:1372).

Separar as duas é o que permite "tiling estável mesmo mudando o foco": mexer no foco reordena
só `stack`, não `clients`, então o layout não pula.

### 4.3 `updategeom()` / `createmon()` / multi-monitor (dwm.c:1867-1943, 632-647)

`updategeom()` usa Xinerama (`XineramaQueryScreens`) para descobrir os retângulos dos monitores,
deduplicando geometrias idênticas (`isuniquegeom`, dwm.c:988-996). Cria/remove `Monitor`s
conforme monitores aparecem/somem, e re-encaixa clientes de monitores removidos no primeiro
monitor. Sem Xinerama, faz um monitor único do tamanho da tela (dwm.c:1928-1937).

**→ ntwm:** vem dos eventos `OUTPUT <oid> x y w h` (pode haver vários). O ntwm cria um Monitor
por OUTPUT. Bem mais simples: dispd já entrega retângulos limpos, sem Xinerama nem dedup.

### 4.4 tags — o modelo mental que define dwm

- `tags[]` (config.h:22) = `{"1".."9"}`. Cada tag é **um bit**. `TAGMASK` (dwm.c:56) =
  `(1<<9)-1`.
- `c->tags` = bitmask: em quais tags a janela aparece (pode ser várias ao mesmo tempo).
- `m->tagset[m->seltags]` = bitmask: quais tags estão sendo **exibidas** agora.
- `ISVISIBLE(C)` (dwm.c:52) = `C->tags & C->mon->tagset[C->mon->seltags]` — a janela é visível
  sse **algum** bit de tag dela está no conjunto exibido. Esse macro é usado em **todo** lugar:
  tile pula invisíveis (`nexttiled`, dwm.c:1205-1210), showhide move as invisíveis pra fora, etc.

Tags são um superconjunto de "workspaces": um workspace é o caso de exibir exatamente 1 tag.
`view` (dwm.c:2054-2063) troca o tagset exibido; `tag` (dwm.c:1669-1677) move a janela pra
outra tag; `toggleview`/`toggletag` mexem bit-a-bit.

**→ ntwm:** o dispd tem um `int ws` por janela e um `int cur_ws` global (dispd.h:26, 66) — ou
seja, o dispd modela **workspaces (1 tag exclusiva)**, não o bitmask completo de dwm. **O ntwm
deve manter o bitmask de tags no seu próprio Client** e traduzir na hora do `PLACE`: mandar
`ws = índice-da-tag-de-menor-bit` ou simplesmente `ws = workspace-atual` se for adotar o modelo
workspace. Como o protocolo `PLACE` já carrega `<ws>` e o dispd filtra visibilidade por `ws`
(`compose_and_present`, compositor.c:207-209), a decisão "que janela aparece" pode ficar
**inteiramente no ntwm**: ele só manda `PLACE` das visíveis com `ws = cur_ws`, e some (não dá
`PLACE`, ou dá com `ws` diferente) as escondidas. Recomendação: ntwm mantém tags estilo dwm
internamente e projeta pro campo `ws` do protocolo.

---

## 5. O substrato Xlib — o catálogo de chamadas que dwm usa (dwm como aula de Xlib)

Aqui está o valor "entender o X por dentro". Cada chamada, o que faz no nível de protocolo, e o
equivalente NTUnix.

| Xlib | O que faz no protocolo X | Onde em dwm | Equivalente ntwm/dispd |
|---|---|---|---|
| `XOpenDisplay(NULL)` | Abre socket ao X server em `$DISPLAY`; devolve `Display*` | main:2152 | `CreateFile(NTU_PIPE_DISPD)` + `HELLO` |
| `XSelectInput(w, mask)` | Pede ao server para **enviar** eventos de `mask` ocorridos em `w` | setup:1611, manage:1071 | implícito: dispd manda eventos pelo pipe |
| `XChangeWindowAttributes(w, CWEventMask\|CWCursor, &wa)` | Muda atributos da janela (máscara de evento, cursor) | setup:1610 | n/a (dispd é dono) |
| `XCreateWindow`/`XCreateSimpleWindow` | Cria uma janela (a barra, o wmcheckwin) no server | updatebars:1831, setup:1594 | dispd `win_create()` (compositor.c:69) |
| `XMapWindow`/`XMapRaised` | Torna a janela visível (e no topo) | manage:1087, updatebars:1835 | `PLACE` com z alto / `visible=1` |
| `XUnmapWindow` | Esconde a janela (sem destruir) | cleanupmon:509 | não dar `PLACE`, ou `visible=0` |
| `XDestroyWindow` | Destrói a janela no server | cleanup:491, cleanupmon:510 | `CLOSE <wid>` (wmproto apply:196) |
| `XMoveResizeWindow(w, x,y,w,h)` | **Reposiciona+redimensiona** a janela | resizeclient(via XConfigureWindow), manage:1081, bar:1720 | **`PLACE <wid> x y w h ws z`** |
| `XMoveWindow(w, x,y)` | Só reposiciona (usado no show/hide) | showhide:1636,1643 | idem, via `PLACE` |
| `XConfigureWindow(w, mask, &wc)` | Muda geometria/border/ordem-Z (`CWX/CWY/CWWidth/CWHeight/CWBorderWidth/CWSibling/CWStackMode`) | resizeclient:1295, restack:1374, manage:1065 | `PLACE` (geom) + `BORDER` (borda) + ordem via `z` |
| `XSetInputFocus(w, revert, time)` | Define quem recebe teclado | setfocus:1476, unfocus:1773, cleanup:494 | **`FOCUS <wid>`** |
| `XSetWindowBorder(w, pixel)` | Cor da borda da janela | focus:803, unfocus:1771, manage:1066 | **`BORDER <wid> px rrggbb`** |
| `XRaiseWindow(w)` | Põe a janela no topo do Z-order | manage:1076, restack:1368, setfullscreen:1494 | `z` alto no `PLACE` |
| `XGrabKey(kc, mods, root, ..., GrabModeAsync)` | Registra atalho global: entrega `KeyPress` ao WM quando a combinação é pressionada em qualquer lugar | grabkeys:972 | **`GRAB <mods> <vk>`** (wmproto apply:203) |
| `XGrabButton(...)` | Idem para botões do mouse (ex.: Mod+click) | grabbuttons:945 | (a implementar: `GRAB` de botão) |
| `XUngrabKey`/`XUngrabButton` | Desfaz grabs | cleanup:483, grabbuttons:938 | **`UNGRAB`** |
| `XGrabPointer(...)` | Captura **todo** o mouse (durante move/resize interativo) | movemouse:1161, resizemouse:1316 | dispd captura mouse na root; move/resize interativo seria um modo no ntwm |
| `XGrabServer`/`XUngrabServer` | **Congela o server inteiro** (nenhum outro cliente é atendido) — usado para operações atômicas em janelas que podem estar morrendo | killclient:1021, unmanage:1788 | **desnecessário** — `FRAME-BEGIN/COMMIT` já é atômico |
| `XChangeProperty(w, atom, ...)` | Escreve uma propriedade (metadado) na janela | manage:1079 (`_NET_CLIENT_LIST`), setfocus:1477, setclientstate:1443 | eventos do protocolo (título vem via `WINDOW-TITLE`) |
| `XGetWindowProperty` | Lê propriedade | getatomprop:871, getstate:899 | n/a |
| `XGetWindowAttributes(w, &wa)` | Lê geometria/estado/override_redirect da janela | maprequest:1107, scan:1402 | vem no `WINDOW`/`WINDOW-CREATED` |
| `XGetWMNormalHints` (`XGetWMSizeHints`) | Lê `WM_NORMAL_HINTS` (min/max/inc/aspect) | updatesizehints:1967 | (opcional) app poderia mandar hints via protocolo |
| `XGetTransientForHint` | "esta janela é um dialog de qual?" | manage:1048, scan:1403 | flag `kind`/campo no protocolo |
| `XGetWMProtocols` | Lê `WM_PROTOCOLS` (suporta WM_DELETE? WM_TAKE_FOCUS?) | sendevent:1455 | `CLOSE` sempre suportado pelo dispd |
| `XSendEvent(w, ..., &ev)` | Injeta um evento sintético no cliente (fechar educadamente, dar foco) | sendevent:1467, configure:549 | `CLOSE` (dispd fecha o pty/processo) |
| `XKillClient(w)` | Mata a conexão do cliente à força (fecha a janela sem dó) | killclient:1024 | `CLOSE` + dispd mata o processo |
| `XQueryTree(root, ...)` | Lista as janelas-filhas (top-levels) | scan:1400 | snapshot `WINDOW…`+`SYNC` |
| `XQueryPointer(root, ...)` | Onde está o mouse agora | getrootptr:887 | dispd sabe (WM_MOUSEMOVE) |
| `XWarpPointer` | Teleporta o cursor | resizemouse:1319,1347 | `SetCursorPos` no dispd |
| `XInternAtom(name, ...)` | String→Atom (id de propriedade) | setup:1568-1581 | n/a |
| `XSync(dpy, False)` | **Flush + espera** o server processar tudo (barreira) | espalhado | n/a (pipe é ordenado; `FRAME-COMMIT` é a barreira) |
| `XNextEvent(dpy, &ev)` | Bloqueia até o próximo evento | run:1388 | `ReadFile` do pipe |
| `XSetErrorHandler` | Instala callback de erro assíncrono do X | checkotherwm:462, setup, killclient | checar retorno de `WriteFile` |

O padrão profundo do X que dwm revela: **quase toda chamada Xlib é assíncrona** — vira uma
requisição no socket que o server processa depois. Erros voltam **depois**, por um handler
global (`xerror`, dwm.c:2098-2114), que dwm usa para **ignorar** erros esperados (janela que
morreu no meio de uma operação: `BadWindow`, `BadMatch` em `SetInputFocus`, etc.). Essa
tolerância a "a janela sumiu embaixo de mim" é metade da robustez de um WM X11 — e é
**exatamente a classe de bug que o modelo dispd/ntwm elimina**, porque o dispd é dono do estado
e o `wid` só é válido enquanto o dispd disser (o ntwm nunca corre atrás de uma janela que já
morreu; ele recebe `WINDOW-DESTROYED` e pronto).

---

## 6. O loop de eventos e cada handler

A tabela `handler[LASTEvent]` (dwm.c:245-260) mapeia `XEvent.type` → função. `LASTEvent` é o
maior número de tipo de evento do X. Índices não preenchidos são `NULL` (ignorados no run,
dwm.c:1389). Vou por cada um, com o evento X explicado.

### 6.1 `MapRequest` → `maprequest()` (dwm.c:1101-1111) — **janela nova quer aparecer**

Evento X: gerado pelo server (por causa do `SubstructureRedirectMask`) quando um cliente chama
`XMapWindow` na sua top-level. A janela **ainda não está visível** — o map foi interceptado.

```c
if (!XGetWindowAttributes(dpy, ev->window, &wa) || wa.override_redirect) return;  // ignora menus etc
if (!wintoclient(ev->window)) manage(ev->window, &wa);  // se ainda não gerenciada, adota
```

Esse é o ponto de entrada de **toda** janela nova no dwm. `manage()` (§7) decide tudo e só no
fim faz o `XMapWindow` real.

**→ ntwm:** = evento **`WINDOW-CREATED <wid> <kind> <pid> <title>`** (wmproto.c:86-92). Não há
override_redirect (dispd nunca cria surface para transientes/menus como janela top-level
gerenciável). O handler do ntwm: cria Client, aplica regras, calcula tags, chama `arrange`.

### 6.2 `ConfigureRequest` → `configurerequest()` (dwm.c:580-630) — **cliente pediu pra se mover/redimensionar**

Evento X: interceptado (SubstructureRedirect) quando um cliente chama `XConfigureWindow` em si
mesmo (querendo x/y/w/h/border/ordem). O WM decide se **honra** ou **sobrescreve**:

```c
if ((c = wintoclient(ev->window))) {          // é uma janela gerenciada?
    if (ev->value_mask & CWBorderWidth)
        c->bw = ev->border_width;             // honra mudança de borda
    else if (c->isfloating || !arrange) {     // FLUTUANTE ou layout floating: honra geom
        // aplica ev->x/y/width/height ao Client, centraliza se sair da tela
        if (ISVISIBLE(c)) XMoveResizeWindow(dpy, c->win, c->x, c->y, c->w, c->h);
    } else
        configure(c);                          // TILED: IGNORA o pedido, só reafirma a geom atual
} else {                                       // janela não gerenciada (ex.: durante scan): repassa cru
    wc = {ev->x, ev->y, ...}; XConfigureWindow(dpy, ev->window, ev->value_mask, &wc);
}
```

A regra de ouro do tiling: **janela tiled não escolhe seu tamanho** — se ela pede resize, dwm
responde com um `configure(c)` (dwm.c:533-550) que é um `XSendEvent` de `ConfigureNotify`
sintético dizendo "seu tamanho é o que EU digo" (o tamanho atual). Só janelas flutuantes têm o
pedido honrado.

**→ ntwm:** **em grande parte desaparece.** No modelo dispd, um app não "pede resize" ao WM por
um caminho assíncrono — o dispd é dono do tamanho da surface (`win_set_client_size`,
compositor.c:127). Se no futuro o protocolo `ntuwm.h` ganhar um evento tipo
`WINDOW-RESIZE-REQUEST`, o ntwm aplicaria a mesma política (honra se floating, ignora se tiled).
Por ora, não há análogo — o ntwm manda `PLACE` e o dispd obedece. **Menos código, menos corrida.**

### 6.3 `ConfigureNotify` → `configurenotify()` (dwm.c:552-578) — **a tela mudou de tamanho**

Evento X: aviso de que uma janela **foi** reconfigurada. dwm só se importa quando é o **root**
(resolução da tela mudou, monitor plugado):

```c
if (ev->window == root) {
    dirty = (sw != ev->width || sh != ev->height);
    sw = ev->width; sh = ev->height;
    if (updategeom() || dirty) {
        drw_resize(drw, sw, bh);         // redimensiona o pixmap da barra
        updatebars();
        // reposiciona fullscreens e as barras; refoca; re-arranja tudo
    }
}
```

**→ ntwm:** = um novo evento `OUTPUT` (mudança de resolução/monitor). Handler: atualiza os
retângulos dos Monitors e re-arranja.

### 6.4 `KeyPress` → `keypress()` (dwm.c:999-1013) — atalho

```c
keysym = XKeycodeToKeysym(dpy, ev->keycode, 0);   // keycode físico -> keysym lógico
for (i = 0; i < LENGTH(keys); i++)
    if (keysym == keys[i].keysym
    && CLEANMASK(keys[i].mod) == CLEANMASK(ev->state)   // modificadores batem (limpos)
    && keys[i].func)
        keys[i].func(&(keys[i].arg));
```

Só chega aqui porque `grabkeys()` (dwm.c:952-978) registrou essas combinações no server via
`XGrabKey`. `CLEANMASK` (dwm.c:49) remove NumLock e CapsLock da máscara antes de comparar (§6.11).
O `keys[]` (config.h:64-99) é a tabela `{mod, keysym, func, arg}`.

**→ ntwm:** = evento **`KEY <mods> <vk>`** (wmproto.c:108-113; input.c:98-100). O dispd já fez o
"grab": só teclas registradas via `GRAB` viram evento `KEY`; as outras vão pro pty (input.c). O
`keypress` do ntwm é um lookup na sua `keys[]` por `(mods, vk)`. **`mods` já vem limpo** — o
`cur_mods()` do dispd (input.c:16-25) só olha ALT/CTRL/SHIFT/WIN, sem NumLock/CapsLock, então
**o CLEANMASK é desnecessário no ntwm** (o dispd resolveu). VKs são códigos Win32 (`'Q'`, `VK_RETURN`)
em vez de KeySyms X.

### 6.5 `ButtonPress` → `buttonpress()` (dwm.c:417-457) — clique

Determina **onde** foi o clique (`ClkTagBar`/`ClkLtSymbol`/`ClkStatusText`/`ClkWinTitle` na
barra, `ClkClientWin` numa janela, `ClkRootWin` no fundo), calculando a região X da barra
(dwm.c:433-446), e despacha pela `buttons[]` (config.h:103-116) casando click+button+modifiers.
`XAllowEvents(ReplayPointer)` (dwm.c:450) reenvia o clique ao cliente depois de focá-lo (o
"click to focus" não come o clique).

**→ ntwm:** o dispd hoje faz `win_focus` no `WM_LBUTTONDOWN` (dispd.c:149-153) — política
embutida. Para paridade com dwm, dispd deveria mandar um evento `BUTTON <mods> <btn> <x> <y> <wid>`
pro ntwm decidir. A lógica de "onde na barra cliquei" fica no ntwm (que é quem desenha a barra —
§10).

### 6.6 `EnterNotify` → `enternotify()` (dwm.c:759-776) — **focus-follows-mouse**

Evento X: o ponteiro entrou numa janela. dwm foca a janela sob o cursor:

```c
if ((ev->mode != NotifyNormal || ev->detail == NotifyInferior) && ev->window != root) return;
c = wintoclient(ev->window);
m = c ? c->mon : wintomon(ev->window);
if (m != selmon) { unfocus(selmon->sel, 1); selmon = m; }
else if (!c || c == selmon->sel) return;
focus(c);
```

Os filtros (`NotifyNormal`, `NotifyInferior`) evitam re-foco espúrio quando o cursor cruza
sub-janelas ou por causa de grabs. É *o* comportamento que define dwm ("sloppy focus").

**→ ntwm:** o dispd sabe onde o mouse está e qual surface está embaixo (`win_at_point`,
compositor.c:164-176). Para focus-follows-mouse, dispd manda `FOCUS`-request ou um evento
`POINTER-ENTER <wid>` e o ntwm responde `FOCUS <wid>`. Alternativa: deixar a política de foco no
ntwm e o dispd só reportar movimento. Hoje o dispd faz clique-para-focar embutido; a versão
"dwm" moveria isso pro ntwm.

### 6.7 `DestroyNotify` → `destroynotify()` (dwm.c:649-657) — janela destruída

```c
if ((c = wintoclient(ev->window))) unmanage(c, 1);   // destroyed=1: não mexe mais no win
```

**→ ntwm:** = evento **`WINDOW-DESTROYED <wid>`** (wmproto.c:94-99). Handler: `unmanage`
(remove do modelo, re-arranja). Como o dispd já destruiu a surface, o ntwm só limpa o Client —
`destroyed=1` sempre (nunca há "devolver a janela ao mundo").

### 6.8 `UnmapNotify` → `unmapnotify()` (dwm.c:1804-1816) — janela escondida

```c
if ((c = wintoclient(ev->window))) {
    if (ev->send_event) setclientstate(c, WithdrawnState);   // unmap "educado" do cliente
    else unmanage(c, 0);                                     // unmap real: para de gerenciar
}
```

Distingue unmap sintético (cliente pediu withdraw) de real. No NTUnix isso colapsa em
`WINDOW-DESTROYED` (não há o conceito de "unmap mas não destruir" separado — se some da tela, o
ntwm para de dar `PLACE`).

### 6.9 `Expose` → `expose()` (dwm.c:778-786) — precisa redesenhar

Evento X: uma região da janela ficou "suja" e precisa ser repintada. dwm só liga pra barra:
`if (ev->count == 0) drawbar(m)` (redesenha quando é a última exposição da fila).

**→ ntwm:** **não existe.** O dispd redesenha tudo todo tick (compose_and_present a ~60fps,
compositor.c:196). Sem eventos Expose — o compositor é retido (retained), não sob demanda como o
X. Grande simplificação: o ntwm nunca trata "repinte-se".

### 6.10 `PropertyNotify` → `propertynotify()` (dwm.c:1221-1256) — metadado mudou

Reage a mudanças de propriedade da janela: `XA_WM_NAME`/`_NET_WM_NAME` (título → `updatetitle`
+ redesenha barra), `XA_WM_NORMAL_HINTS` (invalida hints), `XA_WM_HINTS` (urgência),
`XA_WM_TRANSIENT_FOR` (virou dialog → flutuante), `_NET_WM_WINDOW_TYPE`. Também
`XA_WM_NAME` no **root** = mudança de status text (dwm.c:1228 → `updatestatus`).

**→ ntwm:** o único que sobrevive é o título: evento **`WINDOW-TITLE <wid> <title>`**
(wmproto.c:101-106; o dispd propaga do terminal em `pump_title`, compositor.c:180-194). Handler:
`updatetitle` + redesenhar barra. Hints/urgência/transient viriam por eventos futuros se
necessário.

### 6.11 `MappingNotify` → `mappingnotify()` (dwm.c:1091-1099) — layout de teclado mudou

`XRefreshKeyboardMapping(ev)` + se mudou o teclado, `grabkeys()` de novo (os keycodes podem ter
mudado). **→ ntwm:** raramente relevante (o dispd lida com VKs); pode ser ignorado.

### 6.12 `FocusIn` → `focusin()` (dwm.c:814-821) — reforça foco

```c
if (selmon->sel && ev->window != selmon->sel->win) setfocus(selmon->sel);
```

Alguns clientes "roubam" o foco; dwm o reafirma para a janela que ele acha que deve ter foco.
**→ ntwm:** desnecessário — só o dispd dá foco (via `FOCUS`), ninguém rouba.

### 6.13 `ClientMessage` → `clientmessage()` (dwm.c:514-531) — RPC do cliente

Trata `_NET_WM_STATE` (fullscreen: `setfullscreen`) e `_NET_ACTIVE_WINDOW` (cliente pede
atenção → marca urgente). É o canal EWMH pelo qual apps pedem coisas ao WM.
**→ ntwm:** se o protocolo ganhar `WINDOW-REQUEST-FULLSCREEN`, mapeia aqui.

### 6.14 `MotionNotify` → `motionnotify()` (dwm.c:1128-1143) — troca de monitor pelo mouse

Só no root: quando o mouse cruza pra outro monitor, troca `selmon` e refoca. **→ ntwm:** dispd
reporta ou trata; equivalente a trocar Monitor ativo.

---

## 7. `manage()` e `unmanage()` — adoção e despejo, linha a linha

### 7.1 `manage()` (dwm.c:1031-1089)

Este é o coração do "adotar uma janela". **Nota importante que o prompt levanta: dwm NÃO faz
reparenting.** WMs como mutter/kwin criam uma janela-moldura e fazem `XReparentWindow` (a janela
do app vira filha da moldura, que desenha título/bordas). dwm **não** — ele deixa a janela do
app como filha direta do root e só desenha a borda via `XSetWindowBorder` (a borda nativa do X,
`border_width`). É por isso que dwm não tem barras de título por janela. Isso simplifica
enormemente (nenhuma janela-moldura pra sincronizar) e é diretamente análogo ao NTUnix, onde a
"borda" é o dispd preenchendo `w->rect` com a cor da borda e fazendo `BitBlt` do conteúdo por
cima (compositor.c:224-232).

Passo a passo:
```c
c = ecalloc(1, sizeof(Client)); c->win = w;              // aloca Client, guarda o XID
c->x=c->oldx=wa->x; c->y=c->oldy=wa->y;                  // geometria inicial vinda do app
c->w=c->oldw=wa->width; c->h=c->oldh=wa->height;
c->oldbw = wa->border_width;
updatetitle(c);                                          // lê _NET_WM_NAME/WM_NAME
if (XGetTransientForHint(dpy,w,&trans) && (t=wintoclient(trans))) {
    c->mon = t->mon; c->tags = t->tags;                  // dialog herda monitor+tags do dono
} else { c->mon = selmon; applyrules(c); }               // senão aplica rules (class/instance/title)
// prende a janela dentro da área do monitor:
if (c->x + WIDTH(c) > c->mon->wx + c->mon->ww) c->x = ...;
c->x = MAX(c->x, c->mon->wx); c->y = MAX(c->y, c->mon->wy);
c->bw = borderpx;                                        // border width configurável (1px)
XConfigureWindow(dpy, w, CWBorderWidth, &wc);            // aplica a border width
XSetWindowBorder(dpy, w, scheme[SchemeNorm][ColBorder].pixel);  // cor da borda (normal)
configure(c);                                            // manda ConfigureNotify sintético
updatewindowtype(c);                                     // dialog? fullscreen?
updatesizehints(c);                                      // min/max/inc/aspect (WM_NORMAL_HINTS)
updatewmhints(c);                                        // urgência, input focus hint
XSelectInput(dpy, w, EnterWindowMask|FocusChangeMask|PropertyChangeMask|StructureNotifyMask);
grabbuttons(c, 0);                                       // registra Mod+click nesta janela
if (!c->isfloating) c->isfloating = c->oldstate = trans != None || c->isfixed;  // dialogs flutuam
if (c->isfloating) XRaiseWindow(dpy, c->win);
attach(c); attachstack(c);                               // entra nas DUAS listas
XChangeProperty(dpy, root, _NET_CLIENT_LIST, ..., PropModeAppend, &c->win, 1);  // EWMH
XMoveResizeWindow(dpy, c->win, c->x + 2*sw, c->y, c->w, c->h);  // ★ move pra FORA da tela primeiro
setclientstate(c, NormalState);
if (c->mon == selmon) unfocus(selmon->sel, 0);
c->mon->sel = c;
arrange(c->mon);                                         // ★ o tiling posiciona de verdade
XMapWindow(dpy, c->win);                                 // ★ SÓ AGORA a janela aparece
focus(NULL);
```

Dois truques a notar:
- **`c->x + 2*sw`** (dwm.c:1081): move a janela pra bem longe (2 telas à direita) **antes** de
  mapear, para não haver "flash" na posição errada; o `arrange()` logo em seguida a traz pra
  posição certa. É um workaround do X (map e move não são atômicos).
- A ordem **attach→property→map→arrange→focus** garante que a janela já está no modelo antes de
  ficar visível.

**→ ntwm:** todo esse "empurra border, manda ConfigureNotify, move pra fora, mapeia, arranja"
colapsa em: criar Client, `applyrules`, `arrange` (que emite `FRAME-BEGIN … PLACE … FRAME-COMMIT`).
Não há flash (o COMMIT é atômico), não há `+2*sw`, não há `XSelectInput` por janela. `configure()`
(o ConfigureNotify sintético) some. `grabbuttons` vira `GRAB` de botão (se implementado). A borda
inicial = `BORDER <wid> 2 <rrggbb-normal>`.

### 7.2 `unmanage()` (dwm.c:1778-1802)

```c
detach(c); detachstack(c);                        // sai das duas listas
if (!destroyed) {                                 // só se a janela ainda existe
    XGrabServer(dpy);                             // congela o server (atomicidade)
    XSetErrorHandler(xerrordummy);                // ignora erros (janela pode morrer)
    XSelectInput(dpy, c->win, NoEventMask);       // para de receber eventos dela
    XConfigureWindow(dpy, c->win, CWBorderWidth, &wc);  // restaura border original
    XUngrabButton(dpy, AnyButton, AnyModifier, c->win); // tira grabs de botão
    setclientstate(c, WithdrawnState);
    XSync; XSetErrorHandler(xerror); XUngrabServer(dpy);
}
free(c); focus(NULL); updateclientlist(); arrange(m);
```

O bloco `XGrabServer`/`xerrordummy` existe porque, entre `detach` e `setclientstate`, a janela
pode estar sendo destruída pelo cliente **em paralelo** — congelar o server evita a corrida.

**→ ntwm:** `unmanage` = tirar o Client das listas + `arrange` + (opcional) `updateclientlist`.
O bloco inteiro de `XGrabServer`/error-handler/`XSelectInput(NoEventMask)`/restaurar-borda
**evapora** — não há server compartilhado pra congelar, não há corrida (o dispd já sinalizou
`WINDOW-DESTROYED`, a surface já sumiu). Este é um dos maiores ganhos de simplicidade do porte.

---

## 8. Layouts: geometria e empilhamento exatos

### 8.1 `arrange()` → `arrangemon()` (dwm.c:381-401) — o orquestrador

```c
void arrange(Monitor *m) {
    if (m) showhide(m->stack);                 // mostra/esconde por tag (recursivo)
    else for todos m: showhide(m->stack);
    if (m) { arrangemon(m); restack(m); }      // aplica layout + reordena Z
    else for todos m: arrangemon(m);
}
void arrangemon(Monitor *m) {
    strncpy(m->ltsymbol, m->lt[m->sellt]->symbol, ...);  // atualiza o símbolo pra barra
    if (m->lt[m->sellt]->arrange) m->lt[m->sellt]->arrange(m);  // chama tile()/monocle()/NULL
}
```

O layout é um **ponteiro de função** (`Layout.arrange`, dwm.c:108-111). `NULL` = floating (nada
a fazer). Trocar layout = trocar o ponteiro (`setlayout`, dwm.c:1510-1522).

### 8.2 `showhide()` (dwm.c:1629-1645) — visibilidade por tag, sem unmap

```c
void showhide(Client *c) {
    if (!c) return;
    if (ISVISIBLE(c)) {                        // visível: move pra posição e desce recursão
        XMoveWindow(dpy, c->win, c->x, c->y);
        if ((floating) && !fullscreen) resize(c, c->x,c->y,c->w,c->h, 0);
        showhide(c->snext);                    // top-down
    } else {                                    // invisível: desce PRIMEIRO, depois esconde
        showhide(c->snext);                    // bottom-up
        XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y);  // ★ esconde movendo pra FORA da tela
    }
}
```

Detalhe genial: dwm **não faz unmap** para esconder janela de outra tag — move ela pra
`x = -2*largura` (fora da tela à esquerda). Isso evita a tempestade de `UnmapNotify`/`MapNotify`
(que disparariam `unmanage`) e é instantâneo. A recursão top-down (visíveis) vs bottom-up
(escondidas) mantém a ordem Z coerente.

**→ ntwm:** trivial. No dispd, "esconder" = não incluir a janela no conjunto `PLACE`d do `ws`
atual (ou mandar `ws` diferente); `compose_and_present` filtra por `w->ws == cur_ws`
(compositor.c:208). **Não há truque de mover pra fora da tela** — o compositor só não desenha.
Muito mais limpo.

### 8.3 `tile()` (dwm.c:1687-1713) — o layout mestre-pilha

```c
for (n=0, c=nexttiled(m->clients); c; c=nexttiled(c->next), n++);  // conta tiled visíveis
if (n == 0) return;
if (n > m->nmaster) mw = m->nmaster ? m->ww * m->mfact : 0;   // largura do master
else mw = m->ww;                                              // só masters: ocupam tudo
for (i=my=ty=0, c=nexttiled(m->clients); c; c=nexttiled(c->next), i++)
    if (i < m->nmaster) {                        // coluna MASTER (esquerda)
        h = (m->wh - my) / (MIN(n, m->nmaster) - i);          // divide altura restante
        resize(c, m->wx, m->wy + my, mw - 2*c->bw, h - 2*c->bw, 0);
        if (my + HEIGHT(c) < m->wh) my += HEIGHT(c);
    } else {                                      // coluna STACK (direita)
        h = (m->wh - ty) / (n - i);
        resize(c, m->wx + mw, m->wy + ty, m->ww - mw - 2*c->bw, h - 2*c->bw, 0);
        if (ty + HEIGHT(c) < m->wh) ty += HEIGHT(c);
    }
```

Geometria: a área útil (`wx,wy,ww,wh`) vira duas colunas. `nmaster` janelas na esquerda
(largura `ww*mfact`), o resto empilhado na direita (largura `ww*(1-mfact)`). Dentro de cada
coluna, altura dividida igualmente entre as que faltam (o `(m->wh - my) / (MIN(n,nmaster) - i)`
é uma divisão incremental que absorve o resto da divisão inteira sem sobra de pixels). `nexttiled`
(dwm.c:1205-1210) pula flutuantes e invisíveis. `2*c->bw` desconta a borda.

**→ ntwm:** **porta 1:1.** É aritmética pura sobre `wx,wy,ww,wh`. A única troca: cada `resize(c,
x,y,w,h,0)` vira um `PLACE <id> x y (w+2bw) (h+2bw) ws z` (lembrando que o `PLACE` do protocolo
passa o rect **com** borda — o dispd subtrai `border_px*2` pra achar a área cliente, wmproto.c:180-181).
Compare com o `builtin_layout()` do dispd (dispd.c, embrião de tiler vertical): o `tile()` do
dwm é o alvo "adulto" que o ntwm deve implementar (mestre+pilha, nmaster, mfact).

### 8.4 `monocle()` (dwm.c:1113-1126)

Todas as janelas visíveis ocupam a área inteira (`m->wx, m->wy, m->ww, m->wh`), sobrepostas; a
de cima é a focada. O símbolo vira `[N]` (contagem). **→ ntwm:** trivial — `PLACE` de todas no
mesmo rect, `z` crescente, foco define quem aparece por cima. Como o dispd compõe por z
(compositor.c:210-216), a monocle "de graça".

### 8.5 `resize()` / `resizeclient()` / `applysizehints()` (dwm.c:1278-1298, 313-379)

```c
void resize(Client *c, int x,int y,int w,int h, int interact) {
    if (applysizehints(c, &x,&y,&w,&h, interact))   // ajusta w/h por hints; retorna se mudou
        resizeclient(c, x,y,w,h);
}
void resizeclient(Client *c, int x,int y,int w,int h) {
    c->oldx=c->x; c->x=wc.x=x;  ... (idem y,w,h)
    wc.border_width = c->bw;
    XConfigureWindow(dpy, c->win, CWX|CWY|CWWidth|CWHeight|CWBorderWidth, &wc);  // ★ empurra geom
    configure(c);                                   // ConfigureNotify sintético
    XSync(dpy, False);
}
```

`applysizehints()` (dwm.c:313-379) é o pedaço ICCCM: respeita `minw/minh/maxw/maxh`, incrementos
(`incw/inch` — terminais que só redimensionam em múltiplos de célula), aspect ratio
(`mina/maxa`), e as regras de base/min (o comentário "ICCCM 4.1.2.3", dwm.c:348). Só aplica hints
se `resizehints` (config, on), ou se a janela é flutuante, ou se não há layout de tiling
(dwm.c:345). Em tiling puro, hints são ignorados (janelas preenchem a célula).

**→ ntwm:** `resizeclient` = **`PLACE`** (é a única "empurra geometria"). `applysizehints` porta
como função pura (é matemática), *se* o ntwm quiser honrar hints de apps — mas como o dispd é
dono da surface e terminais redimensionam por célula no `term_resize` (compositor.c:149-150), boa
parte disso pode ficar no dispd. O `configure()` + `XSync` somem.

### 8.6 `restack()` (dwm.c:1357-1380) — ordem Z real

```c
drawbar(m);
if (!m->sel) return;
if (m->sel->isfloating || !arrange) XRaiseWindow(dpy, m->sel->win);  // focado floating vai ao topo
if (arrange) {                                    // no tiling, empilha pela stack list
    wc.stack_mode = Below; wc.sibling = m->barwin;
    for (c = m->stack; c; c = c->snext)
        if (!c->isfloating && ISVISIBLE(c)) {
            XConfigureWindow(dpy, c->win, CWSibling|CWStackMode, &wc);  // c fica ABAIXO do anterior
            wc.sibling = c->win;
        }
}
XSync(dpy, False);
while (XCheckMaskEvent(dpy, EnterWindowMask, &ev));  // ★ descarta EnterNotify gerados pelo restack
```

O `XConfigureWindow(CWSibling|CWStackMode=Below)` encadeia as janelas na ordem da `stack` list,
todas abaixo da barra. O `while(XCheckMaskEvent(EnterWindowMask))` no fim **descarta** os
`EnterNotify` que o próprio reempilhamento gera (senão o foco pularia pra janela que o cursor
"cruzou" durante o restack). Esse é um patch de corrida clássico do X.

**→ ntwm:** ordem Z = campo `z` do `PLACE`. O ntwm atribui `z` crescente percorrendo a stack
list e manda no frame. O dispd ordena por z ao compor (compositor.c:210-216). **O
`XCheckMaskEvent(EnterWindowMask)` desaparece** — não há EnterNotify espúrio porque o dispd não
gera enter/leave a partir de recomposição; o modelo atômico não tem essa corrida.

---

## 9. Foco e entrada

### 9.1 `focus()` (dwm.c:788-811) — o núcleo

```c
if (!c || !ISVISIBLE(c))                          // sem alvo? pega o topo da stack visível
    for (c=selmon->stack; c && !ISVISIBLE(c); c=c->snext);
if (selmon->sel && selmon->sel != c) unfocus(selmon->sel, 0);  // desfoca o anterior
if (c) {
    if (c->mon != selmon) selmon = c->mon;         // segue o monitor
    if (c->isurgent) seturgent(c, 0);              // limpa urgência
    detachstack(c); attachstack(c);                // ★ traz ao topo da stack list
    grabbuttons(c, 1);                             // grabs de "janela focada"
    XSetWindowBorder(dpy, c->win, scheme[SchemeSel][ColBorder].pixel);  // borda de foco
    setfocus(c);                                   // dá o input focus X
} else {                                            // nada pra focar: foco no root
    XSetInputFocus(dpy, root, RevertToPointerRoot, CurrentTime);
    XDeleteProperty(dpy, root, netatom[NetActiveWindow]);
}
selmon->sel = c;
drawbars();
```

### 9.2 `setfocus()` (dwm.c:1472-1480) — o input focus de verdade

```c
if (!c->neverfocus) XSetInputFocus(dpy, c->win, RevertToPointerRoot, CurrentTime);  // teclado -> c
XChangeProperty(dpy, root, _NET_ACTIVE_WINDOW, XA_WINDOW, 32, ..., &c->win, 1);     // EWMH
sendevent(c, wmatom[WMTakeFocus]);                                                  // WM_TAKE_FOCUS
```

`XSetInputFocus` é o que faz o teclado ir pra janela `c`. `neverfocus` (de `WM_HINTS` input flag)
respeita apps que gerenciam o próprio foco. `WM_TAKE_FOCUS` avisa apps ICCCM educados.

### 9.3 `unfocus()` (dwm.c:1765-1776)

Tira grabs de foco, pinta borda normal (`SchemeNorm`), e opcionalmente devolve o input focus ao
root.

**→ ntwm:** `focus()` porta quase 1:1, **mas as 3 chamadas Xlib viram 2 verbos:**
`XSetWindowBorder(SchemeSel)` = **`BORDER <id> px <rrggbb-foco>`**; `XSetInputFocus`+
`_NET_ACTIVE_WINDOW`+`WM_TAKE_FOCUS` = **`FOCUS <id>`** (o dispd faz o `win_focus` interno,
compositor.c:154-162, que marca `focused` e o input vai pro pty dessa surface via `to_term`,
input.c:29-31). `detachstack/attachstack`, `neverfocus`, `seturgent` são estado interno do ntwm.
`drawbars` = ntwm redesenha a barra e a envia (§10).

### 9.4 `grabkeys()` (dwm.c:952-978) e o problema NumLock/CapsLock

```c
XUngrabKey(dpy, AnyKey, AnyModifier, root);              // limpa grabs antigos
XDisplayKeycodes(dpy, &start, &end);
syms = XGetKeyboardMapping(dpy, start, end-start+1, &skip);
for (k=start; k<=end; k++)                               // p/ cada keycode físico
  for cada keys[i]                                       // p/ cada bind configurado
    if (keys[i].keysym == syms[(k-start)*skip])          // o keysym bate?
      for cada mod em { 0, LockMask, numlockmask, numlockmask|LockMask }:  // ★ 4 variantes
        XGrabKey(dpy, k, keys[i].mod | mod, root, True, GrabModeAsync, GrabModeAsync);
```

O X entrega o estado de NumLock (Mod2) e CapsLock (Lock) **junto** com os modificadores. Se você
só registrar `Mod1+p`, o atalho **não dispara** com CapsLock ligado (a máscara seria
`Mod1+Lock+p`). Solução de dwm: registrar **as 4 combinações** (com/sem Lock, com/sem
numlockmask). `updatenumlockmask()` (dwm.c:1945-1959) descobre qual bit é o NumLock consultando
`XGetModifierMapping`. Espelhado no `CLEANMASK` (dwm.c:49), que remove esses bits ao comparar em
`keypress`/`buttonpress`.

**→ ntwm:** **desnecessário.** O dispd lê modificadores com `GetKeyState(VK_MENU/CONTROL/SHIFT/
LWIN)` (input.c:16-25), que já ignora NumLock/CapsLock por construção. Então o ntwm registra 1
`GRAB <mods> <vk>` por bind (não 4), e compara direto sem CLEANMASK. **Um problema inteiro do X
que some no NT.**

### 9.5 `grabbuttons()` (dwm.c:931-950)

Registra os botões do mouse relevantes na janela (Mod+Button1 move, etc.), também nas 4 variantes
de Lock. Quando não-focada, faz um grab "sync" de qualquer botão pra implementar click-to-focus
(o clique é capturado, a janela é focada, e `XAllowEvents(ReplayPointer)` reenvia o clique).

**→ ntwm:** viraria `GRAB` de botão (o protocolo hoje só tem `GRAB` de tecla; estender pra
`GRAB-BUTTON <mods> <btn>` ou tratar botões no dispd, que já sabe a surface sob o cursor).

---

## 10. A barra e o `drw` (dwm como aula de desenho X)

dwm desenha a barra **off-screen** e faz um blit pra tela — o padrão double-buffer clássico do X:

- `drw_create` (drw.c): `XCreatePixmap(root, w, h, depth)` cria um **pixmap** (buffer off-screen
  no server) + `XCreateGC` (graphics context: cor, fonte, line style).
- `drawbar()` (dwm.c:697-748): desenha tags, símbolo de layout, título e status text **no
  pixmap**, via `drw_text`/`drw_rect`:
  - `drw_rect` (drw.c) = `XFillRectangle`/`XDrawRectangle`.
  - `drw_text` (drw.c) = `XftDrawStringUtf8` (texto UTF-8 antialiased via Xft/FreeType), com
    fallback de fonte por codepoint (`XftCharExists`, `FcCharSet`) e elipse "…" no overflow.
  - As cores vêm dos `scheme[SchemeNorm/SchemeSel]` (fg/bg/border, config.h:15-19).
- `drw_map` (drw.c) = **`XCopyArea(drawable, barwin, gc, ...)`** — copia o pixmap pronto pra
  janela real da barra de uma vez (o "present" da barra). `XSync` depois.

A barra é uma janela `override_redirect` (dwm.c:1823) — o próprio dwm não a gerencia como
cliente. `updatebars()` (dwm.c:1818-1838) cria uma por monitor.

**→ ntwm:** o modelo é **o mesmo** e cai como uma luva no dispd: o "pixmap off-screen" = um DIB;
`XCopyArea` pro barwin = `BitBlt` pro DIB da surface; `XftDrawStringUtf8` = `TextOutA`/`DrawText`
GDI (o dispd já usa `OEM_FIXED_FONT` + `TextOutA`, compositor.c:59-66, 120-122). **Decisão de
arquitetura:** a barra pode ser (a) uma surface especial que o **ntwm** desenha e envia via um
verbo novo (`BARBLIT`/DIB compartilhado), ou (b) o dispd desenha a barra a partir de dados que o
ntwm manda (tags ativas, título, layout symbol, status). Dado que o dispd já tem fonte+célula+
compositor, o caminho mais barato é **(b)**: o ntwm manda o *conteúdo* da barra (quais tags,
qual símbolo, título do focado, status text) e o dispd rasteriza. Isso mantém o ntwm livre de
GDI. O `drw` inteiro de dwm vira, no NTUnix, código do lado do dispd.

O `TEXTW(X)` (dwm.c:57) = largura do texto + padding, usado pra hit-testing dos cliques na barra
(buttonpress, dwm.c:433-446). No ntwm, o hit-testing precisa das larguras de texto — que só o
dispd sabe (é quem tem a fonte). Então ou o dispd reporta larguras, ou o dispd faz o hit-testing
da barra e manda um evento semântico (`BAR-CLICK tag N` / `BAR-CLICK layout`). O caminho (b)
naturalmente empurra o hit-testing pro dispd também.

---

## 11. EWMH e átomos — o "protocolo social" do X

dwm implementa um subconjunto mínimo de EWMH (dwm.c:62-64, setup:1568-1604):
- `_NET_SUPPORTED` — anuncia o que suporta.
- `_NET_SUPPORTING_WM_CHECK` + `_NET_WM_NAME="dwm"` — "há um WM compliant" (pra barras/pagers).
- `_NET_ACTIVE_WINDOW` — qual janela está focada (setfocus:1477).
- `_NET_CLIENT_LIST` — lista de janelas gerenciadas (manage:1079, updateclientlist:1853).
- `_NET_WM_STATE` + `_NET_WM_STATE_FULLSCREEN` — fullscreen (clientmessage/setfullscreen).
- `_NET_WM_WINDOW_TYPE` + `_..._DIALOG` — dialogs viram flutuantes (updatewindowtype:2022).
- `_NET_WM_NAME` (UTF-8) — título (updatetitle:2014).
- ICCCM: `WM_PROTOCOLS`/`WM_DELETE_WINDOW` (fechar educado, sendevent:1447),
  `WM_TAKE_FOCUS`, `WM_STATE` (Normal/Iconic/Withdrawn, setclientstate:1438),
  `WM_NORMAL_HINTS` (size hints), `WM_HINTS` (urgência/input), `WM_TRANSIENT_FOR` (dialogs),
  `WM_CLASS` (rules, applyrules:289).

Esses "átomos" existem porque no X **vários processos independentes** (WM, barra externa, app,
pager) precisam concordar num vocabulário. É um protocolo de interoperabilidade entre
desconhecidos.

**→ ntwm:** **quase tudo isso desaparece**, e é uma percepção importante: EWMH/ICCCM é a cola de
um ecossistema *aberto* de processos X que não se conhecem. No NTUnix, o dispd e o ntwm são um
par *fechado* que fala um protocolo próprio (`ntuwm.h`). Não há barra externa nem pager de
terceiros pra satisfazer. Então:
- `_NET_ACTIVE_WINDOW`/`_NET_CLIENT_LIST` = estado interno do ntwm (não precisa publicar).
- `WM_DELETE_WINDOW`/`XKillClient` = **`CLOSE <wid>`** (dispd decide como fechar o pty/processo).
- `WM_NORMAL_HINTS`/aspect = fica no dispd (terminal por célula) ou vira campo de evento futuro.
- `WM_TRANSIENT_FOR`/`_NET_WM_WINDOW_TYPE_DIALOG` = campo `kind` no `WINDOW-CREATED` (dispd
  poderia classificar app vs dialog).
- Título UTF-8 = **`WINDOW-TITLE`** (já existe).

O ntwm só reimplementa a parte de EWMH que for *funcionalidade* (fullscreen, dialog-flutua), e
**nada** da parte que é *anúncio para terceiros*.

---

## 12. Ausência de IPC — config em tempo de compilação (dwm vs i3)

dwm **não tem** socket de controle, nem arquivo de config em runtime, nem linguagem de config.
Toda a configuração é `config.h` compilado (dwm.c:271). Reconfigurar = editar C + `make` +
reiniciar (há o patch `restart` que faz `execvp` de si mesmo preservando as janelas via scan).
Comunicar com dwm de fora = ou `xdotool`/`wmctrl` (via EWMH/eventos X), ou o patch
`dwm-msg`/`fifo`. Filosofia suckless: **o código-fonte é a interface de configuração**; menos de
2500 linhas cabem na cabeça; sem parser de config, sem estado de runtime pra bug.

Contraste **i3**: tem `~/.config/i3/config` (parser próprio), um **socket IPC** (`i3-msg`,
JSON sobre unix socket) que permite scriptar/consultar o WM em runtime (`i3-msg workspace 2`,
barras como `i3bar`/`polybar` consultam layout via IPC), e `i3-nagbar`. i3 é ~30k linhas. A
troca: i3 é reconfigurável a quente e scriptável; dwm é minúsculo e auditável.

**→ ntwm:** interessante — o NTUnix **já tem** o que dwm não tem: um **IPC de verdade** (o pipe
`NTU_PIPE_DISPD`). Mas repare na **direção**: no i3, o IPC é *cliente externo → WM* (controle).
No NTUnix, o pipe é *ntwm → dispd* (o WM controla o compositor). São eixos diferentes. Se o
NTUnix quiser um `ntctl`-para-WM (scriptar tags/layout de fora, tipo `i3-msg`), isso seria um
**segundo** canal (ntctl → ntwm), não o pipe do dispd. Recomendação: o ntwm pode manter a
filosofia dwm (keybinds compilados) **e** expor um pipe de controle opcional depois — o melhor
dos dois, já que a infraestrutura de named pipe do NTUnix (wmproto.c) é reusável. Para o v1,
copie dwm: `keys[]`/`rules[]`/`layouts[]` como arrays C no ntwm, sem runtime config.

---

## 13. Mapa de porte para o `ntwm` — função por função

Legenda: **[POLÍTICA]** = porta pro ntwm quase 1:1 (é o cérebro). **[MECANISMO→dispd]** =
já existe/pertence ao dispd. **[SOME]** = artefato do X que não tem análogo (o modelo atômico o
elimina).

### 13.1 Ciclo de vida

| dwm (linha) | vira, no ntwm | notas |
|---|---|---|
| `main()` 2143 | `main()` do ntwm | `XOpenDisplay`→`CreateFile(NTU_PIPE_DISPD)`+`HELLO ntwm 1` |
| `checkotherwm()` 459 | **[SOME]** | o pipe já garante 1 cliente |
| `setup()` 1539 | `setup()` do ntwm | ver decomposição abaixo |
| `setup:1606-1611` (SubstructureRedirect) | mandar `HELLO`, receber `WELCOME`/`OUTPUT` | **é o "virar WM"** |
| `setup:1568-1604` (átomos/EWMH) | **[SOME]** | sem clientes X observando |
| `setup:1612` grabkeys | enviar um `GRAB <mods> <vk>` por bind | ver 13.5 |
| `scan()` 1393 | consumir snapshot `WINDOW…` até `SYNC` | wmproto.c:143 |
| `run()` 1382 | loop `ReadFile` do pipe → dispatch por verbo | ver 13.2 |
| `cleanup()` 470 | `UNGRAB` de tudo + fechar pipe (ou `QUIT`) | dispd sobrevive c/ builtin_layout |

### 13.2 Handlers de evento (o `handler[]` → dispatch por verbo)

| dwm handler (linha) | evento X | evento `ntuwm.h` | ação no ntwm |
|---|---|---|---|
| `maprequest`/`manage` 1101/1031 | MapRequest | **`WINDOW-CREATED`** | criar Client, applyrules, arrange |
| `destroynotify`/`unmanage` 649/1778 | DestroyNotify | **`WINDOW-DESTROYED`** | remover Client, arrange |
| `unmapnotify` 1804 | UnmapNotify | (colapsa em DESTROYED) | — |
| `propertynotify` 1221 (título) | PropertyNotify | **`WINDOW-TITLE`** | updatetitle + drawbar |
| `propertynotify` (hints/urgência) | PropertyNotify | (evento futuro) | opcional |
| `keypress` 999 | KeyPress | **`KEY <mods> <vk>`** | lookup em `keys[]`, chamar func |
| `buttonpress` 417 | ButtonPress | (evento `BUTTON` futuro) | hit-test barra / focus |
| `enternotify` 759 | EnterNotify | (evento `POINTER-ENTER` futuro) | focus-follows-mouse |
| `configurenotify` 552 (root) | ConfigureNotify | **`OUTPUT`** (re-emitido) | updategeom + arrange |
| `configurerequest` 580 | ConfigureRequest | **[SOME]** (ou evento futuro) | dispd é dono da geom |
| `expose` 778 | Expose | **[SOME]** | compositor é retido |
| `focusin` 814 | FocusIn | **[SOME]** | ninguém rouba foco |
| `mappingnotify` 1091 | MappingNotify | **[SOME]** | dispd usa VKs |
| `clientmessage` 514 | ClientMessage | (evento futuro) | fullscreen/urgência |
| `motionnotify` 1128 | MotionNotify | (evento `POINTER` futuro) | troca de monitor |

### 13.3 Modelo de dados

| dwm | ntwm |
|---|---|
| `struct Client{...,Window win}` (86) | `struct Client{..., unsigned id}` — `id`=`wid` do protocolo |
| `struct Monitor{...}` (113) | igual; retângulos vêm de `OUTPUT` |
| `clients`/`stack` (next/snext) | igual (duas listas: tiling + foco/Z) |
| `tags`/`tagset[2]`/`ISVISIBLE` (52) | igual internamente; projeta pro campo `<ws>` do `PLACE` |
| `wintoclient()` 2065 | `idtoclient()` — busca por `id` |
| `updategeom()` 1867 | monta Monitors a partir dos `OUTPUT` (sem Xinerama/dedup) |

### 13.4 Layout / geometria (o coração transferível)

| dwm (linha) | ntwm | comando dispd |
|---|---|---|
| `arrange()` 381 | `arrange()` | abre `FRAME-BEGIN`, emite tudo, fecha `FRAME-COMMIT` |
| `arrangemon()` 395 | igual | atualiza ltsymbol da barra |
| `tile()` 1687 | **porta 1:1** (mestre+pilha, nmaster, mfact) | cada resize → `PLACE` |
| `monocle()` 1113 | porta 1:1 | `PLACE` sobreposto + `z` |
| `showhide()` 1629 | filtra por tag/ws | não-`PLACE` das escondidas (sem truque -2*W) |
| `restack()` 1357 | atribui `z` pela stack list | `z` no `PLACE` (sem XCheckMaskEvent) |
| `resizeclient()` 1285 | **`PLACE <id> x y (w+2bw) (h+2bw) ws z`** | wmproto:171 |
| `applysizehints()` 313 | porta como função pura (se honrar hints) | ou deixa no dispd |
| `resize()` 1278 | chama applysizehints + PLACE | — |
| `nexttiled()` 1205 | igual (pula floating/invisível) | — |
| `configure()` 533 (ConfigureNotify sintético) | **[SOME]** | COMMIT é a notificação |

### 13.5 Foco e entrada

| dwm (linha) | ntwm | comando dispd |
|---|---|---|
| `focus()` 788 | porta 1:1 (detachstack/attachstack/borda/foco) | `BORDER`(foco) + `FOCUS` |
| `setfocus()` 1472 | vira **`FOCUS <id>`** | wmproto:183 → `win_focus` (compositor:154) |
| `unfocus()` 1765 | borda normal | `BORDER <id> px <normal>` |
| `focusstack()` 837 | porta 1:1 (Mod+j/k) | resulta em `FOCUS` |
| `grabkeys()` 952 | 1× `GRAB` por bind (sem 4 variantes de Lock) | `GRAB` (wmproto:203) |
| `updatenumlockmask()` 1945 / `CLEANMASK` 49 | **[SOME]** | dispd ignora Num/CapsLock (input.c:16) |
| `grabbuttons()` 931 | `GRAB`-de-botão (protocolo a estender) | — |
| `keypress()` 999 | lookup `(mods,vk)` em `keys[]` | recebe `KEY` |
| `killclient()` 1015 | vira **`CLOSE <id>`** (sem XGrabServer/XKillClient) | wmproto:196 |
| `spawn()` 1647 | **`SPAWN-TERM [cmd]`** (ou CreateProcess) | wmproto:194 |

### 13.6 Ações de config (portam 1:1, são pura política)

`view`/`toggleview`/`tag`/`toggletag` (tags), `setlayout`/`setmfact`/`incnmaster` (layout),
`togglefloating`/`togglebar`/`zoom`/`pop` (2054/1753/1669/1739/1510/1525/980/1724/1715/2131/1212)
— **todas** são manipulação de estado do ntwm seguida de `arrange()`. Nenhuma toca Xlib
diretamente além do `arrange`. Copie o corpo, troque o `arrange` final pela emissão de frame.

### 13.7 Barra

| dwm | ntwm/dispd |
|---|---|
| `drw` inteiro (drw.c), `drawbar()` 697, `TEXTW` 57 | **fica no dispd** (tem fonte/GDI) |
| conteúdo (tags ativas, ltsymbol, título, status) | ntwm envia; dispd rasteriza (caminho **b** §10) |
| `buttonpress` hit-test barra 433 | dispd faz hit-test e manda evento semântico, ou reporta larguras |
| `updatestatus()` 2005 (xsetroot name) | ntwm lê status de onde quiser e envia |

### 13.8 O que **desaparece** no porte (dívida do X que o modelo atômico paga)

1. `XSync`/`XCheckMaskEvent(EnterWindowMask)` espalhados — o pipe é ordenado, o COMMIT é barreira.
2. `XGrabServer`/`XUngrabServer` (killclient 1021, unmanage 1788) — sem server compartilhado.
3. `xerror`/`xerrordummy`/`xerrorstart` (2098-2129) — sem erros assíncronos de "janela morreu".
4. `c->x + 2*sw` (manage 1081) e o showhide `-2*WIDTH` (1643) — sem flash; o compositor só
   desenha onde mandado.
5. `configure()` ConfigureNotify sintético (533) — o COMMIT já notifica.
6. NumLock/CapsLock 4-variantes + CLEANMASK — dispd normaliza modificadores.
7. EWMH/ICCCM de *anúncio* (`_NET_SUPPORTED`, `_NET_SUPPORTING_WM_CHECK`, `_NET_CLIENT_LIST`) —
   par fechado, sem terceiros.
8. `override_redirect` filtering — dispd não cria surface top-level pra menu de ninguém.

Some ~30-40% do volume de `dwm.c` (todo o "combate ao X"). O que sobra é o **cérebro**: modelo
de tags/monitores, listas clients/stack, tile/monocle, foco, keybinds — e isso porta limpo.

### 13.9 O que o dispd **já** tem (embriões a "crescer" para o padrão dwm)

- `builtin_layout()` (dispd.c) — tiler vertical ingênuo → alvo: `tile()` de dwm (mestre+pilha).
- `win_focus()` (compositor.c:154) — foco simples → recebe `FOCUS` do ntwm.
- `win_at_point()` (compositor.c:164) — hit-test por z → base pra focus-follows-mouse/cliques.
- input.c binds embutidos (Alt+Enter/Tab/Q) — fallback quando **não** há ntwm; o ntwm os
  substitui via `GRAB`+`KEY` (input.c:98-104 dá prioridade ao grab do ntwm).
- `compose_and_present()` (compositor.c:196) — o compositor retido (equivalente a
  restack+showhide+expose de dwm, tudo de uma vez por tick).

---

## 14. Sequências completas (para copiar o fluxo)

**Nasce uma janela (Mod+Shift+Enter → terminal):**
- dwm: `spawn`→fork/exec `st` → `st` faz `XMapWindow` → server manda `MapRequest` → `maprequest`
  → `manage` (attach, geometria, map real, arrange) → `focus`.
- ntwm: `keypress`(`KEY`) → func `spawn` → **`SPAWN-TERM`** → dispd cria surface, manda
  **`WINDOW-CREATED`** → handler cria Client → `arrange` (**`FRAME-BEGIN`/`PLACE…`/`FRAME-COMMIT`**)
  → `focus` (**`BORDER`**+**`FOCUS`**).

**Troca de tag (Mod+2):**
- dwm: `keypress`→`view`→ muda `tagset`→`focus(NULL)`→`arrange` (showhide move escondidas pra
  -2*W, tile reposiciona, restack reordena Z).
- ntwm: `KEY`→`view`→ muda tagset interno→`arrange`: emite `FRAME-BEGIN`, `PLACE` só das
  visíveis (com `ws=cur_ws`), `FOCUS` na nova selecionada, `FRAME-COMMIT`. dispd troca o quadro
  atomicamente.

**Fecha janela (Mod+Shift+C):**
- dwm: `keypress`→`killclient`→ `sendevent(WM_DELETE)` ou `XKillClient` (com XGrabServer) → app
  morre → `DestroyNotify` → `unmanage` → `arrange`.
- ntwm: `KEY`→`killclient`→ **`CLOSE <id>`** → dispd fecha pty/processo, manda
  **`WINDOW-DESTROYED`** → `unmanage` (só limpa Client) → `arrange`.

---

## 15. Referências de linha (índice rápido do dwm.c 6.8)

- Macros/ISVISIBLE/CLEANMASK/TAGMASK: 47-57. Enums (Cur/Scheme/Net/WM/Clk): 59-67.
- Structs Client/Monitor/Rule/Layout/Key/Button: 69-141. `handler[]`: 245-260.
- Lifecycle: `main` 2143, `checkotherwm` 459, `setup` 1539, `scan` 1393, `run` 1382,
  `cleanup` 470.
- Adoção: `manage` 1031, `unmanage` 1778, `applyrules` 277.
- Eventos: `maprequest` 1101, `configurerequest` 580, `configurenotify` 552, `destroynotify`
  649, `unmapnotify` 1804, `enternotify` 759, `expose` 778, `focusin` 814, `keypress` 999,
  `buttonpress` 417, `propertynotify` 1221, `clientmessage` 514, `mappingnotify` 1091,
  `motionnotify` 1128.
- Layout: `arrange` 381, `arrangemon` 395, `showhide` 1629, `restack` 1357, `tile` 1687,
  `monocle` 1113, `resize` 1278, `resizeclient` 1285, `applysizehints` 313, `nexttiled` 1205.
- Foco/entrada: `focus` 788, `setfocus` 1472, `unfocus` 1765, `focusstack` 837, `grabkeys` 952,
  `grabbuttons` 931, `updatenumlockmask` 1945, `killclient` 1015, `spawn` 1647.
- Tags/config: `view` 2054, `toggleview` 1753, `tag` 1669, `toggletag` 1738, `setlayout` 1510,
  `setmfact` 1525, `incnmaster` 980, `togglefloating` 1724, `togglebar` 1715, `zoom` 2131,
  `pop` 1212.
- Barra/drw: `drawbar` 697, `drawbars` 750, `updatebars` 1818, `updatebarpos` 1840,
  `updatestatus` 2005; `drw_map`=`XCopyArea`, `drw_text`=`XftDrawStringUtf8`, `drw_rect`=
  `XFillRectangle` (drw.c).
- EWMH/ICCCM: átomos 62-65 + setup 1568-1604; `sendevent` 1447, `setclientstate` 1438,
  `setfullscreen` 1482, `updatewindowtype` 2022, `updatewmhints` 2035, `updatesizehints` 1961,
  `updatetitle` 2014, `getatomprop` 863, `gettextprop` 908.
- Xlib error tolerance: `xerror` 2098. Config: `config.def.h` (keys 64-99, buttons 103-116,
  rules 24-32, layouts 41-46, tags 22).
