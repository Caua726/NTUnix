# Auditoria estática profunda — `dispd` / `ntwm`

Data da auditoria: 2026-07-18.

A auditoria foi exclusivamente read-only. Foram lidos integralmente os 142
arquivos obrigatórios, o prompt e o closure local de runtime/configuração. Não
foram executados testes, builds, binários, Wine ou analisadores, e nenhum arquivo
foi alterado durante a auditoria.

O estado inicial observado foi:

```text
## main
?? docs/auditoria-dispd-ntwm-prompt.md
```

> Nota: este é o estado registrado no início da auditoria. O working tree recebeu
> outras alterações depois que a auditoria terminou; elas não foram descartadas
> nem modificadas durante a criação deste relatório.

## Resumo executivo

Foram encontrados **127 achados**:

| Tipo | Total |
|---|---:|
| `DEF-C` | 71 |
| `RISK` | 30 |
| `LIMIT` | 18 |
| `ARCH` | 8 |

| Gravidade | Total |
|---|---:|
| Crítica | 7 |
| Alta | 54 |
| Média | 47 |
| Baixa | 19 |

Os problemas mais destrutivos são:

- Um processo hospedado no terminal consegue corromper a memória do `dispd`
  imprimindo uma CSI com mais de 16 argumentos.
- O resize do fallback scrape pode conservar uma grade menor, atualizar as
  dimensões e escrever ou ler fora dela depois de OOM.
- O protocolo publica transações de layout truncadas quando excede `FRAMECAP` ou
  quando a cópia de um comando falha.
- O buffer de respostas do `initd` pode avançar além de 128 KiB e causar
  leitura/escrita fora do array.
- `fork()` retoma um filho mesmo quando a cópia de memória falha.
- `posix_spawn()` corrompe permanentemente os descritores do pai quando há mais
  de 64 alvos.
- Falhas de foco, reconexão, filas e I/O síncrono podem enviar entrada à janela
  errada ou congelar partes centrais do desktop.

## UI/UX

### 1. Debug visual permanentemente ligado

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Categorias:** UI/UX, diagnóstico
- **Local:** `src/dispd/input.c:337-342`
- **Fluxo:** `input_install_hook()` lê `DISPD_DEBUG`, mas força
  `g_srv.debug = 1`.
- **Impacto:** a barra sempre exibe contadores e backend internos.
- **Causa raiz:** override temporário de diagnóstico esquecido.
- **Direção:** respeitar a variável e remover o modo forçado.

### 2. Falhas importantes não possuem feedback visual

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Categorias:** UI/UX, tratamento de erro
- **Locais:** `src/dispd/dispd.c:52-68`, `src/dispd/term.c:85-93`
- **Fluxo:** criação de janela, aba ou backend falha → apenas `dispd.log` é
  atualizado → nenhum elemento aparece na tela.
- **Impacto:** o usuário pressiona um atalho e nada parece acontecer.
- **Direção:** placeholder/toast visível e retorno de erro pelo protocolo.

### 3. Barra de workspaces não é interativa

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Categorias:** UI/UX, mouse
- **Locais:** `src/dispd/compositor.c:297-368`,
  `src/dispd/dispd.c:283-299`
- **Fluxo:** a barra é desenhada, mas o mouse só faz hit-test de janelas
  virtuais.
- **Impacto:** não é possível trocar de workspace clicando na barra.
- **Direção:** regiões interativas explícitas para barra e decorações.

### 4. Clique em decoração de app vira clique no conteúdo

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Categorias:** UI/UX, mouse, apps
- **Local:** `src/dispd/input.c:297-308`
- **Fluxo:** clique na barra/borda → `cx/cy` ficam negativos → clamp para zero →
  `APP-MOUSE` é enviado.
- **Impacto:** o app pode ativar um controle que o usuário não clicou.
- **Direção:** consumir decorações antes de calcular coordenadas cliente.

### 5. Floating não possui interação direta

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Categorias:** UI/UX, layout
- **Local:** `src/ntwm/layout.c:141-155`
- **Fluxo:** o WM recalcula uma cascata fixa; não há move/resize por ponteiro.
- **Impacto:** janelas floating não podem ser arrastadas, redimensionadas ou
  fechadas pela decoração.
- **Direção:** protocolo de move/resize interativo e geometria persistente.

### 6. Console de recuperação não cede lugar ao desktop

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Categorias:** UI/UX, sessão, lifecycle
- **Local:** `src/ntsession/ntsession.c:83-103`
- **Fluxo:** `dispd` indisponível → cria console → `dispd` volta →
  `ntsession` continua em `WaitForSingleObject` até o console terminar.
- **Impacto:** o console pode permanecer indefinidamente sobre o desktop.
- **Direção:** esperar simultaneamente o processo e a saúde do `dispd`.

## Foco e entrada

### 7. Foco pode permanecer em workspace invisível

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Categorias:** foco, workspace, entrada
- **Locais:** `src/ntwm/layout.c:51-66,158-160`,
  `src/dispd/compositor.c:249-260`, `src/dispd/input.c:147-170`
- **Sequência:** destrói a única janela do workspace → `cl_remove()` não acha
  substituta local → usa `g_clients` de outro workspace → envia `FOCUS` →
  `win_focus()` aceita a janela oculta → teclado é roteado a ela.
- **Impacto:** entrada vai para uma janela que o usuário não vê.
- **Direção:** foco nulo quando não houver cliente visível e validação no
  `dispd`.

### 8. `Alt+Tab` embutido pode focar janela invisível

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Categorias:** foco, fallback
- **Local:** `src/dispd/input.c:118-134`
- **Fluxo:** sem `ntwm`, escolhe `focused->next` ou a cabeça da lista sem filtrar
  workspace/visibilidade.
- **Direção:** circular somente entre janelas visíveis do workspace atual.

### 9. Teclado enfileirado perde o destino original

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Categorias:** teclado, ordering, foco
- **Locais:** `src/dispd/input.c:23-36,137-170`
- **Fluxo:** `KeyEv` não guarda `wid` → mouse/foco pode mudar antes do drain →
  `route_key()` usa o foco novo.
- **Impacto:** a tecla chega à janela errada.
- **Direção:** capturar destino e geração no enqueue.

### 10. Keyup de atalho capturado vaza para apps

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Categorias:** atalhos, apps
- **Local:** `src/dispd/input.c:140-143,252-261`
- **Fluxo:** keydown vai ao WM e é suprimido → keyup é enfileirado para o app
  atualmente focado.
- **Impacto:** app recebe keyup órfão.
- **Direção:** registrar o consumidor de cada tecla até a soltura.

### 11. Keyup de app pode ser perdido em fila cheia

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Categorias:** teclado, fila, apps
- **Local:** `src/dispd/input.c:252-258`
- **Fluxo:** `kq_push()` do keyup é chamado sem verificar o retorno.
- **Impacto:** modificadores podem ficar presos no estado do app.
- **Direção:** reserva para releases ou backpressure explícito.

### 12. Estado `g_down` é comprometido antes do enqueue

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Categorias:** teclado, fila
- **Local:** `src/dispd/input.c:232-250`
- **Fluxo:** marca tecla como pressionada → fila falha → repetições de grab são
  suprimidas como se o primeiro evento tivesse sido entregue.
- **Direção:** confirmar a fila antes de atualizar o estado lógico.

### 13. Remoção silenciosa do hook desativa também o fallback

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Categorias:** teclado, hook, liveness
- **Locais:** `src/dispd/input.c:269-272`,
  `src/dispd/dispd.c:270-279`
- **Fluxo:** Windows remove o low-level hook → `g_hook` continua não nulo →
  `root_proc` não chama o fallback.
- **Impacto:** teclado para de funcionar.
- **Direção:** Raw Input ou monitoramento/reinstalação real do hook.

### 14. `ToUnicodeEx` é chamado também no keyup

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Categorias:** Unicode, dead keys, layouts
- **Local:** `src/dispd/input.c:151-170`
- **Fluxo:** tradução usa estado atual durante o drain tanto para down como up.
- **Impacto:** dead keys e AltGr podem consumir/produzir composição na fase
  errada.
- **Direção:** traduzir somente keydowns apropriados com snapshot completo.

### 15. Estado de teclado passado à tradução é incompleto

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Categorias:** layouts de teclado
- **Local:** `src/dispd/input.c:151-160`
- **Problema:** só representa Shift, Ctrl, AltGr e Caps Lock; Num Lock, estados
  laterais e flags extended não são preservados.
- **Direção:** usar `GetKeyboardState` e conservar flags do hook.

### 16. Sequências especiais ignoram modos do terminal

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Categorias:** teclado, terminal
- **Local:** `src/dispd/input.c:68-115`
- **Fluxo:** setas/Home/End sempre viram CSI fixa, sem consultar DECCKM no
  libvterm.
- **Impacto:** aplicações em application-cursor mode recebem sequência errada.
- **Direção:** usar a codificação de teclado do libvterm.

### 17. Atalhos internos aceitam modificadores extras

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Categorias:** atalhos
- **Local:** `src/dispd/input.c:176-190`
- **Fluxo:** testes por `mods &` aceitam combinações adicionais.
- **Direção:** comparar combinações exatas.

### 18. Mouse não possui capture por botão

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Categorias:** mouse, apps
- **Local:** `src/dispd/input.c:276-318`
- **Fluxo:** down em A → cursor entra em B → up refaz hit-test e é entregue a B.
- **Impacto:** A fica acreditando que o botão continua pressionado.
- **Direção:** capturar `wid` no down até o último release.

### 19. Coordenadas de app não são limitadas no máximo

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Categorias:** mouse, geometria
- **Local:** `src/dispd/input.c:297-308`
- **Problema:** há clamp inferior, mas não a `cw/ch`.
- **Impacto:** apps recebem coordenadas fora da superfície.
- **Direção:** clip completo e indicação de leave/outside.

### 20. Hover puro é descartado

- **Tipo:** `LIMIT`
- **Gravidade:** baixa
- **Categorias:** apps, mouse
- **Local:** `src/dispd/input.c:302-307`
- **Impacto:** tooltips e estados hover não funcionam.
- **Direção:** coalescer motions em vez de eliminá-los.

### 21. Modificadores do mouse são perdidos no terminal

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Categorias:** terminal, mouse
- **Local:** `src/dispd/vt.c:268-289`
- **Problema:** todos os eventos usam `VTERM_MOD_NONE`.
- **Direção:** propagar Shift/Ctrl/Alt do evento original.

## Workspaces e layout

### 22. Clamp de gaps considera somente altura

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/ntwm/layout.c:92-134`
- **Cenário:** saída estreita com `gap=200` → stack começa fora da área e largura
  é reduzida artificialmente a 1 px.
- **Direção:** limitar gaps por ambas as dimensões e número de colunas.

### 23. Foco de floating não atualiza z-order

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Locais:** `src/ntwm/layout.c:141-155,163-186`
- **Fluxo:** `focusstack()` envia apenas `FOCUS`; z elevado só é recalculado em
  `send_frame()`.
- **Impacto:** janela focada pode continuar coberta.
- **Direção:** atualizar foco e z atomicamente.

### 24. Cascata floating satura no canto

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Local:** `src/ntwm/layout.c:141-155`
- **Impacto:** todas as janelas posteriores ocupam a mesma geometria.
- **Direção:** wrap, algoritmo espacial ou geometria persistente.

### 25. `zoom()` em floating aparenta não funcionar

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Local:** `src/ntwm/layout.c:232-247`
- **Fluxo:** cliente é movido à cabeça, mas continua excluído do tiling.
- **Direção:** rejeitar floating ou convertê-lo para tiled.

### 26. `togglefloating()` viola atomicidade

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/ntwm/layout.c:255-261`
- **Fluxo:** envia `FLOAT` fora da transação e só depois envia o frame.
- **Impacto:** falha entre escritas deixa flag e geometria divergentes.
- **Direção:** incluir `FLOAT` no mesmo frame.

### 27. Movimento pendente é apagado sem confirmação

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/ntwm/layout.c:70-84`
- **Fluxo:** `wm_send(WS)` → resultado ignorado → `g_move_pending=0`.
- **Impacto:** `ntwm` e `dispd` podem discordar permanentemente.
- **Direção:** ACK/serial ou retry até confirmação.

### 28. `mod` inválido vira Alt silenciosamente

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Local:** `src/ntwm/ntwm.c:62-64`
- **Direção:** aceitar somente `alt|win` e reportar inválidos.

### 29. Geometria floating não sobrevive a relayout/restart

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Local:** `src/ntwm/layout.c:141-155`
- **Problema:** snapshot conserva apenas o booleano `floating`; posição e tamanho
  são recalculados.
- **Direção:** incluir geometria no modelo e snapshot.

## Superfícies de apps

### 30. Apps não recebem resize/configure

- **Tipo:** `LIMIT`
- **Gravidade:** alta
- **Local:** `src/dispd/compositor.c:215-218`
- **Fluxo:** layout muda tile → `win_set_client_size()` retorna para `WK_APP` →
  surface permanece fixa.
- **Impacto:** conteúdo cortado ou área vazia.
- **Direção:** `CONFIGURE(serial,w,h)` e troca confirmada de buffer.

### 31. Surface compartilhada não possui sincronização

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Locais:** `src/dispd/appsrv.c:275-302`,
  `src/dispd/compositor.c:526-537`
- **Fluxo:** app escreve no mesmo DIB que o compositor lê.
- **Impacto:** tearing e frames misturando pixels antigos/novos.
- **Direção:** front/back buffers com swap atômico.

### 32. `APP-COMMIT` pode ser perdido

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/appsrv.c:308-312`
- **Problema:** retorno de `aq_push()` é ignorado.
- **Direção:** coalescer dirty por conexão ou backpressure.

### 33. Drain de apps não possui budget

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/appsrv.c:174-210`
- **Impacto:** muitos commits atrasam input, WM e present no main thread.
- **Direção:** budget e coalescing.

### 34. CREATE/DESTROY podem fazer spin infinito

- **Tipo:** `RISK`
- **Gravidade:** média
- **Local:** `src/dispd/appsrv.c:66-72`
- **Cenário:** fila cheia e main bloqueado → worker dorme/tenta para sempre.
- **Direção:** evento de espaço, cancelamento e estado de shutdown.

### 35. Framing e verbos do protocolo de apps são frouxos

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/appsrv.c:241-315`
- **Problemas:** `ERROR_MORE_DATA` não é remontado; `APP-HELLOx`,
  `APP-COMMIT-anything` e `APP-CLOSE...` são aceitos por prefixo.
- **Direção:** remontar mensagem completa e tokenizar verbo exato.

### 36. Clientes inválidos podem esgotar as 32 conexões

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/dispd/appsrv.c:214-260`
- **Fluxo:** cliente envia junk antes de cada timeout → nunca completa HELLO →
  conserva o slot.
- **Direção:** deadline absoluto de handshake.

### 37. Quota de surfaces permite cerca de 512 MiB

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Locais:** `src/dispd/appsrv.c:39-40,270-281`,
  `etc/units/dispd.service:8`
- **Problema:** 32 × 2048² × 4 bytes, enquanto `MemoryMax=128M`.
- **Direção:** orçamento agregado de bytes.

### 38. Entrada para app pode bloquear o compositor por um segundo

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/dispd/appsrv.c:111-143`
- **Fluxo:** main envia input → app não lê → espera timeout de 1 s.
- **Direção:** escritor assíncrono por conexão.

### 39. Pedido cooperativo é seguido por kill imediato

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/dispd/appsrv.c:158-170`
- **Impacto:** app não tem tempo para salvar dados.
- **Direção:** grace period, ACK e kill apenas no timeout.

### 40. PID do cliente pode ficar zero

- **Tipo:** `RISK`
- **Gravidade:** média
- **Local:** `src/dispd/appsrv.c:222-225`
- **Problema:** retorno de `GetNamedPipeClientProcessId()` é ignorado.
- **Impacto:** fechar a janela pode não encerrar o processo.
- **Direção:** rejeitar conexão sem identidade confirmada.

### 41. Conclusão do accept não é verificada

- **Tipo:** `RISK`
- **Gravidade:** baixa
- **Local:** `src/dispd/appsrv.c:345-356`
- **Problema:** depois do wait, define `con=TRUE` sem validar wait ou
  `GetOverlappedResult`.
- **Direção:** validar todos os estados e cancelamentos.

### 42. Log afirma que appsrv está ouvindo mesmo sem thread

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Local:** `src/dispd/appsrv.c:365-371`
- **Direção:** log de sucesso condicionado e propagação da falha.

### 43. Fechamento não controla a árvore do app

- **Tipo:** `RISK`
- **Gravidade:** média
- **Local:** `src/dispd/appsrv.c:158-170`
- **Problema:** apenas o PID que conectou é terminado; descendentes ou holders de
  handle podem sobreviver.
- **Direção:** Job Object por app.

## Protocolo `dispd` ↔ `ntwm`

### 44. `FRAMECAP` permite publicar frame parcial

- **Tipo:** `DEF-C`
- **Gravidade:** crítica
- **Local:** `src/dispd/wmproto.c:312-327`
- **Sequência:** BEGIN → mais de 512 comandos → extras só marcam overflow →
  COMMIT no mesmo drain → primeiros 512 são aplicados.
- **Impacto:** corrupção do estado global de layout.
- **Direção:** invalidar toda a transação e impedir commit.

### 45. OOM em `frame_push()` também publica frame parcial

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/dispd/wmproto.c:318-326`
- **Fluxo:** `_strdup` falha → apenas aquele comando some → COMMIT aplica os
  demais.
- **Direção:** `frame_failed` abortando o quadro inteiro.

### 46. `RESYNC` não descarta comandos antigos

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/dispd/wmproto.c:415-444`
- **Fluxo:** trata overflow e envia `RESYNC` → fila antiga permanece → main aplica
  comandos anteriores à perda.
- **Direção:** descartar geração pendente ou usar serial de snapshot.

### 47. Reset da conexão antiga pode apagar HELLO da nova

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Locais:** `src/dispd/wmproto.c:402-418,474-526`
- **Sequência:** reader encerra geração A e marca reset → aceita B e enfileira
  HELLO → main executa `q_clear()` global → HELLO de B desaparece.
- **Impacto:** novo WM espera para sempre.
- **Direção:** reset vinculado à geração encerrada.

### 48. Timeout de escrita não libera o único slot

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/dispd/wmproto.c:100-124`
- **Fluxo:** timeout → `g_connected=0` → reader e instância continuam presos ao
  cliente antigo.
- **Impacto:** nenhum WM novo consegue conectar.
- **Direção:** teardown completo da geração no timeout.

### 49. Primeiros bytes removem o deadline para sempre

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/dispd/wmproto.c:480-520`
- **Cenário:** cliente envia junk uma vez, não faz HELLO e prende a única
  instância em reads infinitos.
- **Direção:** deadline absoluto para HELLO e heartbeat posterior.

### 50. Servidor WM não possui teardown

- **Tipo:** `ARCH`
- **Gravidade:** baixa
- **Local:** `src/dispd/wmproto.c:531-558`
- **Problema:** reader, eventos, pipe e critical section vivem até o processo
  terminar.
- **Direção:** objeto servidor com start/stop/join.

### 51. Snapshot contém no máximo 256 janelas

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/dispd/wmproto.c:209-218`
- **Impacto:** após restart, janelas posteriores desaparecem do modelo do WM.
- **Direção:** snapshot paginado/streaming sem truncamento silencioso.

### 52. Compositor e hit-test discordam acima de 256 janelas

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Locais:** `src/dispd/compositor.c:444-456,262-275`
- **Fluxo:** composição ignora janela 257+ → hit-test ainda a encontra.
- **Impacto:** janela invisível captura foco/mouse.
- **Direção:** modelo único de visibilidade e z.

### 53. Budget conta mensagens, não comandos

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/wmproto.c:430-444`
- **Problema:** cada mensagem pode conter milhares de linhas processadas sem
  budget interno.
- **Direção:** contar comandos/bytes e continuar no próximo frame.

### 54. Erro de versão é descartado

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Local:** `src/dispd/wmproto.c:357-376`
- **Fluxo:** chama `wm_send(ERR)` antes de `g_connected=1`.
- **Direção:** resposta de handshake independente do estado pronto.

### 55. Gramática transacional é permissiva

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/wmproto.c:382-400`
- **Problemas:** COMMIT sem BEGIN, BEGIN aninhado e verbos de frame fora da
  transação são aceitos.
- **Direção:** máquina de estados estrita.

### 56. `TITLEBAR` é definido, mas não implementado

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Locais:** `src/common/ntuwm.h:54`,
  `src/dispd/wmproto.c:300`
- **Direção:** implementar ou remover do protocolo.

### 57. OOM na fila WM perde comando sem resync

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/dispd/wmproto.c:57-74`
- **Problema:** falha de `_strdup` retorna zero sem marcar overflow.
- **Direção:** invalidar geração/layout.

### 58. Cliente `ntwm` ignora erro e quantidade escrita

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/ntwm/proto.c:33-48`
- **Impacto:** modelo do WM avança quando o `dispd` não recebeu o comando.
- **Direção:** retorno obrigatório, desconexão e resnapshot.

### 59. Escrita síncrona pode travar o único thread do WM

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/ntwm/proto.c:33-48`
- **Impacto:** o WM deixa de ler eventos e responder.
- **Direção:** writer overlapped/assíncrono.

### 60. Mensagem grande é dividida em mensagens falsas

- **Tipo:** `RISK`
- **Gravidade:** baixa
- **Local:** `src/ntwm/proto.c:50-69`
- **Fluxo:** buffer de 8192 enche → retorna parte → resto é lido na próxima
  chamada e interpretado como nova mensagem.
- **Direção:** buffer dinâmico ou descarte explícito do restante.

### 61. OOM em `cl_add()` não provoca resync

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Locais:** `src/ntwm/layout.c:33-48`,
  `src/ntwm/ntwm.c:168-176`
- **Impacto:** janela existe no `dispd`, mas nunca entra no layout do WM.
- **Direção:** abortar o modelo e reconectar/resnapshot.

### 62. Não existe heartbeat

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Problema:** WM conectado, mas parado, não é substituído até alguma escrita
  disparar timeout.
- **Direção:** ping/ack e lease de conexão.

## Concorrência e lifecycle

### 63. Reap de abas usa somente a aba ativa

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/dispd/dispd.c:176-190`
- **Fluxo:** aba morta em background não é removida → ao ativá-la, `reap_dead`
  destrói a janela inteira e todas as abas vivas.
- **Direção:** reap e fechamento por aba.

### 64. Close de terminal mata só o shell

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Locais:** `src/dispd/term_conpty.c:217-220`,
  `src/dispd/term_pty.c:220-241`, `src/dispd/term_scrape.c:208-227`
- **Impacto:** descendentes sobrevivem sem janela.
- **Direção:** Job Object por terminal.

### 65. Input de terminal bloqueia o main thread

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Locais:** `src/dispd/term_conpty.c:169-178`,
  `src/dispd/term_pty.c:198-207`
- **Fluxo:** pipe cheio → `WriteFile` síncrono bloqueia → teclado, WM, apps e
  present param.
- **Direção:** fila de escrita assíncrona.

### 66. Fallback pode herdar libvterm de tentativa fracassada

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Locais:** `src/dispd/term.c:85-93`,
  `src/dispd/term_pty.c:172-195`,
  `src/dispd/term_conpty.c:144-166`
- **Fluxo:** libvterm inicia → criação do reader falha → backend faz rollback
  parcial → scrape inicia com `t->vt` ainda ativo.
- **Impacto:** grade scrape não é renderizada e mouse segue caminho errado.
- **Direção:** rollback completo entre tentativas.

### 67. Teardown contém joins infinitos

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Locais:** `src/dispd/term_conpty.c:209-212`,
  `src/dispd/term_pty.c:228-232`,
  `src/dispd/term_scrape.c:216-218`
- **Impacto:** driver/I/O que não conclui deixa o main preso.
- **Direção:** overlapped, cancelamento confirmado e timeout final seguro.

### 68. Títulos de abas inativas têm data race

- **Tipo:** `RISK`
- **Gravidade:** média
- **Locais:** `src/dispd/compositor.c:500-503`,
  `src/dispd/vt.c:63-77`
- **Fluxo:** compositor lê `tt->title` sem lock enquanto reader escreve OSC.
- **Direção:** copiar cada título sob seu lock.

### 69. `volatile` é usado como sincronização

- **Tipo:** `RISK`
- **Gravidade:** baixa
- **Local:** `src/dispd/term.h:58-60`
- **Problema:** `dirty`/`alive` misturam acessos comuns e `Interlocked`.
- **Direção:** atomics explícitos ou lock consistente.

### 70. Falha ao criar watcher não falha o spawn

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/initd/service.c:263-272`
- **Impacto:** serviço fica “running” sem detecção de saída/restart.
- **Direção:** watcher como parte transacional do spawn.

### 71. `ResumeThread` não é verificado

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/initd/service.c:260`
- **Impacto:** processo suspenso, serviço running e watcher esperando para sempre.
- **Direção:** abortar e limpar o job/processo.

### 72. Job pode continuar sem `KILL_ON_JOB_CLOSE`

- **Tipo:** `RISK`
- **Gravidade:** média
- **Local:** `src/initd/service.c:203-218`
- **Fluxo:** duas tentativas de `SetInformationJobObject` falham → código continua.
- **Direção:** segunda falha deve abortar.

### 73. `Requires=` desconhecido é ignorado

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/initd/service.c:346-360`
- **Impacto:** serviço inicia sem requisito obrigatório.
- **Direção:** falhar o start; separar `After=` de `Requires=`.

### 74. Cliente mudo bloqueia o controle do `initd`

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/initd/pipesrv.c:197-226`
- **Fluxo:** única instância → `ReadFile` bloqueante sem timeout.
- **Impacto:** todo `ntctl`, inclusive shutdown, fica indisponível.
- **Direção:** overlapped e deadline.

### 75. Cliente que não lê bloqueia resposta do `initd`

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/initd/pipesrv.c:221-225`
- **Problema:** `WriteFile` e `FlushFileBuffers` são bloqueantes.
- **Direção:** resposta limitada/overlapped e cancelamento.

### 76. Macro `ADD` avança além do buffer

- **Tipo:** `DEF-C`
- **Gravidade:** crítica
- **Local:** `src/initd/pipesrv.c:14-18`
- **Fluxo:** `snprintf` trunca, mas retorna tamanho desejado → `g_len` ultrapassa
  o array → próximo ponteiro e `WriteFile` usam tamanho fora do buffer.
- **Impacto:** crash, memória exposta ou corrupção.
- **Direção:** saturar contador e retornar erro de truncamento.

### 77. `calloc(Service)` não é verificado

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/initd/service.c:138-146`
- **Fluxo:** OOM → `s=NULL` → `memcpy(NULL,...)`.
- **Direção:** tratar ENOMEM antes da inserção.

### 78. Health check de `ntsession` rouba o pipe do WM

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `src/ntsession/ntsession.c:25-33,83-94`
- **Sequência:** pipe WM livre → probe conecta e fecha sem HELLO → reader cria
  geração/reset → `ntwm` disputa o único slot.
- **Direção:** pipe de health separado ou PING válido não exclusivo.

### 79. `fork()` ignora falhas de cópia/suspend/resume

- **Tipo:** `RISK`
- **Gravidade:** crítica
- **Local:** `musl-nt/nt/sys_proc.c:354-406,474-490`
- **Fluxo:** regiões falham → contador é ignorado → filho retoma com memória
  parcial.
- **Direção:** abortar ao primeiro erro e validar bytes copiados.

### 80. `posix_spawn()` corrompe o pai após 64 alvos

- **Tipo:** `DEF-C`
- **Gravidade:** crítica
- **Local:** `musl-nt/override/posix_spawn.c:43-80,115-125`
- **Fluxo:** apenas 64 FDs são salvos → ações posteriores ainda os modificam →
  não há restauração.
- **Direção:** journal dinâmico ou erro antes de qualquer mutação.

### 81. Erros das file actions são ignorados

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `musl-nt/override/posix_spawn.c:81-109`
- **Problema:** erros de `close`, `dup3`, `openat`, `chdir` e `fchdir` não
  interrompem o spawn.
- **Impacto:** filho nasce com redirects errados e pai pode permanecer alterado.
- **Direção:** journal transacional e rollback.

### 82. `posix_spawn` altera estado global sem excluir outros threads

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `musl-nt/override/posix_spawn.c:50-125`
- **Problema:** CWD e FDs do pai são modificados temporariamente.
- **Direção:** lista explícita de handles/atributos sem mutar o pai.

### 83. Tabela cheia devolve PID não aguardável

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `musl-nt/nt/sys_proc.c:20-35,214-269`
- **Fluxo:** `child_add` fecha handle na tabela cheia → spawn ainda retorna PID →
  `waitpid` retorna `ECHILD`.
- **Direção:** reservar slot antes de criar o processo.

### 84. `npx_open` vaza handle quando a tabela está cheia

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/libntposix/npx.c:32-42,53-76`
- **Direção:** rollback explícito de ownership.

## Renderização GDI/DXGI

### 85. Resolução, DPI e monitores não são atualizados

- **Tipo:** `LIMIT`
- **Gravidade:** alta
- **Locais:** `src/dispd/dispd.c:265-307,344-349`
- **Problema:** métricas do monitor primário são lidas uma vez; não há
  `WM_SIZE`, `WM_DISPLAYCHANGE` ou `WM_DPICHANGED`.
- **Direção:** modelo de outputs e resize transacional de todos os buffers.

### 86. Device loss não possui recuperação

- **Tipo:** `LIMIT`
- **Gravidade:** alta
- **Local:** `src/dispd/present_dxgi.c:136-180`
- **Fluxo:** removed/reset → `dead=1` → todo present futuro retorna.
- **Impacto:** desktop visualmente congelado.
- **Direção:** recriar device/swapchain ou migrar para GDI.

### 87. Falhas de apresentação são silenciosas

- **Tipo:** `RISK`
- **Gravidade:** média
- **Locais:** `src/dispd/present_gdi.c:26-32`,
  `src/dispd/present_dxgi.c:136-161`
- **Problema:** `BitBlt`, `Map` e erros gerais de `Present` não geram recuperação.
- **Direção:** diagnóstico, retry e fallback.

### 88. Blend usa divisão por 256

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Local:** `src/dispd/compositor.c:421-435`
- **Impacto:** canais ficam levemente escurecidos.
- **Direção:** arredondamento/divisão por 255.

### 89. Títulos UTF-8 são desenhados com API ANSI

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/compositor.c:355-362,500-522`
- **Impacto:** títulos não ASCII viram mojibake.
- **Direção:** UTF-8 → UTF-16 e `DrawTextW`.

### 90. `dispd` não possui shutdown ordenado

- **Tipo:** `ARCH`
- **Gravidade:** baixa
- **Local:** `src/dispd/dispd.c:420-436`
- **Problema:** retorna sem destruir backend, compositor, fonte, hook e servidores.
- **Direção:** sequência explícita de stop/join/destroy.

## Terminal/VT

### 91. Overflow controlável do array de argumentos CSI

- **Tipo:** `DEF-C`
- **Gravidade:** crítica
- **Locais:** `third_party/libvterm/src/vterm_internal.h:24,211-218`,
  `third_party/libvterm/src/parser.c:220-236`
- **Fluxo:** cada `;` incrementa `argi` sem limite → escreve além de
  `args[16]`.
- **Impacto:** processo no terminal pode corromper memória do `dispd`.
- **Direção:** validar antes de incrementar e atualizar o vendor.

### 92. Argumentos CSI/OSC podem sofrer signed overflow

- **Tipo:** `RISK`
- **Gravidade:** média
- **Local:** `third_party/libvterm/src/parser.c:220-226,261-269`
- **Direção:** saturação e limite de dígitos.

### 93. OOM durante resize scrape causa OOB

- **Tipo:** `RISK`
- **Gravidade:** crítica
- **Locais:** `src/dispd/vt.c:217-243`,
  `src/dispd/term_scrape.c:69-78`, `src/dispd/vt.c:362-364`
- **Fluxo:** `calloc` falha → grade velha permanece → `cols/rows` novos são
  publicados → reader escreve e renderer copia usando tamanho maior.
- **Direção:** só publicar dimensões após alocação bem-sucedida.

### 94. Inicialização do libvterm não trata OOM

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Locais:** `third_party/libvterm/src/vterm.c:51-80`,
  `third_party/libvterm/src/state.c:59-93`,
  `third_party/libvterm/src/screen.c:869-906`
- **Problema:** resultados de alocação são desreferenciados sem checagem.
- **Direção:** APIs falíveis com rollback.

### 95. Resize/crescimento do libvterm não trata OOM

- **Tipo:** `RISK`
- **Gravidade:** crítica
- **Locais:** `third_party/libvterm/src/screen.c:519-528,761-803`,
  `third_party/libvterm/src/state.c:1977-2035`
- **Impacto:** escrita por NULL ou substituição de buffer válido.
- **Direção:** alocar tudo, validar e só então fazer swap.

### 96. Buffer de reply VT descarta fragmentos inteiros

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/vt.c:36-43`
- **Impacto:** aplicação aguardando resposta pode travar.
- **Direção:** buffer dinâmico/fila de saída.

### 97. Combining marks e caracteres suplementares são perdidos

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/vt.c:329-360,390-401`
- **Problema:** usa só `chars[0]`; acima do BMP vira U+FFFD.
- **Direção:** clusters completos e surrogate pairs.

### 98. Render força uma célula por glifo

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/vt.c:390-401`
- **Impacto:** CJK/emoji podem sobrepor a célula seguinte.
- **Direção:** render por clusters e largura declarada.

### 99. Shape e blink do cursor são ignorados

- **Tipo:** `LIMIT`
- **Gravidade:** baixa
- **Locais:** `src/dispd/vt.c:63-86,414-416`
- **Direção:** armazenar propriedades e temporizador real de blink.

### 100. Scrape corrompe Unicode nos dois sentidos

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/term_scrape.c:39-84,168-189`
- **Problema:** `ReadConsoleOutputA` e `WriteConsoleInputA` tratam bytes da
  codepage/UTF-8 como caracteres independentes.
- **Direção:** APIs `W` e conversão UTF-16/UTF-8.

### 101. Falha de `CONIN$` não aborta o backend

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/dispd/term_scrape.c:126-133`
- **Impacto:** terminal inicia visível, mas sem teclado.
- **Direção:** validar `CONIN$` e `CONOUT$`.

### 102. Backend scrape não controla ownership do console

- **Tipo:** `RISK`
- **Gravidade:** média
- **Local:** `src/dispd/term_scrape.c:113-118,208-227`
- **Fluxo:** falha de `AllocConsole` é ignorada; close sempre chama
  `FreeConsole`.
- **Direção:** registrar se o backend realmente criou o console.

### 103. Scrape lê toda a grade a 30 Hz mesmo oculto

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `src/dispd/term_scrape.c:87-99`
- **Direção:** pausar/coalescer quando janela/aba estiver invisível.

### 104. `poll()` sempre declara PTY legível

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `musl-nt/nt/sys_file.c:465-491`
- **Fluxo:** `NT_FD_PTY` cai no caso de arquivo comum → `POLLIN` imediato →
  caller chama `read` e bloqueia.
- **Direção:** `PeekNamedPipe` também para PTY.

### 105. `O_NONBLOCK` é ignorado no modo canônico

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `musl-nt/nt/sys_pty.c:146-165`
- **Direção:** verificar disponibilidade antes do loop canônico.

### 106. Linhas canônicas acima de 4095 bytes são truncadas

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `musl-nt/nt/sys_pty.c:140-144,214-229`
- **Impacto:** perda silenciosa de dados.
- **Direção:** buffer expansível ou erro/feedback.

### 107. PTY não possui job control

- **Tipo:** `LIMIT`
- **Gravidade:** alta
- **Local:** `musl-nt/nt/sys_pty.c:14-17,175-190`
- **Problema:** sinais só atingem o processo que está lendo, não o foreground
  process group.
- **Impacto:** `Ctrl+C` não interrompe corretamente comandos filhos.

### 108. `VMIN`/`VTIME` são armazenados, mas ignorados

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Local:** `musl-nt/nt/sys_pty.c:25-42,119-137`
- **Direção:** implementar as quatro combinações POSIX.

### 109. Resize não envia `SIGWINCH`

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Local:** `src/dispd/term_pty.c:209-218`
- **Direção:** sinalizar o foreground process group.

### 110. Mapping de winsize pode falhar ou colidir

- **Tipo:** `RISK`
- **Gravidade:** média
- **Local:** `src/dispd/term_pty.c:113-124`
- **Problemas:** `MapViewOfFile` pode falhar sem abortar; nome PID+contador é
  previsível e colisão não é verificada.
- **Direção:** mapping anônimo duplicado ou nome forte com verificação.

### 111. Herança ampla de handles

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/dispd/term_pty.c:126-157`
- **Fluxo:** falha de `SetHandleInformation` é ignorada e
  `CreateProcess(..., TRUE, ...)` herda todos os handles herdáveis.
- **Impacto:** EOF impedido e recursos indevidos no filho.
- **Direção:** `PROC_THREAD_ATTRIBUTE_HANDLE_LIST`.

### 112. Handle do mapping slave é vazado

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Local:** `musl-nt/nt/sys_pty.c:56-69`
- **Problema:** abre `m`, cria a view e nunca fecha o handle.
- **Direção:** fechar depois de `MapViewOfFile`.

## Desempenho

### 113. Não há damage tracking no compositor

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Local:** `src/dispd/compositor.c:439-544`
- **Problema:** qualquer dirty recompõe wallpaper e todas as janelas visíveis.
- **Direção:** regiões de dano e occlusion tracking.

### 114. Terminal dirty redesenha a grade completa

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Local:** `src/dispd/vt.c:314-420`
- **Problema:** regiões de damage do libvterm são descartadas.
- **Direção:** conservar dirty rects.

### 115. Repaint periódico não produz o blink declarado

- **Tipo:** `DEF-C`
- **Gravidade:** baixa
- **Locais:** `src/dispd/dispd.c:239-253`,
  `src/dispd/vt.c:414-416`
- **Problema:** há repaint periódico “para cursor”, mas o cursor nunca alterna.
- **Direção:** temporizadores separados e invalidation por componente.

## Segurança

### 116. Pipe do WM não autentica o controlador

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/dispd/wmproto.c:542-558`
- **Problema:** DACL padrão, sem validação de token/PID; cliente admitido pode
  usar `QUIT`, `SPAWN-TERM`, `CLOSE`, `GRAB` e layout.
- **Direção:** ACL da sessão e autenticação do WM.

### 117. Pipe do `initd` não autoriza comandos privilegiados

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/initd/pipesrv.c:121-195,197-228`
- **Problema:** cliente admitido pela DACL pode executar STOP/RESTART/SHUTDOWN.
- **Direção:** ACL explícita e token checks.

### 118. Pipes e mappings de apps dependem da DACL padrão

- **Tipo:** `RISK`
- **Gravidade:** alta
- **Local:** `src/dispd/appsrv.c:275-287,338-342`
- **Problema:** nome com salt não substitui controle de acesso.
- **Direção:** descriptor restrito ao servidor e SID do cliente.

### 119. Imagem cria administrador com senha fixa e autologon

- **Tipo:** `DEF-C`
- **Gravidade:** alta
- **Local:** `build/autounattend.xml:25-39`
- **Problema:** usuário `ntunix`, grupo Administrators, senha literal `ntunix`,
  autologon quase infinito.
- **Direção:** senha fornecida no build/OOBE, usuário não administrador e remoção
  do autologon.

### 120. Requisitos de segurança da plataforma são ignorados

- **Tipo:** `LIMIT`
- **Gravidade:** média
- **Local:** `build/autounattend.xml:43-63`
- **Problema:** bypass de TPM, Secure Boot e RAM é obrigatório.
- **Direção:** opção explícita.

### 121. Check de itens perigosos em `strip.list` sempre passa

- **Tipo:** `DEF-C`
- **Gravidade:** média
- **Local:** `test/build-check.sh:46-50`
- **Fluxo:** `grep -q ... | grep -v` → o primeiro grep não emite linhas → o
  segundo sempre falha → caminho “conservador”.
- **Direção:** filtrar comentários antes ou remover `-q`.

## Qualidade arquitetural e gambiarras

### 122. Header referencia `config.c` inexistente

- **Tipo:** `ARCH`
- **Gravidade:** baixa
- **Local:** `src/ntwm/ntwm.h:52-53`
- **Problema:** implementação real está em `ntwm.c`.
- **Direção:** corrigir documentação ou separar o módulo.

### 123. `Server g_srv` concentra responsabilidades demais

- **Tipo:** `ARCH`
- **Gravidade:** média
- **Local:** `src/dispd/dispd.h:57-109`
- **Problema:** janelas, foco, input, present, filas e protocolo compartilham um
  estado global mutável.
- **Direção:** subsistemas com ownership e APIs explícitas.

### 124. Gerenciamento de processos é fragmentado

- **Tipo:** `ARCH`
- **Gravidade:** alta
- **Problema:** serviços usam Job Objects; apps e terminais usam
  `TerminateProcess`; musl mantém outra tabela de filhos.
- **Sintomas:** achados 39, 43, 63, 64 e 70-83.
- **Direção:** abstraction única de process group/job/lifecycle.

### 125. Limites fixos possuem semânticas incompatíveis

- **Tipo:** `ARCH`
- **Gravidade:** média
- **Problema:** caps de 64, 256, 512 e 2048 ora truncam, ora perdem, ora fazem
  spin.
- **Direção:** limites compartilhados, falha explícita e backpressure uniforme.

### 126. `libntposix/npx` duplica backend não consumido

- **Tipo:** `ARCH`
- **Gravidade:** baixa
- **Local:** `src/libntposix/npx.c`
- **Problema:** backend legado duplica FDs/processos já implementados em
  `musl-nt`.
- **Direção:** remover, marcar experimental ou consolidar contratos.

### 127. Não existe orquestração única de shutdown

- **Tipo:** `ARCH`
- **Gravidade:** média
- **Problema:** `dispd`, appsrv, wmproto, input e terminais dependem da limpeza do
  SO.
- **Direção:** estado global de shutdown, cancelamento, joins e destruição em
  ordem reversa.

## Verificação das invariantes

| Invariante | Veredito | Evidência/fluxo |
|---|---|---|
| No máximo uma janela realmente focada | `MANTIDA` | `win_focus()` limpa `focused` de todas antes de definir uma. |
| Janela focada pertence ao workspace visível | `VIOLADA` | Achados 7 e 8. |
| `dispd` e `ntwm` concordam sobre estado | `VIOLADA` | 26, 27, 44-49, 57, 58 e 61. |
| BEGIN...COMMIT é realmente atômico | `VIOLADA` | 44, 45, 46 e 55. |
| Janela movida desaparece imediatamente do workspace antigo | `PARCIAL` | Fluxo normal funciona; 27/58 mantêm divergência após perda. |
| Janela destruída não continua referenciada | `MANTIDA` | `win_destroy` desliga da lista antes de liberar; não foi demonstrado UAF no fluxo normal. |
| App fechado não continua rodando/commitando | `PARCIAL` | Commits cessam, mas 40/43 podem deixar processo vivo; 39 mata cedo demais. |
| Resize não desconecta app do buffer | `PARCIAL` | O buffer permanece porque o app simplesmente não é redimensionado; achado 30. |
| Buffer compartilhado possui sincronização | `VIOLADA` | Achado 31. |
| Nenhuma thread usa memória após teardown | `PARCIAL` | Joins tentam evitar UAF, mas 34/67 não garantem conclusão. |
| Fila cheia não corrompe silenciosamente o estado | `VIOLADA` | 11, 32, 44-46 e 57. |
| WM travado não bloqueia compositor/teclado | `PARCIAL` | Timeout limita uma escrita, mas 48/49 impedem novo WM; 38/65 bloqueiam o main. |
| Terminal oculto não força redraw permanente | `MANTIDA` | `frame_tick` considera somente terminal ativo/visível; scrape ainda consome CPU em 103. |
| Falhas GDI/DXGI são detectadas e recuperadas | `VIOLADA` | 86 e 87. |
| Entrada nunca vai para janela invisível/errada | `VIOLADA` | 7-10, 18 e 19. |
| Atalho capturado não vaza keyup/não duplica | `VIOLADA` | 10 e 13. |
| Unicode/layouts não americanos não são truncados | `VIOLADA` | 14-16, 21, 89, 97, 98 e 100. |
| Snapshot restaura o desktop | `VIOLADA` | 51 e 61. |
| Comandos/eventos possuem implementação coerente | `VIOLADA` | 54-56. |
| Todo estado possui dono/lifecycle consistente | `VIOLADA` | 39, 43, 63-84 e 123-127. |

## Contradições entre documentação, comentários e implementação

| Alegação | Implementação real | Achados | Arquivos |
|---|---|---:|---|
| Frames são aplicados atomicamente e nunca parciais | Cap/OOM permitem commit truncado | 44, 45 | `ntuwm.h:12-15`, `desktop-known-gaps.md:17-19`, `wmproto.c:312-327` |
| Overflow faz resync evitando estado corrompido | Fila antiga é drenada depois do `RESYNC` | 46 | `desktop-known-gaps.md:20-23`, `wmproto.c:415-444` |
| Input tem par down/up e fila infalível | Keyup capturado vaza e keyup de app pode ser descartado | 10-12 | `desktop-known-gaps.md:24-25`, `input.c:225-261` |
| Fallback de teclado fica disponível se o hook cair | Fallback depende de `g_hook == NULL` | 13 | `input.c:10-14`, `dispd.c:270-279` |
| Terminal usa fonte OEM/CP437 e não tem scrollback | Usa TTF, `ExtTextOutW` e ring de 2000 linhas | 97-99 | `vt.c:4-13`, `desktop-known-gaps.md:54-61`, `compositor.c:63-77`, `vt.c:91-116` |
| Barra/titlebars não respondem a clique | Abas de terminal são clicáveis; barra e títulos de apps não | 3, 4 | `desktop-known-gaps.md:87-91`, `input.c:282-295` |
| Default é ConPTY com scrape fallback | Default real é PTY nativo para Unix e scrape para console Windows | 66 | `term.c:1-8,77-83`, `term_conpty.c:1-6` |
| `backend_is_pty=1` significa ConPTY | PTY nativo também define o campo | 66 | `term.h:36-40`, `term_pty.c:102-109` |
| `ntsession` abre o terminal e o ressuscita | Vigia `dispd`; terminal próprio só na recuperação | 6, 78 | `README.md:38`, `ntsession.c:74-104` |
| `fork()` permanece indisponível | Existe `nt_sys_fork` e teste dedicado | 79 | `musl-nt/README.md:19,93-95,131`, `musl-nt-spec.md:219,270`, `sys_proc.c:409-492` |
| Job sempre tem `KILL_ON_JOB_CLOSE` e filho só roda após entrar nele | Segunda falha e `ResumeThread` não são tratadas | 71, 72 | `PROTOCOLO.md:56-62`, `service.c:203-218,260` |
| Opacidade default é 92% | Código usa 85% | 88 | `compositor.c:372-383` |
| Budget é comandos por tick | Budget conta mensagens, não linhas internas | 53 | `wmproto.c:27,430-444` |

## Priorização

### Impede o desktop ou função central

1. Corrigir 91 e atualizar o libvterm.
2. Corrigir 44-49: transação, overflow, reconexão e liveness do WM.
3. Remover I/O síncrono do main: 38 e 65.
4. Corrigir foco/input: 7-13 e 18.
5. Corrigir supervisão: 70, 71 e 74-78.
6. Corrigir PTY `poll`/nonblocking: 104 e 105.

### Pode causar crash ou corrupção

- Primeiro: 76, 79, 80, 91, 93 e 95.
- Depois: 45, 57, 77, 81-83, 94 e 111.
- OOM nunca deve publicar dimensões ou estado parcialmente atualizados.

### Prejudica fortemente a UX

- Resize/configure de apps e tearing: 30-32.
- Foco/z-order/floating: 7 e 22-29.
- Recovery e feedback: 2 e 6.
- Unicode, teclado e terminal: 14-21, 89 e 97-109.
- Device loss/resolução: 85-87.

### Pode esperar

- Cosméticos/manutenção: 1, 17, 25, 28, 42, 50, 54, 56, 88, 90, 99,
  112, 115, 122 e 126.
- Damage tracking: 103, 113 e 114.

## Causas raiz

| Causa raiz | Sintomas | Achados | Arquivos principais |
|---|---|---:|---|
| Falha “continua mesmo assim” | Frames truncados, grade incoerente, filho parcial | 44, 45, 57, 71, 72, 77, 79-81, 93-95 | `wmproto.c`, `service.c`, `sys_proc.c`, `posix_spawn.c`, libvterm |
| I/O síncrono em threads centrais | Freeze de compositor, WM e initd | 38, 59, 65, 74, 75 | appsrv, proto, term backends, pipesrv |
| Protocolo sem serial/ACK | Divergência e reconexão perdida | 26, 27, 44-49, 55, 58, 62 | `wmproto.c`, `layout.c`, `proto.c` |
| Evento não carrega destino/lifecycle | Tecla/release/mouse para janela errada | 7-13, 18 | `input.c`, `layout.c`, `compositor.c` |
| Processo sem grupo/Job uniforme | Descendentes órfãos e close destrutivo | 39, 43, 63, 64, 70-83, 124 | appsrv, term, initd, musl-nt |
| Surfaces fixas e sem ownership | Clipping, tearing, commits perdidos | 30-32 | `appsrv.c`, `compositor.c` |
| Limites locais inconsistentes | Snapshot invisível, hit-test fantasma, perdas | 11, 32, 37, 44, 51-53, 76, 83, 106, 125 | vários |
| Vendor sem hardening | OOB por CSI e crashes em OOM | 91-95 | `third_party/libvterm` |
| Segurança baseada no ambiente single-user | Controle de desktop/serviços e surfaces expostas | 116-120 | pipes, mappings, autounattend |
| Documentação desatualizada | Garantias falsas e diagnóstico errado | tabela de contradições | docs, comentários e fontes |

## Inventário exato de leitura

Classificações:

- `P`: runtime principal;
- `C`: closure comum;
- `S`: sessão/supervisão;
- `B`: build/configuração;
- `M`: musl/PTY/processos;
- `V`: libvterm;
- `D`: documentação;
- `T`: teste/fixture lido, mas não executado.

Todos os arquivos abaixo foram lidos da linha 1 até EOF.

```text
Makefile | 134 | integral | B | build dos componentes
README.md | 104 | integral | D | comportamento declarado
build/SetupComplete.cmd | 47 | integral | B | boot/instalação
build/autounattend.xml | 67 | integral | B | instalação/segurança
build/make-iso.sh | 120 | integral | B | imagem instalada
build/make-live.sh | 87 | integral | B | imagem WinPE
build/strip.list | 35 | integral | B | remoções da imagem
build/test-vm.sh | 27 | integral | T | VM, não executado
build/vm-setup.sh | 46 | integral | T | VM, não executado
build/winpeshl.ini | 7 | integral | B | WinPE → ntsession
docs/auditoria-dispd-ntwm-prompt.md | 484 | integral | D | manifesto da auditoria
docs/PROTOCOLO.md | 68 | integral | D | contrato do initd
docs/VISAO.md | 1133 | integral | D | arquitetura normativa
docs/desktop-known-gaps.md | 91 | integral | D | garantias/gaps
docs/musl-nt-spec.md | 386 | integral | D | contrato libc/PTY
docs/pesquisa/bb-coreutils-funcoes-libc-consumidas.txt | 242 | integral | D | BusyBox/libc
docs/pesquisa/bb-coreutils-headers-fecho-completo.txt | 477 | integral | D | closure headers
docs/pesquisa/bb-coreutils-headers-publicos.txt | 85 | integral | D | headers públicos
docs/pesquisa/busybox-coreutils-musl-deps.md | 174 | integral | D | BusyBox/musl
docs/pesquisa/comparacao-linux-vs-nt.md | 414 | integral | D | arquitetura
docs/pesquisa/deep-compositors.md | 591 | integral | D | compositor
docs/pesquisa/deep-dwm.md | 1230 | integral | D | layout ntwm
docs/pesquisa/deep-gfx-server-linux.md | 895 | integral | D | referência gráfica
docs/pesquisa/deep-gfx-server-windows.md | 462 | integral | D | GDI/DXGI
docs/pesquisa/deep-wm-architecture.md | 1404 | integral | D | invariantes WM
docs/pesquisa/deep-xorg-server.md | 788 | integral | D | display server
docs/pesquisa/nt-boot-install.md | 470 | integral | D | boot/instalação
docs/pesquisa/nt-dwm-compositor.md | 207 | integral | D | DWM/flip
docs/pesquisa/nt-native-api.md | 646 | integral | D | libntposix/NT
docs/pesquisa/nt-win11-image.md | 678 | integral | D | imagem Windows
docs/pesquisa/wm-compositor-survey.md | 554 | integral | D | survey WM
etc/group | 1 | integral | B | runtime
etc/hosts | 2 | integral | B | resolução libc
etc/mtab | 1 | integral | B | BusyBox
etc/ntwm/ntwm.conf | 12 | integral | P | configuração ntwm
etc/passwd | 1 | integral | B | identidade Unix
etc/units/demod.service | 12 | integral | S | supervisão
etc/units/dispd.service | 11 | integral | S | compositor
etc/units/logd.service | 11 | integral | S | logging
etc/units/ntwm.service | 12 | integral | S | WM
musl-nt/Makefile | 168 | integral | M | build libc
musl-nt/README.md | 152 | integral | D | comportamento libc
musl-nt/arch/x86_64/bits/setjmp.h | 2 | integral | M | ABI/fork
musl-nt/arch/x86_64/crt_arch.h | 1 | integral | M | startup
musl-nt/arch/x86_64/pthread_arch.h | 14 | integral | M | TLS
musl-nt/arch/x86_64/syscall_arch.h | 37 | integral | M | syscalls
musl-nt/crt/crt0.c | 267 | integral | M | startup/fork
musl-nt/include/nt/ntabi.h | 460 | integral | M | ABI
musl-nt/include/nt/ntpriv.h | 268 | integral | M | estruturas internas
musl-nt/nt/convert.c | 127 | integral | M | conversões
musl-nt/nt/errno_xlat.c | 129 | integral | M | erros
musl-nt/nt/fdtable.c | 262 | integral | M | FDs/PTY
musl-nt/nt/nt_syscall.c | 201 | integral | M | dispatch
musl-nt/nt/nt_syscall.h | 15 | integral | M | dispatch API
musl-nt/nt/ntpath.c | 260 | integral | M | caminhos/CWD
musl-nt/nt/stubs.c | 234 | integral | M | limites
musl-nt/nt/sys_dir.c | 200 | integral | M | diretórios
musl-nt/nt/sys_file.c | 507 | integral | M | I/O/poll
musl-nt/nt/sys_fs.c | 151 | integral | M | filesystem
musl-nt/nt/sys_ioctl.c | 126 | integral | M | termios/winsize
musl-nt/nt/sys_link.c | 94 | integral | M | links
musl-nt/nt/sys_mem.c | 215 | integral | M | memória
musl-nt/nt/sys_net.c | 942 | integral | M | sockets
musl-nt/nt/sys_proc.c | 492 | integral | M | fork/spawn/wait
musl-nt/nt/sys_pty.c | 230 | integral | M | line discipline
musl-nt/nt/sys_signal.c | 131 | integral | M | sinais
musl-nt/nt/sys_stat.c | 106 | integral | M | stat
musl-nt/nt/sys_time.c | 155 | integral | M | timers
musl-nt/ntposix-gcc | 96 | integral | B | toolchain
musl-nt/override/__init_tls.c | 67 | integral | M | TLS/fork
musl-nt/override/__set_thread_area.c | 19 | integral | M | TLS
musl-nt/override/posix_spawn.c | 132 | integral | M | spawn/actions
musl-nt/override/setjmp.S | 43 | integral | M | contexto
musl-nt/override/syscall_cp.c | 21 | integral | M | cancelamento
musl-nt/patches/busybox-ntunix-ash.patch | 99 | integral | M | shell/PTY
musl-nt/test/busybox-coreutils.config | 174 | integral | T | configuração
musl-nt/test/busybox-runtime.cmd | 83 | integral | T | não executado
musl-nt/test/forktest.c | 39 | integral | T | contrato fork
musl-nt/test/hello.c | 7 | integral | T | startup
musl-nt/test/memory.c | 41 | integral | T | memória
musl-nt/test/network.c | 304 | integral | T | rede/poll
musl-nt/test/signal.c | 36 | integral | T | sinais
musl-nt/test/smoke.c | 67 | integral | T | syscalls
musl-nt/test/spawn.c | 58 | integral | T | posix_spawn
musl-nt/tools/configure-busybox | 47 | integral | B | BusyBox
musl-nt/tools/lp64-coff-cc | 44 | integral | B | ABI/toolchain
musl-nt/tools/sysv-coff-rewrite.cpp | 64 | integral | B | ABI/COFF
proc/mounts | 1 | integral | B | BusyBox
src/apps/ntclock/ntclock.c | 105 | integral | P | cliente appsrv
src/common/ntu.h | 39 | integral | C | APIs comuns
src/common/ntuini.c | 51 | integral | C | parser config
src/common/ntupath.c | 88 | integral | C | caminhos
src/common/ntuutil.c | 24 | integral | C | utilitários
src/common/ntuwm.h | 87 | integral | P | protocolo WM
src/demod/demod.c | 36 | integral | S | serviço demo
src/dispd/appsrv.c | 372 | integral | P | surfaces/apps
src/dispd/compositor.c | 544 | integral | P | janelas/composição
src/dispd/dispd.c | 436 | integral | P | main loop
src/dispd/dispd.h | 136 | integral | P | estado global
src/dispd/input.c | 350 | integral | P | teclado/mouse
src/dispd/present.h | 34 | integral | P | apresentação
src/dispd/present_dxgi.c | 211 | integral | P | DXGI
src/dispd/present_gdi.c | 67 | integral | P | GDI
src/dispd/term.c | 127 | integral | P | seleção backend
src/dispd/term.h | 116 | integral | P | estado terminal
src/dispd/term_conpty.c | 228 | integral | P | ConPTY
src/dispd/term_pty.c | 246 | integral | P | PTY nativo
src/dispd/term_scrape.c | 231 | integral | P | scrape
src/dispd/vt.c | 420 | integral | P | render VT
src/dispd/wmproto.c | 558 | integral | P | transações/protocolo
src/initd/initd.c | 87 | integral | S | supervisor
src/initd/initd.h | 59 | integral | S | estado serviços
src/initd/pipesrv.c | 228 | integral | S | controle initd
src/initd/service.c | 432 | integral | S | jobs/restart
src/libntposix/npx.c | 296 | integral | M | backend legado
src/libntposix/npx.h | 69 | integral | M | API legado
src/logd/logd.c | 76 | integral | S | logging
src/ntctl/ntctl.c | 93 | integral | S | cliente initd
src/ntsession/ntsession.c | 105 | integral | S | sessão/recovery
src/ntwm/layout.c | 262 | integral | P | tiling/floating
src/ntwm/ntwm.c | 212 | integral | P | event loop/config
src/ntwm/ntwm.h | 55 | integral | P | modelo WM
src/ntwm/proto.c | 70 | integral | P | cliente pipe
test/build-check.sh | 90 | integral | T | lido, não executado
test/smoke.sh | 84 | integral | T | lido, não executado
third_party/libvterm/LICENSE | 23 | integral | V | licença
third_party/libvterm/include/vterm.h | 645 | integral | V | API VT
third_party/libvterm/include/vterm_keycodes.h | 61 | integral | V | teclado
third_party/libvterm/src/encoding.c | 230 | integral | V | encodings
third_party/libvterm/src/encoding/DECdrawing.inc | 36 | integral | V | charset DEC
third_party/libvterm/src/encoding/uk.inc | 6 | integral | V | charset UK
third_party/libvterm/src/fullwidth.inc | 111 | integral | V | largura Unicode
third_party/libvterm/src/keyboard.c | 226 | integral | V | teclado VT
third_party/libvterm/src/mouse.c | 99 | integral | V | mouse VT
third_party/libvterm/src/parser.c | 402 | integral | V | parser CSI
third_party/libvterm/src/pen.c | 607 | integral | V | SGR
third_party/libvterm/src/rect.h | 56 | integral | V | geometria
third_party/libvterm/src/screen.c | 1214 | integral | V | grade/resize
third_party/libvterm/src/state.c | 2380 | integral | V | estado VT
third_party/libvterm/src/unicode.c | 313 | integral | V | Unicode
third_party/libvterm/src/utf8.h | 39 | integral | V | UTF-8
third_party/libvterm/src/vterm.c | 430 | integral | V | core/allocation
third_party/libvterm/src/vterm_internal.h | 298 | integral | V | estruturas/limites
```

Consultados parcialmente: **nenhum**.

Apenas identificados:

```text
src/ntwm/config.c | — | apenas identificado | referido por ntwm.h, mas não existe
/tmp/musl-1.2.6 | — | apenas identificado | fonte externa da musl
/tmp/bbsrc | — | apenas identificado | fonte externa do BusyBox
headers/libraries MinGW, Windows SDK e LLVM | — | apenas identificado | dependências externas
links/projetos citados em docs/pesquisa | — | apenas identificado | bibliografia fora do runtime
```

