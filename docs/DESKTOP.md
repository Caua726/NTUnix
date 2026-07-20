# Desktop NTUnix: `ntwm`, `dispd` e `ntbar`

Este documento é o contrato da primeira geração moderna do desktop NTUnix.
O marco continua **single-output** e mantém composição em CPU. A divisão de
responsabilidade é:

| Processo | Responsabilidade |
|---|---|
| `ntwm` | política, estado por workspace, layouts, regras, binds e dispatchers |
| `dispd` | surfaces, input, cena retida, animação, efeitos e apresentação |
| `ntbar` | layer surface da barra; consulta/aciona o `ntwm` pelo IPC público |

As ideias foram comparadas com o Hyprland no commit
[`5b260fa`](https://github.com/hyprwm/Hyprland/commit/5b260faeb9e4563b9e6bd74450a97f39a9d708e9):
separação de caixa lógica/visual em `Target`, árvore binária do dwindle, damage
antes/depois de valores animados, stream de eventos com consumidor lento
limitado e geometria/zona exclusiva de layer surfaces. O código do NTUnix é uma
adaptação em C para named pipes, DIBs e render software; não é um porte da
infraestrutura Wayland/OpenGL.

## Estado e layouts

`WmState` contém nove `Workspace`s. Cada workspace guarda sua própria ordem de
clientes, foco, layout, árvore dwindle, `nmaster`, `mfact`, orientação e gaps.
Trocar de workspace não recria nem perde nenhum desses valores.

Os layouts implementam `LayoutOps`: inserir, remover, organizar, selecionar por
direção, mover, redimensionar e receber mensagem. Estados floating, fullscreen
e maximized pertencem ao cliente e não ao algoritmo.

### Dwindle

- Cada cliente tiled é uma folha.
- A inserção cria um pai no lugar da folha focada.
- A orientação inicial vem do aspecto da região.
- Cada pai guarda sua proporção, limitada a `0.10..0.90`.
- Remover uma folha promove o irmão e elimina o pai.
- `layoutmsg togglesplit` troca o eixo do pai focado.
- `layoutmsg splitratio +/-N` ajusta a proporção.

### Master

Suporta `left`, `right`, `top`, `bottom` e `center`, mais de um master,
`mfact`, inserção no início/fim e stack dividida no eixo apropriado. Em
`center`, masters ficam no centro e a stack é distribuída nos dois lados.
Cada cliente também conserva um peso relativo: resize no eixo da pilha
transfere espaço entre vizinhos sem alterar indiscriminadamente todos eles.

O foco é geométrico nas quatro direções. `move` troca os clientes; `resize`
altera o split dwindle, o `mfact` master ou o retângulo floating.

### Manipulação pelo mouse

`Alt+LMB/RMB` cria uma sessão com cliente, geometria inicial, último ponto,
modo, canto agarrado e threshold. Até o cursor percorrer `drag_threshold`
pixels, o gesto continua sendo somente um clique.

- Move tiled usa floating apenas como preview; no release a janela volta ao
  layout na região esquerda/direita/cima/baixo indicada pelo cursor.
- Resize tiled nunca troca o estado floating. Dwindle altera o ancestral que
  possui a borda agarrada; master ajusta `mfact` e/ou o peso do vizinho.
- Resize floating ancora o canto oposto, respeita mínimo de `80x60` e funciona
  pelos quatro cantos.
- A captura Win32 mantém motion/release no compositor mesmo fora da janela. Se
  ela for perdida ou o `ntwm` desconectar, a sessão é cancelada.

## Destino lógico e estado visual

`PLACE` sempre descreve o destino lógico completo. Ao aplicar um frame, o
`dispd` atualiza imediatamente:

- hit-test e foco;
- workspace e z-order;
- resize da surface/PTY;
- posição de uma janela Win32 estrangeira.

Cada surface composta também guarda `AnimatedRect {begin,current,goal}`.
Somente `current` é usado para desenhar. Assim input e política nunca aguardam
uma animação. Durante move/resize manual, `current` acompanha `goal`
imediatamente; ao soltar, a transição final de volta ao tiling pode ser animada.

Defaults:

- movimento/resize: 180 ms, ease-out cúbico;
- abrir: fade e deslocamento vertical de 8 px, 160 ms;
- fechar: ghost/fade;
- workspace: slide horizontal, 220 ms;
- borda/foco: 120 ms.

Enquanto há movimento, o compositor cobre o retângulo antigo e o novo com um
frame completo. Fora disso, mantém damage por surface. Blur usa passe separável
com scratch reutilizado; sombras usam máscaras de falloff cacheadas. Se frames
passam repetidamente de 18 ms, a qualidade cai nesta ordem: blur, suavidade da
sombra e sombra. Após uma sequência estável ela volta gradualmente. GDI e DXGI
continuam sendo somente backends de apresentação do quadro composto em CPU.

Surfaces de terminal e apps NTUnix recebem composição e efeitos. Janelas Win32
são posicionadas, focadas e movidas entre workspaces pelo mesmo estado lógico,
mas os pixels continuam sendo apresentados pelo `win32k`; elas não recebem
blur, transparência, recorte ou sombra do compositor.

## Protocolo `ntwm` ↔ `dispd` v2

O handshake é `HELLO ntwm 2` / `WELCOME dispd 2 ...`. O `dispd` ainda aceita o
handshake v1 para clientes antigos.

Um quadro v2 é:

```text
FRAME-BEGIN 42
ANIMATIONS 1 180 160 220 120
WORKSPACE 0
WS 7 0
STATE 7 0 0 0
STYLE 7 2 7aa2f7 238 1 10 1 1
PLACE 7 12 46 1200 700 0 0
FOCUS 7
FRAME-COMMIT 42
```

Depois do swap completo, o `dispd` responde `FRAME-APPLIED 42`. Enquanto um
serial aguarda confirmação, o `ntwm` não envia passos intermediários: marca o
estado como pendente e depois declara somente o snapshot mais recente.

Após reinício de qualquer lado, `WELCOME`, `OUTPUT`, `CURWS`, `WINDOW2`,
`FOCUSED` e `SYNC` formam um snapshot completo. Grabs também têm troca
transacional: `GRABS-BEGIN`, lista de `GRAB`s e `GRABS-COMMIT`.
`POINTER-MOD` declara o modificador exato usado por LMB/RMB, evitando que o
compositor capture `Super` quando a configuração usa `Alt`, ou vice-versa.

## App protocol v2 e layers

O handshake antigo continua válido:

```text
APP-HELLO 340 150 relogio
APP-SURFACE section 340 150
APP-COMMIT
```

No v2:

```text
APP-HELLO 2 layer 0 34 0x0d 34 0 ntbar
APP-WELCOME 2 12
APP-CONFIGURE 1 section 1920 34
APP-ACK 1
APP-COMMIT 1
```

`0x0d` é `top|left|right`. O novo buffer só substitui o anterior depois do
`APP-ACK`; isso também vale para resize do output. Pixels são BGRA
premultiplicados. Roles:

- `toplevel`: entra na política/layout do `ntwm`;
- `layer`: fica na camada de componentes do desktop, com anchors, zona
  exclusiva e interatividade de teclado independente.

`ntbar` é uma layer de 34 px. Sem ela, nenhuma zona é reservada e o desktop
continua operacional. A barra interna antiga do compositor fica desligada por
padrão; `DISPD_INTERNAL_BAR=1` existe somente como fallback de diagnóstico.

Os indicadores seguem workspaces dinâmicos: a barra mostra o ativo e os
ocupados/urgentes. Um workspace vazio desaparece ao deixar de ser ativo;
`Alt+1..9` volta a criá-lo/focá-lo sob demanda, enquanto a roda percorre somente
os workspaces existentes. Não há workspaces persistentes por padrão.

## IPC público do `ntwm`

Os pipes são locais, rejeitam clientes remotos e validam PID/sessão:

- `\\.\pipe\ntunix-ntwm`: uma requisição e uma resposta;
- `\\.\pipe\ntunix-ntwm-events`: stream `evento>>dados\n`.

Exemplos:

```text
ntwmctl status
ntwmctl reload
ntwmctl workspace 3
ntwmctl movetoworkspace 2
ntwmctl layout master
ntwmctl layoutmsg "orientation center"
ntwmctl dispatch spawn "cmd.exe"
```

Eventos incluem snapshot, workspace, estado de workspaces, foco, título,
layout, cliente adicionado/removido, frame aplicado, reload e erro. O buffer do
pipe é o limite da fila; consumidor que não progride é desconectado sem
bloquear o WM.

O evento `snapshot` leva em uma única mensagem o workspace ativo, layout,
bitmasks de ocupação/urgência e título. Os eventos `workspace-state` detalham
nome e estado individual, mas a barra não depende de nove mensagens para
produzir um quadro coerente.

## Configuração

`/etc/ntwm/ntwm.conf` é INI estrito. Se qualquer seção, chave, tipo ou faixa for
inválida, **nada** do arquivo é aplicado. O estado anterior e os grabs continuam
ativos; o erro vai para log, toast e IPC.

Seções:

- `[general]`: `mod`, `layout`, `gaps_in`, `gaps_out`, `drag_threshold`;
- `[decoration]`: borda, cor ativa, opacidade, sombra, rounding, titlebar;
- `[animations]`: enable e durações;
- `[dwindle]`: proporção inicial;
- `[master]`: orientação, `nmaster`, `mfact`, política de inserção;
- `[binds]`: linhas `bind=mods, tecla, dispatcher, argumento`;
- `[workspace N]`: overrides de layout/master e nome;
- `[rule NOME]`: match e efeitos ordenados;
- `[bar]`: habilitação e altura declarada.

Rules fazem match inicial por `kind`, `title_glob`, `exe_glob` e workspace.
Quando várias casam, a última que define uma propriedade vence. Efeitos:
floating, workspace, borda, opacidade, sombra, animação e titlebar.

Compatibilidade com a seção antiga `[ntwm]` permanece para `mod`, `nmaster`,
`mfact`, `gap` e `border`.

## Atalhos padrão

| Atalho | Ação |
|---|---|
| `Alt+Enter` | terminal |
| `Alt+H/J/K/L` | foco esquerda/baixo/cima/direita |
| `Alt+Shift+H/J/K/L` | troca/move cliente |
| `Alt+Ctrl+H/J/K/L` | resize |
| `Alt+Space` | floating |
| `Alt+F` / `Alt+M` | fullscreen / maximized |
| `Alt+T` | alterna dwindle/master |
| `Alt+1..9` | troca workspace |
| `Alt+Shift+1..9` | move ao workspace |
| `Alt+Shift+R` | reload atômico |
| `Alt+LMB` | move floating; tiled usa preview e reinserção geométrica |
| `Alt+RMB` | redimensiona pelo canto sem tirar tiled do layout |

## Validação

`test/desktop-check.sh` compila os quatro binários do desktop, valida a
configuração padrão, confirma que uma configuração inválida é recusada e roda
fixtures determinísticas de dwindle, colapso, threshold, move/reinserção tiled,
resize tiled/floating pelos cantos, resize master, regras e estado por workspace
sob Wine.

Multi-output, gestos, plugins, grupos e workspaces especiais ficam fora deste
marco.
