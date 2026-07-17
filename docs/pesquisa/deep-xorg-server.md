# X.Org Server (xserver) — análise profunda das entranhas, como fundação para o `dispd`

> Leitura direta do código-fonte do **xserver** (branch master, commit `1133421bbd4a`, 2026-07-15, `version: 21.1.99.1`),
> clonado de `gitlab.freedesktop.org/xorg/xserver`. Todas as citações são `arquivo:linha` / nome de função reais.
> Objetivo: entender **por que** um display server maduro consegue ter *window manager* e *compositor* como
> clientes separados e trocáveis — a propriedade exata que quero replicar em `dispd` + `ntwm`.
>
> Complementa `docs/pesquisa/nt-dwm-compositor.md` (o lado NT/WDDM) e a `src/dispd/dispd.h`.
> Onde o X faz algo que o `dispd` já faz por outro caminho, eu marco **[dispd]**.

---

## 0. Resumo em uma tela (o que roubar do X)

O X.Org acerta uma coisa acima de todas: **o protocolo é o único contrato**. O servidor não sabe o que é um "window
manager" nem o que é um "compositor". Ele só sabe: (a) rotear requisições para funções, (b) rotear eventos para clientes
segundo *máscaras de interesse*, e (c) gerenciar recursos com dono. WM e compositor **emergem** de três primitivas do
protocolo, nenhuma delas com código especial:

1. **Substructure redirection** (`SubstructureRedirectMask`) — um cliente diz "intercepte map/configure/circulate dos
   filhos desta janela e me mande como *evento* em vez de executar". Isso, aplicado à janela raiz, **é** o window manager.
   O servidor garante por construção que **no máximo um** cliente pode segurar essa máscara por janela (`AtMostOneClient`).
2. **Event masks + propagação na árvore** — eventos sobem de filho para pai e são entregues a *quem declarou interesse*.
   O input vai para o cliente certo sem o servidor saber nada sobre políticas de foco de um WM.
3. **Redirect de desenho para pixmap offscreen** (extensão Composite, `redirectDraw`) — uma janela passa a desenhar num
   *pixmap privado* em vez de no framebuffer compartilhado. Um cliente lê esses pixmaps e compõe. Isso **é** o compositor.

Essas três coisas são **ortogonais** e **plugáveis por qualquer cliente**. É por isso que você troca `i3` por `mutter`
sem reiniciar o servidor, e roda `picom` (compositor) como um terceiro processo independente do WM. O `dispd` já nasce
com o item (3) embutido (cada janela tem seu DIB próprio — `src/dispd/dispd.h`), então a lição maior é como desenhar o
**protocolo** dos itens (1) e (2) para que `ntwm` seja um cliente trocável de verdade, e não um módulo linkado no `dispd`.

---

## 1. Arquitetura e camadas

O xserver é dividido em camadas com uma regra de ouro: **DIX nunca toca hardware; DDX nunca implementa política de
protocolo**. A ligação entre elas é feita por **vtables de ponteiros de função** penduradas em três structs-chave
(`ScreenRec`, `GCRec`, `DrawableRec`).

| Camada | Diretório | O que possui |
|---|---|---|
| **DIX** (Device Independent X) | `dix/` | Semântica do protocolo X11 core: dispatch, recursos/XIDs, árvore de janelas, entrega de eventos, foco, grabs, GCs, propriedades, seleções, átomos. É *portável* — não sabe o que é uma GPU. |
| **OS layer** | `os/` | I/O de rede/sockets, buffers de requisição/resposta, o *event loop* (`WaitForSomething`), timers, autenticação, poll/epoll (`ospoll`), thread de input. Isola o SO. |
| **mi** (machine independent) | `mi/` | Implementações *de referência* em software das operações que a DDX poderia acelerar: cálculo de clipping (`miComputeClips`), rasterização de arcos/linhas/polígonos, cursor de software (`misprite`), fila de eventos de input (`mieq`). "DDX genérica em C puro." |
| **fb** | `fb/` | O rasterizador de framebuffer real (o "fbdev" moderno) — preenche pixels em memória. É a `GCOps` concreta padrão. |
| **DDX** (Device Dependent X) | `hw/xfree86/`, `hw/xwayland/`, `hw/kdrive/`… | O "backend". No Linux moderno é `hw/xfree86/` + o driver `modesetting` (`hw/xfree86/drivers/modesetting/`), que fala KMS/DRM, faz o *page flip* para o scanout e implementa a extensão Present. |
| **Extensões** | `render/`, `composite/`, `damageext/`, `randr/`, `xfixes/`, `present/`, `Xext/`, `Xi/`… | Cada uma registra opcodes/eventos/erros próprios e *envelopa* (wrappea) funções do Screen. Composite e Damage são a base do compositing. |

O `main()` (`dix/main.c`) roda um laço externo de "gerações": inicializa OS, chama `InitOutput()` (entra na DDX, cria as
telas), `InitInput()`, `Dispatch()`, e quando o `Dispatch` retorna (reset), reinicializa tudo. Cada tela vira um
`ScreenRec` com dezenas de ponteiros de função (`CreateWindow`, `RealizeWindow`, `ValidateTree`, `GetWindowPixmap`,
`BlockHandler`…). **Toda a mágica de plugabilidade do X é function-pointer wrapping desses campos.**

**[dispd]** O `dispd` colapsa DIX+DDX num processo só (é dono de UMA janela real, a raiz fullscreen — `dispd.h:Server.root`),
o que é legítimo para um servidor de uma tela só. Mas vale manter a *disciplina conceitual*: a lógica de protocolo
(quem é dono de qual superfície, quem recebe qual evento) deveria ficar separável do backend de apresentação
(`PresentBackend *present` em `dispd.h`), exatamente como DIX↔DDX. Isso deixa trocar o backend (GDI hoje, DXGI/flip
amanhã — ver `nt-dwm-compositor.md` §3b/§3d) sem tocar na semântica.

---

## 2. O laço de dispatch e o modelo de cliente

### 2.1 O event loop: `WaitForSomething()` (`os/WaitFor.c:168`)

O coração é um `poll()` (via a abstração `ospoll`, `os/ospoll.c`) sobre todos os fds de cliente + fontes de input:

```
os/WaitFor.c:191   while (1) {
os/WaitFor.c:193       ProcessWorkQueue();               // jobs adiados
os/WaitFor.c:195       timeout = check_timers();         // menor deadline de timer
os/WaitFor.c:196       are_ready = clients_are_ready();  // clientes com request completo já bufferizado
os/WaitFor.c:201       BlockHandler(&timeout);           // DDX/extensões podem baixar o timeout (ex.: vblank)
os/WaitFor.c:208       i = ospoll_wait(server_poll, timeout);
os/WaitFor.c:210       WakeupHandler(i);
os/WaitFor.c:223       if (InputCheckPending()) return FALSE;  // há input p/ processar → volta pro Dispatch
os/WaitFor.c:226       if (are_ready) { ...; return TRUE; }
```

Detalhes que importam:
- **`BlockHandler`/`WakeupHandler`** são *ganchos* que a DDX e as extensões registram para participar do select. É assim
  que o driver `modesetting` insere o fd do DRM e o Present ajusta o timeout para acordar no vblank. **[dispd]** o
  equivalente é o loop do `dispd` acordar por: mensagem no `NTU_PIPE_DISPD` (comandos do `ntwm`), input, e o vsync do
  `PresentBackend`. Vale ter um "block handler" abstrato para o backend baixar o timeout.
- **`clients_are_ready()`** existe porque um cliente pode ter *várias* requisições completas já lidas do socket num único
  `read`. O servidor as processa sem voltar a chamar `poll()`, mas de forma *justa* (ver scheduler abaixo).

### 2.2 `Dispatch()` (`dix/dispatch.c:475`) — o laço round-robin

```
dix/dispatch.c:487  while (!dispatchException) {
dix/dispatch.c:488      if (InputCheckPending()) { ProcessInputEvents(); FlushIfCriticalOutputPending(); }
dix/dispatch.c:493      if (!WaitForSomething(clients_are_ready())) continue;
dix/dispatch.c:502      client = SmartScheduleClient();      // escolhe o próximo cliente (prioridade)
dix/dispatch.c:507      while (!isItTimeToYield) {
dix/dispatch.c:508          if (InputCheckPending()) ProcessInputEvents();   // input ENTRE requests
dix/dispatch.c:521          result = ReadRequestFromClient(client);          // 1 request contígua
dix/dispatch.c:529          client->sequence++;
dix/dispatch.c:530          client->majorOp = ((xReq *) client->requestBuffer)->reqType;
dix/dispatch.c:532          if (client->majorOp >= EXTENSION_BASE) { ... minorOp ... }  // opcode de extensão
dix/dispatch.c:549          result = XaceHookDispatch(client, client->majorOp);        // hook de segurança (XACE)
dix/dispatch.c:552          result = (*client->requestVector[client->majorOp])(client); // <<< O DISPATCH
dix/dispatch.c:567          if (client->noClientException != Success) { CloseDownClient(client); break; }
dix/dispatch.c:571          else if (result != Success) { SendErrorToClient(...); break; }
dix/dispatch.c:578      FlushAllOutput();
```

A linha **`dix/dispatch.c:553`** é o núcleo de todo o servidor: `client->requestVector[client->majorOp](client)`.
`requestVector` é um array de 256 ponteiros de função indexado pelo **opcode major** (primeiro byte da requisição).

### 2.3 A tabela de requisições: `ProcVector[256]` (`dix/tables.c:66`)

```c
int (*ProcVector[256]) (ClientPtr) = {
    ProcBadRequest,          //   0
    ProcCreateWindow,        //   1
    ProcChangeWindowAttributes,
    ProcGetWindowAttributes,
    ProcDestroyWindow,
    ...
    ProcReparentWindow,      //   7
    ProcMapWindow,           //   8
    ...
    ProcConfigureWindow,     //  12
    ...
};
```

- **Opcodes 1–127** são o X11 core (fixos, definidos no protocolo). **Opcode 128–255** (`EXTENSION_BASE = 128`,
  `include/misc.h:102`) são atribuídos dinamicamente a extensões (§8).
- Existe um **`SwappedProcVector`** paralelo: se o cliente tem *byte order* diferente do servidor, `client->requestVector`
  aponta para as versões que fazem *byte-swap* antes de decodificar (`dix/dispatch.c:3723`:
  `client->requestVector = client->swapped ? SwappedProcVector : ProcVector;`). Elegante: o swap é escolhido *uma vez*, no
  handshake, não por-request.
- Cada `ProcFoo` faz o mesmo ritual: `REQUEST(xFooReq)` (casta o buffer), `REQUEST_SIZE_MATCH` (valida tamanho),
  `dixLookupWindow`/`VALIDATE_DRAWABLE_AND_GC` (resolve XIDs em ponteiros com checagem de acesso), executa, e retorna
  `Success` ou um código de erro. Ex.: `ProcMapWindow` (`dix/dispatch.c`) é literalmente
  `dixLookupWindow(&pWin, stuff->id, client, DixShowAccess); MapWindow(pWin, client); return Success;` — ou seja, o
  handler de protocolo é uma casca fina sobre a função DIX real (`MapWindow`, §4).

### 2.4 Leitura da requisição: `ReadRequestFromClient()` (`os/io.c:227`)

O protocolo X é *stream de bytes* sobre socket. O tamanho de cada request está nos bytes 3–4 (em unidades de 4 bytes),
salvo *Big Requests* (bytes 3–4 = 0, aí os 4 bytes seguintes dão o tamanho — `os/io.c:296 get_big_req_len`). A função:
1. Garante um buffer de input (reciclado entre clientes p/ economizar RAM — `NextAvailableInput`, `os/io.c:206`).
2. Lê o cabeçalho, calcula `needed`, lê o resto até ter **uma requisição contígua** (`client->requestBuffer`).
3. Retorna `>0` (ok, tamanho), `0` (incompleto, volta depois) ou `-1` (matar cliente).

A *contiguidade* é requisito do dispatcher: ele casta `requestBuffer` direto para `xFooReq*`. **[dispd]** como o `dispd`
usa named pipe do NT em vez de socket, o mesmo cuidado vale: enquadrar mensagens (comprimento no cabeçalho), montar a
mensagem completa antes de despachar, e ter buffer por-conexão.

### 2.5 Scheduler "justo": `SmartScheduleClient` (`dix/dispatch.c:330`)

O X não deixa um cliente tagarela travar os outros. `isItTimeToYield` (volátil, `dix/dispatch.c:177`) é setado por
`YieldControl()` (`os/io.c:178`) quando o cliente esvaziou seu buffer, e um *time slice* (`SmartScheduleSlice`) força a
rotação mesmo se o cliente tem mais requests. Clientes ganham/perdem prioridade (`smart_priority`) conforme consomem CPU.
`ProcessInputEvents()` é chamado *entre cada request* (`dix/dispatch.c:508`) — **input tem prioridade sobre o pipeline de
requests**, senão o mouse travaria sob carga. **[dispd]** lição: no drain do pipe (`wmproto_drain`), processar input a
cada N comandos, não só no fim.

### 2.6 O modelo de cliente: `ClientRec` (`include/dixstruct.h`)

```c
typedef struct _Client {
    void *requestBuffer;              // a request atual, contígua
    void *osPrivate;                  // OsCommPtr: fd, buffers, scheduler
    Mask clientAsMask;                // << os bits de cliente no XID (identidade!)
    short index;                      // slot em clients[]  (1..LimitClients)
    unsigned char majorOp, minorOp;
    unsigned int swapped:1;           // byte order != servidor
    unsigned int clientGone:1;
    short noClientException;          // "este cliente morreu / precisa morrer"
    int (**requestVector)(ClientPtr); // = ProcVector ou SwappedProcVector
    int sequence;                     // nº de sequência (p/ casar reply/erro)
    PrivateRec *devPrivates;          // storage por-extensão pendurado no cliente
    ...
} ClientRec;
```

Clientes vivem num array global `clients[]` (índice = `client->index`). O índice **1** é o `serverClient` (recursos do
próprio servidor). `nextFreeClientID` (`dix/dispatch.c:164`) é sempre o menor slot livre.

---

## 3. Recursos e XIDs — o mecanismo que faz tudo ter dono

Este é o subsistema mais copiável do X e o que dá **limpeza automática** quando um cliente morre. Sem isso, um WM ou app
que crashe vazaria janelas para sempre.

### 3.1 A anatomia de um XID (`include/resource.h:102`)

Um XID é um inteiro de **29 bits** partido em dois campos:

```
include/resource.h:104   #define RESOURCE_AND_CLIENT_COUNT  29
include/resource.h:105   #define RESOURCE_CLIENT_BITS       ResourceClientBits()      // = ilog2(LimitClients)
include/resource.h:106   #define CLIENTOFFSET   (RESOURCE_AND_CLIENT_COUNT - RESOURCE_CLIENT_BITS)
include/resource.h:108   #define RESOURCE_ID_MASK    ((1 << CLIENTOFFSET) - 1)        // bits que o CLIENTE escolhe
include/resource.h:110   #define RESOURCE_CLIENT_MASK  (((1 << RESOURCE_CLIENT_BITS) - 1) << CLIENTOFFSET)
include/resource.h:114   #define CLIENT_ID(id)   ((int)(CLIENT_BITS(id) >> CLIENTOFFSET))  // XID → índice do dono
```

Com `LimitClients = 256` (default), `RESOURCE_CLIENT_BITS = 8`, então `CLIENTOFFSET = 21`: cada cliente tem **2²¹ ≈ 2
milhões de XIDs** só seus, e os **8 bits altos codificam qual cliente é o dono**. Consequência-chave:

> **Dado qualquer XID, o servidor recupera o cliente-dono em O(1) por bit-shift** (`CLIENT_ID(id)`), sem tabela de busca.

### 3.2 Alocação decentralizada mas verificável: `ridBase`/`ridMask`

No handshake, o servidor devolve ao cliente (`dix/dispatch.c:3725`):

```c
((xConnSetup *) lConnectionInfo)->ridBase = client->clientAsMask;   // os bits altos do cliente
((xConnSetup *) lConnectionInfo)->ridMask = RESOURCE_ID_MASK;       // os bits que ele pode preencher
```

e `client->clientAsMask = ((Mask) i) << CLIENTOFFSET;` (`dix/dispatch.c:3594`). Ou seja: **o cliente gera os próprios
XIDs** localmente (Xlib faz `ridBase | próximo_contador`, sem round-trip ao servidor — isso é o que faz `XCreateWindow`
retornar um ID *na hora*). O servidor não precisa confiar cegamente: ele valida no `AddResource` que `CLIENT_ID(id)` bate
com o cliente que está pedindo, e a máscara garante que nenhum cliente consegue forjar o XID de outro.

### 3.3 A tabela de recursos e `AddResource` (`dix/resource.c:805`)

Cada cliente tem uma **hash table própria** (`clientTable[MAXCLIENTS]`, `dix/resource.c:601`), com listas encadeadas por
bucket, redimensionada quando cresce (`RebuildTable`, `dix/resource.c:839`). Um recurso é `{id, type, value}`:

```c
dix/resource.c:814   client = CLIENT_ID(id);                 // dono derivado do próprio XID
dix/resource.c:823   head = &rrec->resources[HashResourceID(id, hashsize)];
dix/resource.c:824   res = malloc(sizeof(ResourceRec));
dix/resource.c:830   res->id = id; res->type = type; res->value = value;   // value = ponteiro real (ex.: WindowPtr)
```

O **`type`** carrega uma `DeleteType` — a função de destruição (`resourceTypes[type].deleteFunc`). Tipos são criados por
`CreateNewResourceType(deleteFunc, name)` (`dix/resource.c:506`); os predefinidos (`RT_WINDOW`, `RT_PIXMAP`, `RT_GC`,
`RT_COLORMAP`, `RT_FONT`, …) estão em `predefTypes[]` (`dix/resource.c:431`). Extensões criam seus próprios tipos.

### 3.4 A joia da coroa: **limpeza automática na morte do cliente**

Quando um cliente desconecta, `CloseDownClient` chama `FreeClientResources`, que **itera a hash table daquele cliente e
chama o `deleteFunc` de cada recurso**. Isso destrói as janelas, GCs, pixmaps, e — crucialmente — remove o cliente das
listas de *event mask* de outras janelas via o tipo `RT_OTHERCLIENT` (§4.4). É por isso que **matar seu WM não derruba o
X**: as janelas dos apps são recursos *dos apps*, não do WM; só some a moldura/decoração que o WM criou.

`FakeClientID(client)` (`dix/resource.c:783`) gera IDs internos do servidor (com `SERVER_BIT`, `include/resource.h:115`)
para recursos que o *servidor* cria em nome de um cliente (ex.: a entrada `OtherClients` de um event-mask selection). Isso
mantém tudo no mesmo esquema de dono/limpeza.

**[dispd] — a maior lição isolada deste documento.** Hoje o `dispd` usa `unsigned id; next_id;` (`dispd.h:Window.id`,
`Server.next_id`) — IDs planos, sem dono embutido, sem limpeza automática. Adotar o esquema do X:
- **Codificar o dono no ID** (bits altos = índice do cliente/conexão `ntwm`/app; bits baixos = contador local).
- **Uma tabela de recursos por conexão**, com `deleteFunc` por tipo (Window, Surface/DIB, PTY/Terminal…).
- **No `disconnect` de uma conexão do pipe, varrer a tabela e destruir tudo dela** — assim um app ou o próprio `ntwm`
  que morre não vaza janelas/DIBs. Isso é exatamente o que `win_destroy` deveria receber "de graça".

---

## 4. Substructure redirection — como um WM externo é possível (o ponto mais fundo)

Aqui está *o* mecanismo. A pergunta central: como um processo separado (o WM) consegue **interceptar** o mapeamento e o
posicionamento de janelas de *outros* clientes e impor sua própria política (tiling, decoração, workspaces), sem que o
servidor tenha uma linha de código sobre "window manager"?

### 4.1 A ideia em uma frase

Um cliente seleciona `SubstructureRedirectMask` na janela **pai** (tipicamente a **raiz**). A partir daí, quando *qualquer
outro* cliente pedir para mapear/configurar/circular um **filho** daquela janela, o servidor **não executa** a operação:
ele converte o pedido num **evento** (`MapRequest`/`ConfigureRequest`/`CirculateRequest`) e entrega **ao cliente que
segura a máscara** (o WM). A janela fica no estado anterior. O WM decide o que fazer (reparent para uma moldura,
posicionar, e então re-emitir o map/configure ele mesmo).

### 4.2 As macros de decisão (`dix/window.c:174`)

```c
dix/window.c:174   #define RedirectSend(pWin) \
                       ((pWin->eventMask | wOtherEventMasks(pWin)) & SubstructureRedirectMask)
dix/window.c:177   #define SubSend(pWin) \
                       ((pWin->eventMask | wOtherEventMasks(pWin)) & SubstructureNotifyMask)
```

`RedirectSend(pParent)` pergunta: "*alguém* selecionou redirect na máscara desta janela (a do criador OU a de outros
clientes)?".

### 4.3 O caminho de `MapWindow` (`dix/window.c:2653`)

```c
int MapWindow(WindowPtr pWin, ClientPtr client) {
    ...
dix/window.c:2673   if ((!pWin->overrideRedirect) && (RedirectSend(pParent)))
dix/window.c:2674       if (MaybeDeliverMapRequest(pWin, pParent, client))
dix/window.c:2675           return Success;          // <<< NÃO mapeia; virou evento pro WM
dix/window.c:2677   pWin->mapped = TRUE;             // só chega aqui se NINGUÉM redirecionou
    ...
```

`MaybeDeliverMapRequest` (`dix/window.c:2621`) monta o evento `MapRequest{window, parent}` e chama
`MaybeDeliverEventsToClient(pParent, &event, 1, SubstructureRedirectMask, client)`. Dois pontos cirúrgicos:

- **`overrideRedirect`**: se a janela tem `override_redirect = True` (menus, tooltips, splash), o redirect é **ignorado** —
  o WM *não* a gerencia. É como um app diz "não decore/gerencie esta janela". **[dispd]** replicar: uma flag por janela
  ("unmanaged") que faz o `dispd` mapear direto sem consultar o `ntwm`.
- **O `dontClient = client`**: o cliente que *pediu* o map é passado como "não entregue a ele". Isso é o que evita loop
  infinito (§4.5).

O `ConfigureWindow` faz o análogo (`dix/window.c:2287`): monta `ConfigureRequest{x,y,width,height,borderWidth,sibling,
valueMask}` e, se `MaybeDeliverEventsToClient(...) == 1`, **`return Success` sem aplicar a geometria** (`dix/window.c:2307`).
Há ainda `ResizeRedirectMask` (`dix/window.c:2316`) — mais granular, só o resize. E `CirculateWindow` (`dix/window.c:2470`).

### 4.4 O ponto de estrangulamento: `MaybeDeliverEventsToClient` (`dix/events.c:2554`)

```c
int MaybeDeliverEventsToClient(WindowPtr pWin, xEvent *pEvents, int count, Mask filter, ClientPtr dontClient) {
    OtherClients *other;
    if (pWin->eventMask & filter) {                        // o criador da janela selecionou?
dix/events.c:2561       if (wClient(pWin) == dontClient) return 0;     // é o próprio requisitante → não redireciona
dix/events.c:2570       return TryClientEvents(wClient(pWin), ...);    // entrega e retorna 1
    }
dix/events.c:2573   for (other = wOtherClients(pWin); other; other = other->next) {   // outros clientes selecionaram?
dix/events.c:2574       if (other->mask & filter) {
dix/events.c:2575           if (SameClient(other, dontClient)) return 0;
dix/events.c:2585           return TryClientEvents(rClient(other), ...);              // entrega e retorna 1
        }
    }
dix/events.c:2589   return 2;                                          // ninguém interessado
}
```

Retorno **1** = entregue ao WM (o chamador então suprime a ação); **0** = o próprio requisitante seria o alvo (segue a
ação); **2** = ninguém (segue a ação). Note `wOtherClients(pWin)` — a lista `OtherClients` são clientes *que não criaram*
a janela mas selecionaram eventos nela. Cada entrada é um recurso `RT_OTHERCLIENT` (`dix/events.c:4605`) → **some
automaticamente quando aquele cliente morre** (`OtherClientGone`, `dix/events.c:4524`). É assim que o WM sumir libera o
redirect da raiz sem deixar lixo.

### 4.5 Por que não vira loop infinito

Quando o WM, em resposta ao `MapRequest`, faz *ele mesmo* `XMapWindow(child)`, o `MapWindow` roda de novo, `RedirectSend`
ainda é verdadeiro… mas agora `dontClient` é o **próprio WM**, e ele *é* o cliente que segura a máscara. Então
`MaybeDeliverEventsToClient` bate em `wClient(pWin) == dontClient` (ou `SameClient`) e retorna **0** →
`MaybeDeliverMapRequest` retorna false → **o map executa de verdade**. O WM é o único cliente cujos próprios pedidos *não*
são redirecionados de volta para si.

### 4.6 A garantia "só existe um WM": `AtMostOneClient` (`dix/events.c:4470`)

```c
dix/events.c:4470   #define AtMostOneClient (SubstructureRedirectMask | ResizeRedirectMask | ButtonPressMask)
dix/events.c:4472   #define ManagerMask     (SubstructureRedirectMask | ResizeRedirectMask)
```

No `EventSelectForWindow` (`dix/events.c:4548`, chamado por `ChangeWindowAttributes`/`SelectInput`):

```c
dix/events.c:4566   check = (mask & AtMostOneClient);
dix/events.c:4567   if (check & (pWin->eventMask | wOtherEventMasks(pWin))) {
dix/events.c:4571       if ((wClient(pWin) != client) && (check & pWin->eventMask)) return BadAccess;
dix/events.c:4573       for (others = wOtherClients(pWin); others; others = others->next)
dix/events.c:4574           if (!SameClient(others, client) && (check & others->mask)) return BadAccess;
    }
```

Traduzindo: **se outro cliente já segura `SubstructureRedirect`/`ResizeRedirect` nesta janela, sua seleção falha com
`BadAccess`.** É *literalmente* a regra "só um window manager por tela": o WM tenta `SelectInput(root,
SubstructureRedirectMask)` na inicialização; se já há um WM, ele leva `BadAccess` e sai ("another window manager is
already running"). Nenhum código de "WM" no servidor — só uma regra de exclusividade sobre uma máscara de evento.

Também requer `DixManageAccess` via XACE (`dix/events.c:4561`) — hook de segurança para políticas que restrinjam quem
pode gerenciar.

### 4.7 Reparenting: como a moldura do WM aparece (`dix/window.c:2502`)

O WM tipicamente cria uma **janela-moldura** própria (com título/bordas), faz `ReparentWindow(appWin, frameWin, ...)`
(`ProcReparentWindow` → `dix/window.c:2502`), e mapeia a moldura. `ReparentWindow`:
1. Desmapeia temporariamente (`dix/window.c:2518`), emite `ReparentNotify` (`dix/window.c:2528-2535`);
2. **Retira a janela da cadeia de irmãos do pai antigo e insere na do novo** (`dix/window.c:2537-2570`) — pura cirurgia de
   ponteiros `firstChild/lastChild/nextSib/prevSib` na `WindowRec`.

Depois disso, os filhos do frame são gerenciados pelo WM, e o app continua desenhando na *sua* janela (agora filha da
moldura). **[dispd]** o `dispd` já modela isso melhor para o caso NT: não há reparent real; o `ntwm` declara a geometria
(borda `border_px`, `rect`) e o `dispd` desenha a decoração ao compor (`dispd.h:Window.border_px/border_rgb`). Ou seja, o
`dispd` faz *server-side decoration* em vez de forçar o WM a criar molduras — mais simples e sem o balé de reparent.

### 4.8 O contrato completo WM↔servidor (a sequência)

1. App: `CreateWindow` (filho da raiz) → `MapWindow`.
2. Servidor: raiz tem redirect do WM → suprime o map, entrega **`MapRequest`** ao WM.
3. WM: cria moldura, `ReparentWindow(app → moldura)`, posiciona, `MapWindow(moldura)` e `MapWindow(app)` (agora não
   redirecionado, pois é o WM pedindo).
4. App: `ConfigureWindow` (quer se mover/redimensionar) → servidor entrega **`ConfigureRequest`** ao WM; o WM re-emite o
   configure com a geometria que *ele* decidiu.
5. Foco/decoração: WM usa `SetInputFocus` (§5) e desenha a moldura.

Tudo isso é protocolo puro. Trocar o WM = matar um processo e subir outro que refaça o passo 0 (`SelectInput` na raiz).

---

## 5. Janelas, clipping e desenho

### 5.1 A árvore de janelas: `WindowRec` (`include/windowstr.h`)

```c
typedef struct _Window {
    DrawableRec drawable;         // id, tipo, x/y/w/h, depth, pScreen, serialNumber
    WindowPtr parent, nextSib, prevSib, firstChild, lastChild;   // a árvore
    RegionRec clipList;           // onde ESTA janela pode desenhar (recortada pelos filhos)
    RegionRec borderClip;         // clipList + borda (não recortada pelos filhos)
    RegionRec winSize, borderSize;
    DDXPointRec origin;           // posição relativa ao pai
    Mask eventMask;               // máscara do cliente CRIADOR
    unsigned short deliverableEvents;  // OR de todas as máscaras de todos os clientes
    WindowOptPtr optional;        // otherClients (event masks de não-criadores), cursor, etc.
    unsigned overrideRedirect:1;
    unsigned mapped:1, realized:1, viewable:1;
    unsigned redirectDraw:2;      // COMPOSITE: None / Automatic / Manual
    ...
} WindowRec;
```

É uma árvore n-ária com irmãos em lista duplamente encadeada (ordem = *stacking order*: `firstChild` é o topo). A raiz é a
janela da tela inteira.

### 5.2 Clipping: `clipList` vs `borderClip` e `miComputeClips` (`mi/mivaltree.c:196`)

Sem compositor, **todas as janelas compartilham o mesmo framebuffer** (o *screen pixmap*). Para não desenharem umas por
cima das outras, cada janela tem um **`clipList`**: a região (conjunto de retângulos) onde ela *realmente* pode escrever —
sua área menos as partes cobertas por filhos e por irmãos acima. `miComputeClips` recomputa isso descendo a árvore:
- Calcula `borderSize` (o retângulo da janela + borda);
- Determina a **visibilidade** (`VisibilityUnobscured`/`PartiallyObscured`/`FullyObscured`) comparando com o "universe"
  (a região disponível vinda do pai) — `mi/mivaltree.c:244`;
- Subtrai a área dos filhos para formar o `clipList` de cada um;
- Emite `VisibilityNotify` se mudou (`mi/mivaltree.c:276`) e calcula regiões **`exposed`** (o que ficou descoberto e
  precisa de `Expose` para o app repintar).

Isto é disparado por `MarkOverlappedWindows` + `ValidateTree` + `HandleExposures` sempre que a topologia muda (map, unmap,
configure, restack — ex.: `dix/window.c:2685-2692` no `MapWindow`).

> **O modelo de occlusão é o custo central do X clássico.** Como não há backing por-janela, mover uma janela expõe o que
> estava atrás, e o *cliente de trás* precisa **repintar** (recebe `Expose`). Daí "janelas em branco quando o app trava".

### 5.3 O que o Composite muda (e o que o `dispd` já assume)

Com a extensão Composite, uma janela redirecionada ganha `redirectDraw != RedirectDrawNone` e um **pixmap próprio**; aí o
`clipList` dela vira o retângulo inteiro (não recortado por irmãos), porque ela desenha *offscreen* — o compositor cuida
da sobreposição (`mi/mivaltree.c:235`: `if (pParent->redirectDraw != RedirectDrawNone) RegionCopy(universe,
&pParent->borderSize);`). **[dispd]** o `dispd` **nasce nesse regime**: cada `Window` tem seu `HDC memdc; HBITMAP dib;
void *bits;` (`dispd.h:33-36`) e nunca lê a superfície de ninguém — o `compose_and_present` (`dispd.h:81`) compõe. Ou seja,
o `dispd` pulou o modelo de occlusão do X inteiro e foi direto para "Composite sempre ligado". Isso elimina `clipList`,
`Expose`, `ValidateTree` — uma simplificação enorme e correta para um servidor moderno.

### 5.4 Desenho: o GC e a vtable de ops (`dix/dispatch.c`, `ProcPolyFillRectangle`)

O X separa *o quê* desenhar (protocolo) de *como* rasterizar (fb/DDX) via o **Graphics Context** e sua **vtable `GCOps`**:

```c
ProcPolyFillRectangle(ClientPtr client) {
    ...
    VALIDATE_DRAWABLE_AND_GC(stuff->drawable, pDraw, DixWriteAccess);   // resolve XIDs + valida acesso
    ...
    (*pGC->ops->PolyFillRect)(pDraw, pGC, things, (xRectangle *)&stuff[1]);   // <<< vtable
    return Success;
}
```

`pGC->ops` é escolhido por `ValidateGC` conforme o drawable e o estado do GC; a implementação padrão é a do **`fb`** (que
escreve pixels em memória), mas a DDX/aceleradores podem substituir por versões via GPU (EXA/glamor). **[dispd]** o mesmo
padrão de *vtable de backend de desenho* vale: as primitivas de desenho do `dispd` (texto do terminal, retângulos)
poderiam ir por uma interface que hoje é GDI (`memdc`) e amanhã Direct2D, sem mudar quem chama.

### 5.5 Root window, screen pixmap e backing store

A **janela raiz** é criada pela DDX na inicialização da tela; seu "pixmap" é o *screen pixmap* — o framebuffer real. Todas
as janelas não-redirecionadas desenham nele através de seus `clipList`. **Backing store** (opcional) manda o servidor
guardar o conteúdo coberto de uma janela para repintar sem `Expose` — historicamente caro, hoje raramente usado; o
Composite tornou-o obsoleto. **[dispd]** o `dispd` é dono de UMA janela real (`Server.root` = HWND fullscreen) e de um
back-buffer composto (`Server.cdc/cdib/cbits`, `dispd.h:56-57`) — exatamente o papel de "screen pixmap", só que o
conteúdo vem da composição dos DIBs por-janela, não de escrita direta dos clientes.

---

## 6. Input — do dispositivo ao cliente certo

### 6.1 Entrada dos eventos: fila `mieq` e `ProcessInputEvents`

Drivers de input (evdev/libinput via `hw/xfree86/`, ou a *input thread* `os/inputthread.c`) leem o dispositivo e chamam
`mieqEnqueue` (`mi/mieq.c`), empilhando na fila `miEventQueue` (`mi/mieq.c:88`). O `SetInputCheck` (`mi/mieq.c:175`) liga
essa fila ao `InputCheckPending()` que o `Dispatch`/`WaitForSomething` consultam. No laço, `ProcessInputEvents()` drena a
fila chamando `mieqProcessInputEvents` → `mieqProcessDeviceEvent` → os handlers do dispositivo.

O input passa por `getevents.c` (`GetPointerEvents`/`GetKeyboardEvents`) que transforma o evento *bruto* do driver em
**eventos internos** (`InternalEvent`) já com valuators processados (aceleração de ponteiro em `dix/ptrveloc.c`), e então
a DIX os *entrega*.

### 6.2 A entrega hierárquica: `DeliverDeviceEvents` (`dix/events.c:2886`)

Para um evento *não-grabbed, não-focado* (tipicamente do ponteiro), o servidor descobre a janela sob o cursor com
**`XYToWindow`** (`dix/events.c:3053`, que desce a árvore achando a janela mais profunda que contém o ponto) e então:

```c
dix/events.c:2895   while (pWin) {
dix/events.c:2896       if ((mask = EventIsDeliverable(dev, event->any.type, pWin))) {
dix/events.c:2898           if (mask & EVENT_XI2_MASK)  ... DeliverOneEvent(XI2) ...   // XInput2 primeiro
dix/events.c:2906           if (mask & EVENT_XI1_MASK)  ... DeliverOneEvent(XI) ...
dix/events.c:2913           if ((mask & EVENT_CORE_MASK) && IsMaster(dev)) ... DeliverOneEvent(CORE) ...
        }
dix/events.c:2922       if ((deliveries < 0) || (pWin == stopAt) || (mask & EVENT_DONT_PROPAGATE_MASK)) break;
dix/events.c:2928       child = pWin->drawable.id;
dix/events.c:2929       pWin = pWin->parent;                 // <<< PROPAGA PARA O PAI
    }
```

**A propagação sobe a árvore**: se a janela sob o cursor não tem interesse no evento, ele sobe para o pai, e assim por
diante até alguém entregar ou até `DontPropagate` barrar. É isso que permite um app selecionar cliques só na janela
top-level e receber cliques dos filhos. `DeliverEventsToWindow` (`dix/events.c:2362`) faz a entrega efetiva, checando a
`eventMask` do criador e a lista `otherClients` — o **mesmo** mecanismo de máscara do redirect (§4.4), só com filtro
diferente.

### 6.3 Foco de teclado: `SetInputFocus` (`dix/events.c:4879`)

O teclado é entregue à **janela de foco**, não à janela sob o cursor. `ProcSetInputFocus` (`dix/events.c:4970`) →
`SetInputFocus` guarda a janela focada por dispositivo; `DeliverFocusedEvent` (`dix/events.c:4202`) roteia teclas para lá.
**É o WM quem chama `SetInputFocus`** conforme sua política (click-to-focus, focus-follows-mouse). O servidor não tem
política de foco — só o mecanismo. Modos especiais: `PointerRoot` (foco segue o cursor) e `RevertTo` (para onde o foco
volta se a janela focada some).

### 6.4 Grabs: `Grab*` e passives (`dix/events.c`)

Um **grab** redireciona *todos* os eventos de um dispositivo para um cliente/janela, ignorando a hierarquia:
- **Active grab**: `ActivatePointerGrab`/`ActivateKeyboardGrab` (`dix/events.c:1612/1731`) — usado por menus, drag,
  `XGrabPointer`.
- **Passive grab**: `GrabKey`/`GrabButton` armam um gatilho — quando a combinação (tecla+modificadores) ocorre, o grab
  ativa. `CheckPassiveGrabsOnWindow` (`dix/events.c:4053`) varre os grabs registrados na janela e ativa o que casa.

**É assim que atalhos globais do WM funcionam**: o WM faz `XGrabKey(Mod+Enter, root)`; quando o usuário aperta, o servidor
entrega ao WM em vez de ao app focado. **[dispd]** o `dispd` já tem o análogo: `wmproto_grabbed(mods, vk)` (`dispd.h:92`) e
`input_keydown` consultando os grabs do `ntwm` antes de rotear a tecla para o terminal focado. Isso é *exatamente* o
passive-grab do X, bem modelado.

---

## 7. DDX/modesetting — do pixmap ao scanout (o `PresentBackend` do X)

O driver `modesetting` (`hw/xfree86/drivers/modesetting/`) é o backend KMS/DRM genérico.

### 7.1 Setup: `ScreenRec`, CRTCs e o framebuffer DRM

`drmmode_display.c` cria o framebuffer via *dumb buffers* (`dumb_bo.c`), configura CRTCs/conectores (RandR mapeia CRTC↔
saída física), e define o *screen pixmap* apontando para o BO do framebuffer. `driver.c` liga tudo ao `ScrnInfoRec`.

### 7.2 Apresentação: page flip para o scanout

O caminho "mostrar um quadro" é um **page flip** DRM. `do_queue_flip_on_crtc` (`hw/xfree86/drivers/modesetting/
pageflip.c:182`) chama `drmmode_crtc_flip(crtc, fb_id, x, y, flags, ...)` → `drmModePageFlip`, que agenda a troca do
buffer de scanout no **vblank**. A conclusão volta como evento no fd do DRM, processado por `ms_flush_drm_events`
(`pageflip.c:70`) — plugado no `WaitForSomething` via block/wakeup handler. `queue_flip_on_crtc` (`pageflip.c:217`)
registra um handler (`ms_pageflip_handler`) por CRTC; só o CRTC de referência entrega o evento de completion.

### 7.3 A extensão Present: flip vs copy (`present/`, `hw/.../modesetting/present.c`)

A extensão **Present** (`present/present.c`, `present_scmd.c`, `present_execute.c`) é a API moderna que os apps usam para
"apresentar um pixmap nesta janela no vblank N" (é o que DRI3/Vulkan/GL usam). Ela decide entre dois caminhos:
- **Flip** (`present_check_flip`, `present/present_scmd.c:59`; `ms_present_check_flip`, `.../modesetting/present.c:317`):
  se o pixmap do app cobre a tela inteira e o formato bate, o Present faz um **page flip direto** — *zero cópia*, o buffer
  do app vira o scanout. É o "independent flip / unredirect fullscreen".
- **Copy** (`present_execute_copy`, `present/present_execute.c:102`): senão, copia a região do pixmap para a janela
  (`present_copy_region`, `present_execute.c:119`) e deixa o compositor compor normalmente.

`ms_present_flip` (`.../modesetting/present.c:367`) faz o flip real; `present_vblank`/`present_execute` sincronizam com o
MSC (media stream counter) do CRTC. **[dispd]** isto é o gêmeo exato do que `nt-dwm-compositor.md` §3b/§3d descreve no lado
NT (flip-model DXGI vs composição): a *mesma* decisão "cabe a tela inteira → flip sem cópia; senão → compõe". O
`PresentBackend` do `dispd` (`dispd.h:present.h`) deve ter essa dualidade: caminho rápido de flip quando uma só janela
está fullscreen, caminho de composição no caso geral.

---

## 8. Extensões — como o compositor e amigos se plugam

### 8.1 O mecanismo universal: `AddExtension` (`dix/extension.c`)

Toda extensão chama `AddExtension(name, NumEvents, NumErrors, MainProc, SwappedMainProc, CloseDownProc, MinorOpcodeProc)`:

```c
dix/extension.c   ext->base = i + EXTENSION_BASE;          // opcode major atribuído (128+)
                  ProcVector[i + EXTENSION_BASE]        = MainProc;
                  SwappedProcVector[i + EXTENSION_BASE] = SwappedMainProc;
                  ext->eventBase = lastEvent;  lastEvent += NumEvents;    // faixa de eventos
                  ext->errorBase = lastError;  lastError += NumErrors;    // faixa de erros
```

Ou seja: **cada extensão recebe um opcode major**, e dentro dele despacha pelo **minor opcode** (o segundo byte da
request) numa sub-tabela própria (ex.: `composite/compext.c:360` tem o array de `ProcCompositeRedirectWindow`, etc.). O
cliente descobre o opcode dinâmico via `QueryExtension`. Eventos e erros também ganham faixas contíguas. Isso é o que faz
o X ser *extensível sem recompilar o core*: RandR, Composite, Damage, XFixes, Present, XInput2, XKB — todos entram por
essa mesma porta. **[dispd]** desde já reservar no protocolo do `dispd`: um espaço de opcodes core + um mecanismo de
"opcode de extensão + minor" e faixas de evento/erro. Barato agora, caro de retrofit depois.

### 8.2 Composite — a base do compositing (`composite/`)

Composite responde à pergunta: como um compositor externo obtém o conteúdo de *cada* janela para compor com transparência/
sombras/animações? Resposta: **redirecionando o desenho da janela para um pixmap offscreen**.

`ProcCompositeRedirectWindow` (`composite/compext.c:141`) → `compRedirectWindow` (`composite/compalloc.c:136`), com dois
modos:
- **`CompositeRedirectAutomatic`**: o servidor continua compondo a janela na tela normalmente, mas agora via o pixmap
  (usado quando você quer só ler o conteúdo). Seta `pWin->redirectDraw = RedirectDrawAutomatic` (`compalloc.c:305`).
- **`CompositeRedirectManual`**: o servidor **para de pintar** a janela na tela — *o compositor assume total
  responsabilidade* por colocá-la lá. É o modo que compositores reais (mutter, picom) usam.

`compAllocPixmap` (`composite/compalloc.c:610`) é o coração:

```c
compalloc.c:617   pPixmap = compNewPixmap(pWin, x, y, w, h);        // pixmap offscreen do tamanho da janela+borda
compalloc.c:625   if (cw->update == CompositeRedirectAutomatic) pWin->redirectDraw = RedirectDrawAutomatic;
compalloc.c:627   else                                              pWin->redirectDraw = RedirectDrawManual;
compalloc.c:630   compSetPixmap(pWin, pPixmap, bw);                 // <<< janela passa a desenhar AQUI
compalloc.c:634   if (cw->update == CompositeRedirectAutomatic) { DamageRegister(&pWin->drawable, cw->damage); ... }
```

O truque de redirecionamento é o `pScreen->GetWindowPixmap`: normalmente devolve o *screen pixmap* (framebuffer
compartilhado); para janelas redirecionadas, devolve o **pixmap privado**. Como todo desenho passa por
`GetWindowPixmap(pWin)`, trocar o pixmap redireciona *todas* as ops de desenho da janela para o offscreen, sem tocar em
`ProcPolyFillRectangle` & cia. **É a mesma inversão do `dispd`**: cada janela tem seu DIB (`dispd.h:Window.dib`), e o
`compose_and_present` lê todos e monta a tela.

`compCreateOverlayWindow` (`composite/compoverlay.c:127`) cria a **Composite Overlay Window (COW)** — uma janela especial,
acima de todas as outras janelas normais mas abaixo do cursor, onde o compositor desenha o resultado final. É o "canvas"
do compositor. `ProcCompositeGetOverlayWindow` (`compext.c:277`) a entrega ao cliente.

> **Composite é opt-in e por-janela.** No X, você *liga* o redirect. No `dispd`, isso é sempre ligado e implícito. Trade-off:
> o `dispd` ganha simplicidade e perde a capacidade de "unredirect" barato de uma janela fullscreen (que no X é o caminho
> flip do Present, §7.3) — vale o `dispd` ter um caminho explícito para isso (janela fullscreen → present direto do DIB
> dela como scanout, pulando a composição).

### 8.3 Damage — "o que mudou" (`damageext/`, `miext/damage/`)

Um compositor não pode recompor a tela inteira a cada pixel que um app muda. A extensão **Damage** rastreia *regiões
sujas*. `DamageRegister(drawable, damage)` (`miext/damage/damage.c`) instala hooks que, a cada operação de desenho,
acumulam a região afetada; `DamageReportDamage` (`damage.c:266`) notifica o cliente (nível `DamageReportRawRegion`,
`damageext/damageext.c:141`). O compositor então só recompõe o que sujou. **[dispd]** o `dispd` já tem `Window.dirty`
(`dispd.h:37`) — é o embrião do Damage. Refinar de "dirty booleano" para "região suja" (retângulos) reduz muito o custo de
composição em telas grandes.

### 8.4 RandR e XFixes (breve)

- **RandR** (`randr/`): resolução, rotação, múltiplos monitores, hotplug. Mapeia CRTCs/outputs do KMS (§7.1) ao protocolo.
  Emite eventos quando a config de tela muda. O WM escuta para reconfigurar o layout.
- **XFixes** (`xfixes/`): regiões server-side, controle de visibilidade/forma do cursor, e *selections* melhoradas.
  Fornece as `Region` que Composite/Damage/o compositor usam. `XFixesCreateRegion` etc.

---

## 9. POR QUE o design do X torna WM e compositor clientes trocáveis (a síntese)

Reunindo os fios, há **quatro decisões arquiteturais** que produzem a plugabilidade — e são as que quero replicar:

1. **Mecanismo, não política, no servidor.** O servidor implementa *primitivas* (redirect de substructure, máscaras de
   evento, foco, grabs, redirect de desenho). *Política* (como empilhar, decorar, focar, animar) vive nos clientes. O
   servidor nunca tem um `if (é_window_manager)`. Isso é o que permite `i3` e `mutter` — políticas radicalmente diferentes
   — falarem o *mesmo* protocolo.

2. **Interesse declarado por máscara + entrega por dono.** Tudo — do `MapRequest` ao clique — é roteado por "quem
   selecionou esta máscara nesta janela?" (`MaybeDeliverEventsToClient`, `DeliverEventsToWindow`). O WM não é especial; ele
   só selecionou `SubstructureRedirectMask`. Trocar o WM = outro processo selecionar a mesma máscara.

3. **Exclusividade como regra de dados, não código.** "Um WM por tela" é `AtMostOneClient` em `EventSelectForWindow`
   (`dix/events.c:4567`) retornando `BadAccess`. "Um compositor" é a posse da Composite Overlay Window. Não há registro de
   "o WM atual"; a exclusividade emerge da regra sobre a máscara.

4. **Recursos com dono + limpeza automática.** Todo objeto (janela, pixmap, GC, entrada de event-mask) é um recurso com
   `CLIENT_ID` embutido no XID e um `deleteFunc`. Quando o WM/compositor morre, **só os recursos dele somem** (moldura,
   COW, seleções de máscara), e as janelas dos apps — recursos *dos apps* — sobrevivem. É isso que torna o WM/compositor
   *reinicializável a quente*: você mata `picom`/`mutter`, os apps nem percebem, e sobe outro.

O corolário para o `dispd`: **a fronteira de processo entre `dispd` e `ntwm` só é robusta se o protocolo carregar essas
quatro propriedades.** Se `ntwm` fosse uma DLL linkada no `dispd`, um crash dele derrubaria o servidor. Mantê-lo como
cliente do pipe (como já está, `dispd.h:wmproto_*`) com IDs com dono e limpeza automática replica a resiliência do X.

---

## 10. Lições para o protocolo e a arquitetura do `dispd`/`ntwm`

Ordenadas por retorno sobre esforço.

### Alto impacto, fazer agora

1. **IDs com dono embutido + tabela de recursos por conexão + limpeza automática no disconnect.** (§3) Hoje `next_id`/`id`
   são planos. Adotar: `id = (conn_index << OFFSET) | contador_local`; uma hash/lista de recursos por conexão do pipe;
   `deleteFunc` por tipo (Window, DIB, PTY). Quando uma conexão cai, varrer e destruir tudo dela. **Isso é o que impede
   vazar janelas quando o `ntwm` ou um app crasha** — a propriedade que faz o WM ser reinicializável a quente. É a maior
   lição do X.

2. **`SubstructureRedirect` como o contrato central do `ntwm`, com a regra "no máximo um".** (§4) O `dispd` já roteia
   comandos do `ntwm` por pipe, mas formalizar: *criação/map/configure de janela de app dispara um evento
   `MapRequest`/`ConfigureRequest` para o `ntwm`, e a ação fica pendente até o `ntwm` responder*. Hoje o `dispd` parece
   aplicar layout de forma mais direta; o modelo do X (pedido→evento→WM decide→WM re-emite) dá ao `ntwm` **poder de veto e
   de política** sem o `dispd` conhecer a política. Garantir que só uma conexão pode ser "o WM" (equivalente ao
   `AtMostOneClient`), retornando erro para uma segunda.

3. **Flag `override_redirect`/"unmanaged" por janela.** (§4.3) Menus, tooltips, splash e o próprio terminal de sistema
   não devem passar pelo `ntwm`. Uma flag que faz o `dispd` mapear direto. Barato e essencial para não travar UI transiente
   esperando o WM.

4. **Máscaras de evento + entrega por interesse (incluindo grabs de tecla).** (§5, §6) Generalizar `wmproto_ev_*`
   (`dispd.h:93-96`) para um modelo de *subscrição*: uma conexão declara quais eventos quer (created/destroyed/title/key/
   focus/geometry). O `dispd` entrega só a quem assinou. Os passive grabs (`wmproto_grabbed`, já existe) são o caso
   especial de tecla — mantê-los, mas encaixá-los nesse modelo geral. Isso desacopla `ntwm` de um painel/barra/outro
   cliente que também queira eventos.

### Médio impacto, desenhar cedo para não sofrer depois

5. **Espaço de opcodes core + extensões (major/minor) e faixas de evento/erro.** (§8.1) Mesmo que só haja um "core" hoje,
   reservar o formato de mensagem para "opcode de extensão + minor opcode" e faixas de evento. Retrofit de extensibilidade
   é caro. Modele o handshake para negociar versão/extensões (como `QueryExtension`).

6. **Região suja em vez de `dirty` booleano.** (§8.3) Trocar `Window.dirty` (`dispd.h:37`) por uma lista de retângulos
   sujos por janela; `compose_and_present` recompõe só a união das regiões sujas. Ganho grande em telas 4K/multi-monitor.

7. **Dualidade flip vs compose no `PresentBackend`.** (§7.3) Quando exatamente uma janela cobre a tela (fullscreen), fazer
   *present direto do DIB dela como scanout* (o "unredirect"/independent-flip), pulando a composição. No caso geral,
   compor. É a mesma escolha que o Present do X e o DXGI flip-model fazem — e o `nt-dwm-compositor.md` já aponta o caminho
   NT (§3b/§3d). Manter a interface `PresentBackend` capaz de ambos.

8. **Decoração server-side (o `dispd` já faz — manter, é melhor que o X).** (§4.7) O X força o WM a criar molduras e
   reparentar (balé de `ReparentWindow`). O `dispd` deixa o `ntwm` *declarar* borda/título e desenha ele mesmo
   (`border_px`, `border_rgb`). Isso é objetivamente mais simples e evita toda a máquina de reparent/`ValidateTree`.
   Confirmar que o protocolo expõe o suficiente (borda, título, cor de foco) para o `ntwm` mandar política sem desenhar.

### Baixo impacto / disciplina de longo prazo

9. **Separar semântica (DIX) de backend (DDX) mesmo dentro de um processo.** (§1, §5.4) O `dispd` colapsa as camadas, mas
   manter a *fronteira conceitual* entre "quem é dono de qual superfície / quem recebe qual evento" e "como isso vira
   pixels no scanout" (GDI hoje, DXGI/D3DKMT amanhã). Vtable de backend de desenho e de present.

10. **Input com prioridade sobre o pipeline de comandos.** (§2.5) No `wmproto_drain`, intercalar processamento de input a
    cada N comandos, como o `Dispatch` chama `ProcessInputEvents` entre requests. Evita o mouse/teclado travarem sob
    enxurrada de comandos do `ntwm`.

11. **Foco é mecanismo no `dispd`, política no `ntwm`.** (§6.3) O `dispd` mantém "qual janela tem foco" e roteia teclado
    para ela (`Server.focused`, `win_focus` — já existe); *quem* recebe foco é decisão do `ntwm` (click-to-focus vs
    follows-mouse). Não embutir política de foco no `dispd`.

---

## 11. Índice de referências de código (arquivo:função — versão master `1133421bbd4a`)

**Dispatch / loop / cliente**
- `dix/dispatch.c:475` `Dispatch()` — laço round-robin; `:553` o dispatch `requestVector[majorOp](client)`.
- `dix/tables.c:66` `ProcVector[256]` — tabela de requisições core.
- `os/WaitFor.c:168` `WaitForSomething()` — o select/poll principal; `:201` `BlockHandler`, `:208` `ospoll_wait`.
- `os/io.c:227` `ReadRequestFromClient()` — enquadramento de request; `:178` `YieldControl`.
- `dix/dispatch.c:330` `SmartScheduleClient()` — scheduler justo.
- `include/dixstruct.h` `ClientRec` — o cliente; `dix/dispatch.c:3594` `clientAsMask`.

**Recursos / XIDs**
- `include/resource.h:102-115` — anatomia do XID (`CLIENT_ID`, `RESOURCE_ID_MASK`, `SERVER_BIT`).
- `dix/resource.c:805` `AddResource()`; `:894` `FreeResource()`; `:783` `FakeClientID()`; `:506` `CreateNewResourceType()`.
- `dix/dispatch.c:3725` `ridBase`/`ridMask` no handshake; `ProcEstablishConnection` `:3779`.

**Substructure redirection (WM)**
- `dix/window.c:174` `RedirectSend`; `:2653` `MapWindow`; `:2621` `MaybeDeliverMapRequest`; `:2287` `ConfigureWindow` redirect.
- `dix/events.c:2554` `MaybeDeliverEventsToClient()` — o estrangulamento.
- `dix/events.c:4470` `AtMostOneClient`/`ManagerMask`; `:4548` `EventSelectForWindow()` (regra BadAccess `:4567`).
- `dix/window.c:2502` `ReparentWindow()`.
- `dix/events.c:4524` `OtherClientGone()` — limpeza da seleção de máscara.

**Janelas / clipping / desenho**
- `include/windowstr.h` `WindowRec` — árvore, `clipList`, `borderClip`, `redirectDraw`.
- `mi/mivaltree.c:196` `miComputeClips()`; visibilidade `:244`.
- `dix/dispatch.c` `ProcPolyFillRectangle` — vtable `pGC->ops->PolyFillRect`.

**Input**
- `mi/mieq.c:88` `miEventQueue`; `mieqEnqueue`/`mieqProcessInputEvents`.
- `dix/events.c:2886` `DeliverDeviceEvents()` (propagação `:2929`); `:3053` `XYToWindow()`.
- `dix/events.c:4879` `SetInputFocus()`; `:4202` `DeliverFocusedEvent()`.
- `dix/events.c:1612/1731` `ActivatePointerGrab`/`ActivateKeyboardGrab`; `:4053` `CheckPassiveGrabsOnWindow()`.

**DDX / present**
- `hw/xfree86/drivers/modesetting/pageflip.c:182` `do_queue_flip_on_crtc()` → `drmModePageFlip`.
- `hw/xfree86/drivers/modesetting/present.c:317` `ms_present_check_flip`; `:367` `ms_present_flip`.
- `present/present_scmd.c:59` `present_check_flip`; `present/present_execute.c:102` `present_execute_copy`.

**Extensões / composite / damage**
- `dix/extension.c` `AddExtension()` — opcode `base = i + EXTENSION_BASE(128)`.
- `composite/compext.c:141` `ProcCompositeRedirectWindow`; `composite/compalloc.c:136` `compRedirectWindow`, `:610` `compAllocPixmap`.
- `composite/compoverlay.c:127` `compCreateOverlayWindow` (COW).
- `miext/damage/damage.c` `DamageRegister`/`DamageReportDamage`; `damageext/damageext.c:141` `DamageReportRawRegion`.

> Fonte: xserver master, commit `1133421bbd4a0b4064ac0e9565bde8c7034277f1` (2026-07-15),
> `git clone https://gitlab.freedesktop.org/xorg/xserver.git`.
