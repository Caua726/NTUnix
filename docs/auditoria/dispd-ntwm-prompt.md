# Prompt — auditoria profunda do `dispd` e do `ntwm`

Faça uma auditoria extremamente profunda do compositor `dispd` e do window
manager `ntwm` deste repositório.

Não altere nenhum arquivo. Não crie relatórios, notas ou arquivos temporários
dentro do repositório. Não implemente correções. Sua tarefa é exclusivamente
ler, raciocinar e produzir um relatório completo de defeitos na resposta.

## Restrições operacionais

Use somente operações read-only para inspecionar Git e ler ou pesquisar
arquivos.

Não execute:

- testes;
- builds;
- compiladores, inclusive em modo syntax-only;
- linters;
- formatadores;
- analisadores estáticos;
- binários do projeto;
- Wine;
- scripts de validação;
- qualquer outra forma de validação executável.

Você deve ler os arquivos inteiros, linha por linha, um por um. Não faça uma
análise superficial e não pare depois de encontrar os primeiros problemas.

Comece verificando o estado atual do repositório com:

```text
git status --short --branch --untracked-files=all
```

Alterações não commitadas e arquivos não rastreados relacionados ao escopo
também fazem parte da auditoria. Preserve tudo e considere o conteúdo atual do
working tree como a fonte de verdade.

## Descoberta e leitura dos arquivos

Primeiro, descubra todos os arquivos relacionados ao compositor, WM, protocolo,
entrada, apresentação, superfícies de apps, terminal integrado, configuração,
inicialização, supervisão dos processos e aplicativos usados como demonstração.

No mínimo, leia integralmente:

- `src/dispd/dispd.h`
- `src/dispd/dispd.c`
- `src/dispd/compositor.c`
- `src/dispd/wmproto.c`
- `src/dispd/appsrv.c`
- `src/dispd/input.c`
- `src/dispd/present.h`
- `src/dispd/present_gdi.c`
- `src/dispd/present_dxgi.c`
- `src/dispd/term.h`
- `src/dispd/term.c`
- `src/dispd/term_conpty.c`
- `src/dispd/term_scrape.c`
- `src/dispd/vt.c`
- `src/ntwm/ntwm.h`
- `src/ntwm/ntwm.c`
- `src/ntwm/layout.c`
- `src/ntwm/proto.c`
- `src/common/ntuwm.h`
- `src/common/ntu.h`
- `src/common/ntuini.c`
- `src/common/ntupath.c`
- `src/common/ntuutil.c`
- `src/apps/ntclock/ntclock.c`
- `src/ntsession/ntsession.c`
- `src/initd/initd.h`
- `src/initd/initd.c`
- `src/initd/service.c`
- `etc/ntwm/ntwm.conf`
- `etc/units/dispd.service`
- `etc/units/ntwm.service`
- `docs/auditoria/desktop-known-gaps.md`
- `docs/PROTOCOLO.md`
- `README.md`
- `Makefile`
- `build/SetupComplete.cmd`
- `build/autounattend.xml`
- `build/make-iso.sh`
- `build/make-live.sh`
- `build/strip.list`
- `build/test-vm.sh`
- `build/vm-setup.sh`
- `build/winpeshl.ini`
- `docs/VISAO.md`
- `docs/musl-nt-spec.md`
- `docs/pesquisa/bb-coreutils-funcoes-libc-consumidas.txt`
- `docs/pesquisa/bb-coreutils-headers-fecho-completo.txt`
- `docs/pesquisa/bb-coreutils-headers-publicos.txt`
- `docs/pesquisa/busybox-coreutils-musl-deps.md`
- `docs/pesquisa/comparacao-linux-vs-nt.md`
- `docs/pesquisa/deep-compositors.md`
- `docs/pesquisa/deep-dwm.md`
- `docs/pesquisa/deep-gfx-server-linux.md`
- `docs/pesquisa/deep-gfx-server-windows.md`
- `docs/pesquisa/deep-wm-architecture.md`
- `docs/pesquisa/deep-xorg-server.md`
- `docs/pesquisa/nt-boot-install.md`
- `docs/pesquisa/nt-dwm-compositor.md`
- `docs/pesquisa/nt-native-api.md`
- `docs/pesquisa/nt-win11-image.md`
- `docs/pesquisa/wm-compositor-survey.md`
- `etc/group`
- `etc/hosts`
- `etc/mtab`
- `etc/passwd`
- `etc/units/demod.service`
- `etc/units/logd.service`
- `musl-nt/Makefile`
- `musl-nt/README.md`
- `musl-nt/arch/x86_64/bits/setjmp.h`
- `musl-nt/arch/x86_64/crt_arch.h`
- `musl-nt/arch/x86_64/pthread_arch.h`
- `musl-nt/arch/x86_64/syscall_arch.h`
- `musl-nt/crt/crt0.c`
- `musl-nt/include/nt/ntabi.h`
- `musl-nt/include/nt/ntpriv.h`
- `musl-nt/nt/convert.c`
- `musl-nt/nt/errno_xlat.c`
- `musl-nt/nt/fdtable.c`
- `musl-nt/nt/nt_syscall.c`
- `musl-nt/nt/nt_syscall.h`
- `musl-nt/nt/ntpath.c`
- `musl-nt/nt/stubs.c`
- `musl-nt/nt/sys_dir.c`
- `musl-nt/nt/sys_file.c`
- `musl-nt/nt/sys_fs.c`
- `musl-nt/nt/sys_ioctl.c`
- `musl-nt/nt/sys_link.c`
- `musl-nt/nt/sys_mem.c`
- `musl-nt/nt/sys_net.c`
- `musl-nt/nt/sys_proc.c`
- `musl-nt/nt/sys_pty.c`
- `musl-nt/nt/sys_signal.c`
- `musl-nt/nt/sys_stat.c`
- `musl-nt/nt/sys_time.c`
- `musl-nt/ntposix-gcc`
- `musl-nt/override/__init_tls.c`
- `musl-nt/override/__set_thread_area.c`
- `musl-nt/override/posix_spawn.c`
- `musl-nt/override/setjmp.S`
- `musl-nt/override/syscall_cp.c`
- `musl-nt/patches/busybox-ntunix-ash.patch`
- `musl-nt/test/busybox-coreutils.config`
- `musl-nt/test/busybox-runtime.cmd`
- `musl-nt/test/forktest.c`
- `musl-nt/test/hello.c`
- `musl-nt/test/memory.c`
- `musl-nt/test/network.c`
- `musl-nt/test/signal.c`
- `musl-nt/test/smoke.c`
- `musl-nt/test/spawn.c`
- `musl-nt/tools/configure-busybox`
- `musl-nt/tools/lp64-coff-cc`
- `musl-nt/tools/sysv-coff-rewrite.cpp`
- `proc/mounts`
- `src/demod/demod.c`
- `src/dispd/term_pty.c`
- `src/initd/pipesrv.c`
- `src/libntposix/npx.c`
- `src/libntposix/npx.h`
- `src/logd/logd.c`
- `src/ntctl/ntctl.c`
- `test/build-check.sh`
- `test/smoke.sh`
- `third_party/libvterm/LICENSE`
- `third_party/libvterm/include/vterm.h`
- `third_party/libvterm/include/vterm_keycodes.h`
- `third_party/libvterm/src/encoding.c`
- `third_party/libvterm/src/encoding/DECdrawing.inc`
- `third_party/libvterm/src/encoding/uk.inc`
- `third_party/libvterm/src/fullwidth.inc`
- `third_party/libvterm/src/keyboard.c`
- `third_party/libvterm/src/mouse.c`
- `third_party/libvterm/src/parser.c`
- `third_party/libvterm/src/pen.c`
- `third_party/libvterm/src/rect.h`
- `third_party/libvterm/src/screen.c`
- `third_party/libvterm/src/state.c`
- `third_party/libvterm/src/unicode.c`
- `third_party/libvterm/src/utf8.h`
- `third_party/libvterm/src/vterm.c`
- `third_party/libvterm/src/vterm_internal.h`

Leia também integralmente as dependências diretas de runtime, headers incluídos,
implementações chamadas, configurações consumidas, units, scripts de
inicialização e documentação que declare o comportamento atual desses
componentes.

Para esta auditoria, "arquivo chamado ou referenciado" significa o closure
direto de:

- runtime;
- compilação dos componentes auditados;
- configuração;
- inicialização;
- supervisão;
- protocolo;
- documentação normativa do comportamento atual.

Citações bibliográficas, pesquisas históricas, links externos e projetos usados
apenas como inspiração não precisam ser seguidos recursivamente, salvo quando o
repositório os trate explicitamente como especificação do comportamento atual.
Identifique esses arquivos ou referências separadamente como fora do closure de
runtime.

Não deixe de seguir funções, estruturas, callbacks, threads, handles, protocolos,
filas, ownership e estados compartilhados através de vários arquivos.

Antes da análise, registre a quantidade de linhas de cada arquivo selecionado.
Para considerar um arquivo "lido integralmente", confirme que todo o intervalo,
da primeira à última linha, foi efetivamente inspecionado. Não classifique como
integral um arquivo cuja saída tenha sido truncada ou do qual apenas trechos
tenham sido pesquisados.

No inventário final, informe para cada arquivo:

- caminho;
- quantidade de linhas;
- classificação: `integral`, `consultado parcialmente` ou `apenas identificado`;
- motivo de inclusão;
- quais arquivos ou fluxos levaram até ele.

## Passagens obrigatórias

Depois de ler cada arquivo individualmente, faça uma segunda passagem cruzando
todos eles. Muitos bugs não aparecem dentro de uma função isolada: surgem quando
um componente assume uma coisa e outro implementa outra.

Para todo defeito que envolva mais de um arquivo, mostre a sequência temporal ou
de chamadas responsável. Identifique em cada etapa:

- processo;
- thread;
- função ou callback;
- estado lido;
- estado modificado;
- mensagem ou evento enviado;
- ponto em que a invariante é violada.

Exemplo de formato:

```text
ntwm/main: envia WORKSPACE
→ dispd/reader: enfileira a mensagem
→ dispd/main: aplica cur_ws antes de FRAME-BEGIN
→ dispd/main: roteia input usando o workspace novo
→ present: ainda mostra o frame anterior
```

Antes de escrever o relatório final, faça uma terceira passagem orientada por
invariantes.

## Escopo técnico da auditoria

Analise absolutamente tudo que puder constituir um defeito, inclusive:

1. Bugs lógicos e comportamentos incorretos.
2. Funcionalidades declaradas, documentadas ou configuradas que não funcionam.
3. Funcionalidades implementadas apenas parcialmente.
4. Bugs de UI e UX.
5. Comportamentos confusos, inconsistentes, feios ou frustrantes para o usuário.
6. Ausência de feedback, mensagens de erro ou recuperação.
7. Foco, teclado, mouse, atalhos e roteamento de entrada.
8. Workspaces, tiling, floating, z-order, geometria, gaps e bordas.
9. Criação, redimensionamento, foco, fechamento e destruição de janelas.
10. Reinicialização ou queda do WM e recuperação do estado.
11. Divergência de estado entre `dispd` e `ntwm`.
12. Violações das invariantes do protocolo.
13. Comandos e eventos definidos mas ignorados ou implementados incorretamente.
14. Parsing incorreto, truncamento, falta de escaping e protocol injection.
15. Concorrência, data races, deadlocks, bloqueios e reentrância.
16. Use-after-free, double-free, leaks, handles vazados e threads abandonadas.
17. Filas cheias, perda silenciosa de eventos e falta de backpressure.
18. Falhas de alocação ou de chamadas Win32 que não são verificadas.
19. Bugs em caminhos de erro e teardown.
20. Problemas de segurança, autenticação, permissões e isolamento entre processos.
21. Problemas no compartilhamento de superfícies e sincronização dos buffers.
22. Tearing, frames parcialmente desenhados e commits não atômicos.
23. Damage tracking, redraw excessivo, consumo de CPU e pacing.
24. GDI, DXGI, device loss, resize de swapchain e fallback.
25. Mudança de resolução, DPI, monitor e múltiplos monitores.
26. Terminal integrado, ConPTY, fallback scrape e parser VT.
27. Unicode, UTF-8, AltGr, dead keys, modificadores e layouts de teclado.
28. Cores, cursor, sequências ANSI e renderização da grade.
29. Valores-limite, overflow, underflow, valores negativos e listas grandes.
30. Configurações inválidas ou não validadas.
31. Código morto, campos escritos mas nunca lidos e APIs declaradas mas não usadas.
32. Duplicação, acoplamento excessivo, estado global e responsabilidades misturadas.
33. Gambiarras, hacks frágeis e suposições que só funcionam por acaso.
34. Código tecnicamente funcional, mas mal projetado ou difícil de manter.
35. Escolhas que arruínam ou degradam a experiência do usuário.
36. Qualquer contradição entre comentários, documentação e implementação.
37. Qualquer coisa que pareça não intencional, mesmo que não cause crash.
38. Qualquer problema adicional que não se encaixe nessas categorias.

Não descarte um problema apenas porque:

- só aparece com muitas janelas;
- depende de timing;
- ocorre apenas durante restart ou shutdown;
- acontece em configuração inválida;
- aparece apenas em outro layout de teclado;
- exige um app mal-comportado;
- é um problema de desempenho;
- é uma falha de UX em vez de crash;
- parece uma limitação da primeira versão;
- já existe um comentário reconhecendo a limitação.

Ao mesmo tempo, não invente bugs. Para cada achado, confirme pelo fluxo real do
código.

## Tipos obrigatórios de achado

Classifique cada item com exatamente um destes tipos:

- `DEF-C`: defeito confirmado pela leitura e pelo fluxo real;
- `RISK`: risco provável dependente de timing, OOM, comportamento de API ou
  condição externa que o código não controla adequadamente;
- `LIMIT`: limitação deliberada, inclusive quando documentada;
- `ARCH`: melhoria arquitetural sem falha comportamental diretamente
  demonstrável.

Não apresente `LIMIT` ou `ARCH` como crash confirmado. No resumo executivo e na
priorização, conte separadamente os achados de cada tipo.

## Rubrica obrigatória de gravidade

Use estas definições:

- **Crítica:** execução arbitrária, corrupção de memória, corrupção do estado
  global, use-after-free, perda irrecuperável de controle ou desktop
  inutilizável.
- **Alta:** quebra de função central, divergência persistente, freeze
  significativo, entrada ou janela direcionada incorretamente, perda de dados
  ou perda relevante de estado.
- **Média:** falha recuperável, funcionalidade importante parcial, degradação
  forte de UI/UX ou desempenho, ou ausência relevante de feedback.
- **Baixa:** problema cosmético, de manutenção, valor-limite pouco provável ou
  degradação pequena.

Não aumente a gravidade apenas porque uma chamada Win32 não é verificada.
Demonstre o estado posterior à falha e sua consequência.

## Invariantes obrigatórias

Verifique pelo menos estas invariantes:

- existe no máximo uma janela realmente focada;
- a janela focada pertence ao workspace visível;
- `dispd` e `ntwm` concordam sobre foco, workspace, geometria e existência das
  janelas;
- todo `FRAME-BEGIN` possui semântica realmente atômica até `FRAME-COMMIT`;
- uma janela movida de workspace desaparece imediatamente do workspace anterior;
- uma janela destruída não continua referenciada;
- um app fechado não continua rodando ou enviando commits;
- um resize não desconecta o compositor do buffer que o app ainda desenha;
- todo buffer compartilhado possui sincronização adequada;
- nenhuma thread continua usando memória depois do teardown;
- uma fila cheia não corrompe silenciosamente o estado;
- um WM travado não bloqueia compositor e teclado;
- um terminal oculto não força redraw permanente;
- falhas de GDI/DXGI são detectadas e recuperadas;
- entrada não é enviada para uma janela invisível ou errada;
- atalhos capturados não vazam keyup ou são processados duas vezes;
- Unicode e layouts não americanos não são truncados;
- o snapshot de reconexão é suficiente para restaurar o desktop;
- todos os comandos e eventos do protocolo têm implementação coerente;
- todos os campos de estado possuem dono claro e ciclo de vida consistente.

Para cada invariante, dê exatamente um destes vereditos:

- `MANTIDA`;
- `VIOLADA`;
- `PARCIAL`;
- `NÃO DEMONSTRÁVEL`.

Cite os números dos achados que justificam o veredito e descreva uma sequência
concreta de eventos. Não marque uma invariante como mantida apenas porque o fluxo
normal do `ntwm` tenta respeitá-la. Considere também:

- comandos perdidos;
- filas cheias;
- reconnect;
- timeout;
- restart;
- teardown;
- clientes malformados;
- falhas de alocação;
- chamadas de sistema que falham.

## Contradições entre documentação e implementação

Para cada contradição encontrada, informe separadamente:

- a alegação exata do comentário ou documentação;
- o arquivo e linha da alegação;
- a implementação real;
- o arquivo e linha da implementação;
- a sequência que desmente a alegação;
- o impacto de manter a documentação incorreta.

Inclua no final uma tabela específica:

```text
Alegação | Implementação real | Achados relacionados | Arquivos
```

## Formato obrigatório do relatório

1. Comece com um resumo executivo informando os problemas mais destrutivos.
2. Informe no resumo:
   - estado inicial do working tree;
   - total de achados;
   - contagem por tipo (`DEF-C`, `RISK`, `LIMIT`, `ARCH`);
   - contagem por gravidade.
3. Depois liste todos os achados individualmente e numere-os continuamente.
4. Não agrupe defeitos independentes em um único número.
5. Para cada achado, informe:
   - número;
   - tipo: `DEF-C`, `RISK`, `LIMIT` ou `ARCH`;
   - gravidade: crítica, alta, média ou baixa;
   - categoria primária;
   - categorias secundárias, se existirem;
   - arquivo e linha exata;
   - trecho ou fluxo responsável;
   - sequência temporal, quando envolver vários componentes;
   - por que está errado;
   - cenário concreto em que acontece;
   - impacto visível para usuário ou sistema;
   - componentes envolvidos;
   - causa raiz;
   - direção recomendada para corrigir, sem implementar.
6. Quando um problema depender de vários arquivos, cite todos.
7. Identifique explicitamente quando vários sintomas possuem a mesma causa raiz.
8. Crie uma seção separada para:
   - UI/UX;
   - foco e entrada;
   - workspaces e layout;
   - superfícies de apps;
   - protocolo;
   - concorrência e lifecycle;
   - renderização GDI/DXGI;
   - terminal/VT;
   - desempenho;
   - segurança;
   - qualidade arquitetural e gambiarras.
9. Não repita o mesmo achado em mais de uma seção. Escolha uma categoria
   primária e use referências cruzadas para as categorias secundárias.
10. Inclua a tabela de invariantes com veredito e achados relacionados.
11. Inclua a tabela de contradições entre documentação e implementação.
12. Termine com uma priorização:
   - o que impede o desktop de funcionar;
   - o que pode causar crash ou corrupção;
   - o que prejudica fortemente a UX;
   - o que pode esperar.
13. Inclua uma tabela final relacionando:
   - causa raiz;
   - sintomas;
   - números dos achados;
   - arquivos afetados.
14. Informe exatamente quais arquivos foram:
   - lidos integralmente;
   - consultados parcialmente;
   - apenas identificados.
15. Para cada arquivo do inventário, informe a quantidade de linhas e o motivo
   de inclusão.
16. Informe o resultado do `git status` inicial. Não altere o repositório para
   produzir o relatório.

Não estabeleça um número máximo de achados. Se houver 100, liste 100. Se houver
200, liste 200. Continue até esgotar os fluxos e defeitos identificáveis no
código.

Não encerre com uma análise genérica. Produza um inventário técnico, exaustivo,
baseado em evidências e útil para orientar uma correção completa do compositor e
do WM.
