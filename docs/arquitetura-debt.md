# Dívida arquitetural conhecida (audit #90, #123–127)

Achados de tipo `ARCH` do `docs/auditoria-dispd-ntwm-relatorio.md`. Não são bugs
pontuais — são características do desenho atual. Registrados aqui com a direção,
para não se perderem e para orientar refactors futuros.

## #123 — `Server g_srv` concentra responsabilidades demais
`src/dispd/dispd.h`: janelas, foco, input, present, filas e protocolo compartilham
um único estado global mutável.
**Direção:** quebrar em subsistemas com ownership e APIs explícitas
(`WindowStore`, `InputRouter`, `PresentPipeline`, `WmChannel`), cada um dono do
seu estado, `g_srv` virando só a cola. Refactor grande; sem mudança de
comportamento — fazer incremental, um subsistema por vez, com o smoke a cada passo.

## #124 — Gerenciamento de processos fragmentado
Serviços usam Job Objects (initd); apps/terminais usam `TerminateProcess` (dispd);
musl-nt mantém a própria tabela de filhos. Sintomas espalhados: #39, #43, #63/#64,
#70–#83 (a maioria já corrigida pontualmente).
**Direção:** uma abstração única de *process group/job/lifecycle* (criar-em-job,
matar-a-árvore, esperar, reap) usada pelos três. Grande; casa com #43 (Job por
app) e #64 (Job por terminal), que estão nos follow-ups.

## #125 — Limites fixos com semânticas incompatíveis
Caps de 64, 256, 512, 2048 ora truncam, ora perdem, ora fazem spin. Vários já
foram endurecidos (FRAMECAP #44, quota de surface #37, ADD #76, fila de apps #34).
**Direção:** limites compartilhados em um header, com política uniforme (falha
explícita + backpressure), em vez de cada sítio decidir sozinho.

## #90 / #127 — Sem orquestração única de shutdown
Nem o dispd (threads/leitoras/DIBs/filhos pty) nem o conjunto initd têm um
shutdown ordenado (parar de aceitar → drenar → matar filhos em ordem inversa de
dependência → liberar). Hoje conta com o encerramento de processo do SO.
**Direção:** um `shutdown()` no dispd (sinaliza as threads, junta, fecha handles,
mata os pty via Job) e no initd um teardown por dependência reversa. Moderado;
registrado como follow-up (não é crash — o SO recupera os recursos no exit).

## #126 — `libntposix/npx` duplica backend não consumido
Feito: marcado como EXPERIMENTAL/LEGADO no cabeçalho de `src/libntposix/npx.c`
(o runtime usa a musl-nt). Consolidar/remover quando a migração ntdll for retomada.
