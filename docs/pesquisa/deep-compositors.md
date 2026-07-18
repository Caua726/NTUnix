# Deep architecture of real compositors — end-to-end, read from source

> Pesquisa técnica para o **dispd**: nosso compositor from-scratch que **toma posse da tela**, hospeda superfícies de terminal (cada uma com um **DIB** por trás), compõe num **backbuffer** e apresenta (**GDI BitBlt** agora, **DXGI flip** depois) sobre Windows/NT.
>
> Este documento traça, lendo o **código real**, como a composição funciona de verdade em: **wlroots/tinywl** (a referência canônica ~1000 linhas), **dwl** (compositor dwm-like), **sway** e **Weston** (arquitetura), e **Picom** (compositor X11 standalone, modelo de redirection).
>
> Objetivo: extrair os mecanismos exatos (fluxo de controle, nomes de função, buffer/damage/present) e mapear cada um para o dispd. Cada afirmação forte cita arquivo:linha real.
>
> Fontes lidas (clones rasos, HEAD em jul/2026):
> - **wlroots** `0.21.0-dev` (commit `d64acff`) — `gitlab.freedesktop.org/wlroots/wlroots`
> - **dwl** (commit `a2d03cf`) — `codeberg.org/dwl/dwl`
> - **sway** (HEAD) — `github.com/swaywm/sway`
> - **weston** (HEAD) — `gitlab.freedesktop.org/wayland/weston`
> - **picom** (commit `6d67682`) — `github.com/yshui/picom`
>
> Complementa `nt-dwm-compositor.md` (o *porquê* do caminho 3b: flip-model fullscreen) — este documento é o *como*, tirado de compositores que já funcionam.

---

## 0. TL;DR — as 12 ideias que mais importam pro dispd

1. **O compositor é uma máquina de estados dirigida por eventos, não um loop de render busy.** Nada é desenhado até que *algo mude*. Mudança → damage → "agende um frame" → (no vblank) → evento `frame` → compõe **só o damage** → apresenta. Sem mudança, zero CPU/GPU. (`wlr_output_schedule_frame`, `output_frame`).
2. **Scene graph, não render manual.** wlroots/dwl/sway compõem via uma árvore de nós (`wlr_scene`); o autor do compositor **só posiciona nós** e chama `wlr_scene_output_commit`. Toda a lógica de damage/oclusão/present é da árvore. dispd deveria ter o mesmo: uma árvore de `dispd_surface` e um `dispd_scene_commit`.
3. **Damage é a alma da performance no software rendering.** O renderer pixman (software, o análogo direto do nosso GDI) **clipa cada blit à região de damage** — `pixman_image_set_clip_region32(buffer->image, clip)` + `pixman_image_composite32`. Isso é *literalmente* `BitBlt` com uma clip region. Sem damage, você redesenha a tela inteira todo frame — inaceitável no software.
4. **Double/triple buffer exige "buffer age": cada backbuffer guarda conteúdo de N frames atrás**, então você precisa repintar tudo que mudou *desde a última vez que aquele buffer específico foi apresentado*, não só o último frame. O `wlr_damage_ring` implementa exatamente isso. Se dispd tiver 2 backbuffers (BitBlt) ou 2-3 (flip DXGI), precisa da mesma contabilidade por-buffer.
5. **O contrato de buffer com o cliente:** o cliente desenha num buffer, faz `commit` (declaração atômica "este frame está pronto"), o compositor **copia para textura e devolve o buffer** (`wl_buffer.release`) para o cliente reusar. wl_shm é liberado *imediatamente após upload*. Para dispd: o terminal desenha no DIB, faz commit, dispd copia o DIB (ou segura a referência) e devolve — é o double-buffer do lado cliente.
6. **`commit` atômico elimina as corridas de partial-update do X11.** O cliente nunca mostra um buffer meio-desenhado porque o compositor só consome buffers *committed*. Isso é o oposto do modelo X11 do Picom (redirect + XDamage + polling reativo), que é "after-the-fact" e sujeito a tearing/races por construção.
7. **Layout atômico multi-janela via "transação":** sway junta todas as mudanças de layout numa transação, envia `configure` a cada janela, **salva o buffer antigo** (`view_save_buffer`) e só aplica o novo layout quando *todas* as janelas responderam com buffer do tamanho certo (ou timeout ~200ms). Resultado: um resize de várias janelas aparece como **uma** mudança visual, nunca um frame intermediário inconsistente.
8. **Present = commit atômico de estado.** No KMS o frame chega à tela via `drmModeAtomicCommit` com `DRM_MODE_PAGE_FLIP_EVENT`; o page-flip completa no vblank, e é *nesse callback* que (a) o buffer antigo é liberado e (b) o próximo `frame` é disparado. O nosso "present backend" (BitBlt/DXGI flip) é o equivalente exato — e o *completion do flip* deve dirigir o próximo frame.
9. **Cursor de hardware é um plano separado.** KMS tem um plano de cursor dedicado (move sem recompor). Fallback de software desenha o cursor *dentro* do render pass, clipado ao damage. dispd: comece com cursor em software (desenhar por último no backbuffer), evolua para camada/overlay depois.
10. **Latência: renderize o mais tarde possível antes do vblank.** sway não renderiza logo que o vblank chega; ele *atrasa* até `vblank_previsto − max_render_time`, para o frame conter o conteúdo mais fresco do cliente. Reduz latência input→foto. Vale ouro pra um terminal.
11. **Oclusão (occlusion culling): não pinte o que está coberto.** A árvore subtrai as regiões opacas das janelas de cima do que precisa ser pintado embaixo (`scene_node_opaque_region` → `pixman_region32_subtract`). Um terminal opaco cobrindo outro economiza toda a pintura de baixo.
12. **Separe estado *pending* de estado *current*.** Toda a robustez vem de acumular mudanças num estado pending (`wlr_output_state`, `wlr_surface_state.pending`) e aplicá-lo de uma vez (`commit`). Nunca mute o estado "ao vivo" no meio do caminho.

---

## 1. Modelo mental: o compositor como dono da tela

Um compositor moderno (Wayland) **é o display server**: ele possui a saída de vídeo (no Linux, é o *DRM master*), recebe input cru (libinput/evdev), hospeda os clientes (protocolo Wayland) e é o único que fala com o scanout (KMS). Isso é exatamente a postura do dispd no caminho 3b: dono da flip chain fullscreen, input via Raw Input, apps nativas como "clientes".

O `main()` do **tinywl** (`tinywl/tinywl.c:887`) monta essa máquina em ~8 peças e entrega o controle ao event loop:

- `wl_display_create()` — o loop de eventos + socket de clientes (libwayland).
- `wlr_backend_autocreate()` (`:915`) — abstrai input+output do hardware (DRM+KMS+libinput, ou uma janela X11/Wayland aninhada). **Vira DRM master no `wlr_backend_start`** (`:1050`).
- `wlr_renderer_autocreate()` (`:925`) — Pixman (software), GLES2 ou Vulkan. **Define os formatos de pixel** que os clientes podem usar.
- `wlr_allocator_autocreate()` (`:937`) — a ponte renderer↔backend que aloca os buffers onde se desenha.
- `wlr_compositor_create()` (`:951`) — expõe o global `wl_compositor`; é o que deixa clientes criarem `wl_surface`.
- `wlr_scene_create()` (`:971`) — **a árvore de cena**: "handles all rendering and damage tracking. All the compositor author needs to do is add things ... and then call wlr_scene_output_commit()".
- `wlr_xdg_shell_create()` (`:979`) — o protocolo de janelas de aplicação (toplevels/popups).
- `wlr_cursor` + `wlr_seat` (`:989`,`:1030`) — cursor e "assento" (foco de teclado/ponteiro).

Depois disso, `wl_display_run()` (`:1070`) roda o loop até sair. **Todo o resto é callback.** Este é o esqueleto que dispd deve espelhar: um event loop, um backend de I/O (Raw Input + a saída DXGI/GDI), um "renderer" (GDI/pixman-like), uma árvore de cena, um protocolo apps↔dispd, e um seat.

---

## 2. Surface/buffer lifecycle (item 1) — do wl_buffer à textura, e de volta

### 2.1 O buffer duplo do cliente e o commit atômico

Wayland separa rigorosamente **pending** de **current**. O cliente acumula requisições no estado pending e as aplica de uma vez com `wl_surface.commit`. Em `types/wlr_compositor.c`:

- `surface_handle_attach` (`:68`) — cliente anexa um `wl_buffer`: seta `pending.committed |= WLR_SURFACE_STATE_BUFFER` e guarda o resource. **Não faz nada visível ainda.**
- `surface_handle_damage` / `damage_buffer` (`:91`) — acumula `pending.surface_damage` (a região que o cliente redesenhou dentro do buffer).
- `surface_handle_frame` (`:108`) — cliente registra um `wl_callback` de "frame done": pede pro compositor avisar quando for boa hora de desenhar o próximo frame (é o throttle do cliente ao refresh).
- `surface_handle_commit` (`:575`) — **o ponto atômico**: `surface_finalize_pending` resolve o buffer, e então `surface_commit_state` (`:511`) move pending→current de uma vez.

### 2.2 Upload para textura + release imediato

O coração do lifecycle está em `surface_apply_damage` (`types/wlr_compositor.c:407`) e no fim de `surface_commit_state`:

- Se já existe uma textura e o buffer é compatível, faz **update in-place só do damage**: `wlr_client_buffer_apply_damage(surface->buffer, surface->current.buffer, &surface->buffer_damage)` (`:421`) — sobe só os pixels sujos. Se conseguiu, **destranca o buffer do cliente na hora** (`wlr_buffer_unlock`, `:423`).
- Senão, cria uma textura nova: `wlr_client_buffer_create(surface->current.buffer, renderer)` (`:433`) — faz upload do buffer para GPU/textura.
- Ao fim de `surface_commit_state` (`:571`): `wlr_buffer_unlock(surface->current.buffer)` com o comentário decisivo:
  > *"Don't leave the buffer locked so that wl_shm buffers can be released immediately on commit when they are uploaded to the GPU."*

**Tradução para dispd:** quando o terminal faz commit do seu DIB, dispd **copia o DIB para a sua própria textura/DIB-de-cena** e imediatamente devolve o buffer (envia o "release"), para o cliente poder reusá-lo no próximo frame. Alternativa (zero-copy): dispd *segura a referência* ao DIB do cliente e só o libera quando não estiver mais em uso (mais eficiente, mas o cliente precisa de ≥2 DIBs). O modelo wl_shm (copiar-e-liberar) é o mais simples e é exatamente o que casa com "cada surface é um DIB".

### 2.3 A ponte protocolo→cena

O `wlr_scene_surface` conecta o `wl_surface` à árvore. Em `types/scene/surface.c`:

- `handle_scene_surface_surface_commit` (`:361`) escuta `surface->events.commit` e chama `surface_reconfigure` (`:279`).
- `surface_reconfigure` empurra o buffer + damage do cliente para o nó da cena: `wlr_scene_buffer_set_buffer_with_options(scene_buffer, &surface->buffer->base, &options)` com `options.damage = &surface->buffer_damage` (`:345`-`353`).

Ou seja: **`wl_surface.damage` do cliente vira o damage do `scene_buffer`, que vira o damage do output.** Um caminho reto de "o cliente redesenhou este retângulo" até "repinte só este retângulo na tela".

### 2.4 O buffer no nó da cena e o release de volta

Em `types/scene/wlr_scene.c`:

- `scene_buffer_set_buffer` (`:785`) — `wlr_buffer_lock(buffer)` para segurar, registra listener em `buffer->events.release`.
- `scene_buffer_handle_buffer_release` (`:775`) — quando o buffer é liberado, zera o ponteiro.
- `scene_buffer_get_texture` (`:1159`) — **lazy upload**: só cria a textura (`wlr_texture_from_buffer`) na hora de renderizar, e se for dono do buffer, destranca após o upload (`:1173`-`1176`). Para client buffers, reusa `client_buffer->texture` (já subida no commit).

**Lição dispd:** distinga "o DIB do cliente" (efêmero, devolvido rápido) de "a textura/DIB de cena" (o que dispd realmente compõe). Faça o upload/cópia lazy e só do damage.

---

## 3. O frame/render loop e o scene graph (item 2)

### 3.1 O que dirige um frame

Nada renderiza espontaneamente. A cadeia é:

1. **Algo muda** → gera damage → `wlr_output_schedule_frame(output)`.
2. No **vblank** (ou no timer do backend), o backend dispara `output->events.frame`.
3. O compositor trata em `output_frame` (tinywl `:573`) / `rendermon` (dwl) / `output_repaint_timer_handler` (sway):
   ```c
   // tinywl.c:573
   static void output_frame(struct wl_listener *listener, void *data) {
       struct wlr_scene_output *scene_output = wlr_scene_get_scene_output(scene, output->wlr_output);
       wlr_scene_output_commit(scene_output, NULL);          // compõe + apresenta se necessário
       clock_gettime(CLOCK_MONOTONIC, &now);
       wlr_scene_output_send_frame_done(scene_output, &now); // libera clientes p/ próximo frame
   }
   ```
4. `wlr_scene_output_send_frame_done` (`wlr_scene.c:2660`) percorre a árvore e dispara os `wl_callback` de frame de cada surface visível — é o que **throttla os clientes ao refresh** (eles só desenham o próximo frame quando avisados).

`wlr_scene_output_commit` (`wlr_scene.c:2162`) começa com o gate de performance:
```c
if (!wlr_scene_output_needs_frame(scene_output)) return true;  // nada mudou → não faz nada
```
`wlr_scene_output_needs_frame` (`:2156`) = `output->needs_frame || !pixman_region32_empty(&pending_commit_damage) || gamma_lut_changed`. **Sem damage, sem frame.**

### 3.2 Scene graph vs render manual

Os quatro compositores modernos (tinywl, dwl, sway, e o Weston moderno via seu próprio scene) usam **árvore de cena**, não render manual. A árvore é uma hierarquia de `wlr_scene_node`:
- `WLR_SCENE_NODE_TREE` — nó interno (grupo, com posição).
- `WLR_SCENE_NODE_RECT` — retângulo de cor sólida (bordas, backgrounds).
- `WLR_SCENE_NODE_BUFFER` — uma textura/surface (o conteúdo de uma janela).

O compositor **só manipula a árvore**: `wlr_scene_node_set_position` (`:1224`), `wlr_scene_node_raise_to_top` (`:1262`), `wlr_scene_node_set_enabled` (`:1207`), `wlr_scene_xdg_surface_create` (cria a subárvore de uma janela). **Cada mutação chama `scene_node_update`**, que gera damage automaticamente (§4.3). O autor nunca escreve o loop de blit.

dwl (`dwl.c`) prova que dá pra fazer um compositor dwm-like completo (tiling, tags, múltiplos monitores) só mexendo na árvore: `Client` tem um `wlr_scene_tree *scene` e 4 `wlr_scene_rect *border` (`dwl.c:110-112`); tiling é `wlr_scene_node_set_position`; o frame é o mesmo `wlr_scene_output_commit` (`dwl.c:2152 rendermon`).

### 3.3 Como o frame é montado — `wlr_scene_output_build_state`

Esta é a função central da composição (`wlr_scene.c:2286`), chamada por `wlr_scene_output_commit`. Fluxo:

1. **Monta a render list** dos nós que caem na box do output, do topo pra baixo:
   ```c
   scene_nodes_in_box(&scene->tree.node, &list_con.box, construct_render_list_iterator, &list_con); // :2363
   ```
   O resultado é `scene_output->render_list` — um array de `render_list_entry{node, x, y}`.
2. **Tenta direct scanout** (§5) se houver só 1 entry e nenhuma transformação — nesse caso o buffer do cliente vai *direto* pro scanout, sem composição.
3. **Adquire um backbuffer** do swapchain: `wlr_swapchain_acquire(swapchain)` (`:2463`).
4. **Roda o damage ring** para obter a região a repintar *neste buffer específico* (buffer age, §4.2):
   ```c
   wlr_damage_ring_rotate_buffer(&scene_output->damage_ring, buffer, &render_data.damage); // :2508
   ```
5. **Occlusion culling do fundo**: calcula onde o fundo (preto) aparece subtraindo as regiões opacas de todos os nós (`:2518`-`2545`), e pinta o fundo só ali.
6. **Renderiza a lista de baixo pra cima**, cada nó clipado ao damage:
   ```c
   for (int i = list_len - 1; i >= 0; i--)   // :2554  (de trás pra frente = de baixo pra cima)
       scene_entry_render(entry, &render_data);
   ```
7. **Cursores de software** entram por último no mesmo pass: `wlr_output_add_software_cursors_to_render_pass(output, render_pass, &render_data.damage)` (`:2592`).
8. **Submete e anexa o buffer ao estado de output**: `wlr_render_pass_submit` (`:2595`) → `wlr_output_state_set_buffer(state, buffer)` (`:2604`).

`scene_entry_render` (`wlr_scene.c:1422`) é onde cada nó vira comandos de render, sempre clipado:
```c
pixman_region32_copy(&render_region, &node->visible);
pixman_region32_intersect(&render_region, &render_region, &data->damage); // só o damage!
if (pixman_region32_empty(&render_region)) return;                        // nada a fazer
...
wlr_render_pass_add_texture(pass, &opts{ .clip = &render_region, ... });   // blit clipado
```

**Lição dispd:** o "build_state" é o modelo do `dispd_compose()`: (1) montar a lista de surfaces visíveis top→bottom, (2) pegar um backbuffer, (3) calcular a região suja *deste* backbuffer, (4) pintar o fundo só onde aparece, (5) blitar cada DIB de baixo pra cima clipado ao damage, (6) desenhar o cursor, (7) present.

---

## 4. Damage tracking (item 3) — por que não redesenhar tudo

Este é o item mais importante para o caso **software/GDI** do dispd. Redesenhar 1920×1080×4 bytes = ~8 MB por frame a 60 Hz = ~500 MB/s de memcpy só pra tela preta. Com damage, um cursor piscando repinta ~20×20 px.

### 4.1 Acumulação de damage por output

`scene_output_damage` (`wlr_scene.c:356`) é o funil único:
```c
static void scene_output_damage(struct wlr_scene_output *so, const pixman_region32_t *damage) {
    pixman_region32_intersect_rect(&clipped, damage, 0,0, output->width, output->height);
    if (!pixman_region32_empty(&clipped)) {
        wlr_output_schedule_frame(so->output);                 // agenda um frame
        wlr_damage_ring_add(&so->damage_ring, &clipped);       // acumula no ring (buffer age)
        pixman_region32_union(&so->pending_commit_damage, ..., &clipped); // damage a mandar ao KMS
    }
}
```
Dois acumuladores: `damage_ring` (para saber o que repintar em cada backbuffer) e `pending_commit_damage` (o damage a anunciar ao present/KMS via `FB_DAMAGE_CLIPS`).

### 4.2 Buffer age — o mecanismo do double/triple buffer

`types/wlr_damage_ring.c`. O ring guarda, por buffer já visto, a damage acumulada *desde a última vez que aquele buffer foi apresentado*. `wlr_damage_ring_rotate_buffer` (`:78`):
```c
pixman_region32_copy(damage, &ring->current);              // o damage novo deste frame
wl_list_for_each(entry, &ring->buffers, link) {
    if (entry->buffer != buffer) {
        pixman_region32_union(damage, damage, &entry->damage); // + o que mudou em OUTROS buffers
        continue;
    }
    // achou o buffer: 'damage' agora é tudo que mudou desde que ELE foi apresentado
    ...rotate...
}
// buffer nunca visto (age desconhecido) → damage = tela inteira:
pixman_region32_union_rect(damage, damage, 0,0, buffer->width, buffer->height); // :112
```
Ou seja: com 2 backbuffers alternando, cada um "atrasa" um frame; ao voltar a usar o buffer A, você precisa repintar o damage do frame N (quando A foi usado) **mais** o do frame N+1 (quando B foi usado). Se o buffer é novo/desconhecido, repinta tudo. Há um teto: se a região passar de `WLR_DAMAGE_RING_MAX_RECTS = 20` (`:9`) retângulos, colapsa para o bounding box (`:93`-`99`) — trade-off entre precisão e custo de iterar retângulos.

**Lição dispd (crítica):** se dispd fizer double-buffer (dois DIBs de backbuffer + BitBlt alternado, ou 2-3 buffers no flip DXGI), **NÃO basta repintar o damage do último frame** — cada backbuffer tem conteúdo velho de N frames atrás. Ou você mantém um damage-ring como este, ou (mais simples no começo) repinta a **união dos últimos N frames de damage** em cada buffer. Ignorar isso causa "fantasmas" (partes velhas aparecendo) ao alternar buffers.

### 4.3 Damage gerado por mutação da cena

Toda operação que muda a árvore chama `scene_node_update` (`wlr_scene.c:695`), que:
```c
scene_node_bounds(node, x, y, &update_region);  // damage = visibilidade ANTIGA ∪ bounds NOVOS
scene_update_region(scene, &update_region);      // recalcula visibilidade/oclusão dos nós afetados
scene_node_visibility(node, damage);
scene_damage_outputs(scene, damage);             // → scene_output_damage → schedule_frame
```
Mover uma janela suja **onde ela estava** e **onde ela está agora** (`node->visible` antigo ∪ bounds novo). É o clássico "damage the old and new rects". Isso fecha o loop: `set_position` → `scene_node_update` → damage → `schedule_frame` → (vblank) → `frame` → compõe só o damage → present.

### 4.4 Occlusion culling — não pintar o que está coberto

Em `build_state` (`:2518`-`2536`) e em `scene_entry_render` (`:1446`-`1450`), regiões **opacas** de nós de cima são subtraídas do que precisa ser pintado embaixo:
```c
scene_node_opaque_region(entry->node, entry->x, entry->y, &opaque);
pixman_region32_subtract(&background, &background, &opaque); // fundo não pintado sob janela opaca
```
E o blend mode do blit é escolhido por isso: se a região é totalmente opaca, usa `WLR_RENDER_BLEND_MODE_NONE` (cópia direta, sem alpha) em vez de `PREMULTIPLIED` (`wlr_scene.c:1520`-`1522`). Para dispd: se um terminal opaco cobre outro, não pinte o de baixo naquela área; e para superfícies 100% opacas use cópia (BitBlt `SRCCOPY`) em vez de alpha-blend (`AlphaBlend`) — muito mais barato.

### 4.5 O render pass software = GDI BitBlt com clip region

O renderer **pixman** (software) é o gêmeo do nosso GDI. `render/pixman/pass.c`:
```c
// render_pass_add_texture (:39)  — blitar uma surface
pixman_image_set_clip_region32(buffer->image, options->clip);   // == SelectClipRgn(hdc, damageRgn)
pixman_image_composite32(op, texture->image, mask, buffer->image,
    src_box.x, src_box.y, 0,0, dst_box.x, dst_box.y, w, h);     // == BitBlt / AlphaBlend
pixman_image_set_clip_region32(buffer->image, NULL);            // limpa o clip

// op = PIXMAN_OP_OVER (alpha)  ou  PIXMAN_OP_SRC (opaco, cópia direta)  — pass.c:29
```
`render_pass_add_rect` (`:202`) é igual para retângulos de cor (bordas/fundo): `pixman_image_composite32` de um `solid_fill` clipado. **Mapeamento direto para dispd:**
- `pixman_image_set_clip_region32(dst, clip)` → `SelectClipRgn(memDC, hDamageRgn)` no HDC do backbuffer, ou clipe manual dos retângulos.
- `PIXMAN_OP_SRC` (opaco) → `BitBlt(dst, ..., src, SRCCOPY)`.
- `PIXMAN_OP_OVER` (alpha premultiplicado) → `AlphaBlend(dst, ..., src, {AC_SRC_OVER, AC_SRC_ALPHA})` (ou `GdiAlphaBlend`). Cuidado: `AlphaBlend` espera alpha **premultiplicado** no DIB — igual ao pixman.
- Para cada retângulo de damage, você pode fazer um `BitBlt` por retângulo em vez de setar clip region (às vezes mais rápido com poucos rects).

---

## 5. Present / KMS (item 4) — como o frame composto chega à tela

Esta seção é o análogo do nosso "present backend" (BitBlt agora / DXGI flip depois). O caminho KMS do wlroots (`backend/drm/`) é o modelo mais completo. Todos os paths abaixo são do wlroots `0.21.0-dev`.

### 5.1 Pipeline de commit de output (genérico → backend)

- **`wlr_output_commit_state(output, state)`** (`types/output/output.c:807`) é o ponto de entrada. `state` é um `wlr_output_state` — o "estado pending do output" (buffer + damage + mode + etc.), montado pelos setters em `types/output/state.c` (`wlr_output_state_set_buffer` tranca o buffer; `set_damage` copia a região; `set_mode` marca `allow_reconfiguration = true`, que vira "modeset").
- Fluxo: `output_compare_state` (`:540`, tira campos no-op para não fazer modeset à toa) → `output_basic_test` (`:596`, valida src/dst box, formato) → `output_ensure_buffer` (`render.c:81`, renderiza um buffer preto se estiver ligando a tela) → `output->impl->commit` (vtable do backend).
- **`wlr_output_test_state`** (`:732`) é o mesmo *sem aplicar* — é como se testa se um estado (ex.: um buffer para direct scanout) é aceitável antes de commitá-lo. dispd deveria ter esse "test" antes de assumir um modo/flip.

### 5.2 Commit atômico DRM

`drm_connector_commit_state` (`backend/drm/drm.c:927`) monta um `wlr_drm_connector_state` e chama `drm_commit` (`drm.c:635`). O interface atômico (`backend/drm/atomic.c`):
- **Planos são fixos por CRTC**: `crtc->primary` e `crtc->cursor` são descobertos uma vez em `init_plane` (`drm.c:161`) lendo `DRM_PLANE_TYPE`. Overlays são planos extras.
- **Buffer → framebuffer DRM**: `drm_fb_import` (`backend/drm/fb.c:235`) → `get_fb_for_bo` (`fb.c:39`) chama `drmModeAddFB2WithModifiers`/`drmModeAddFB2` para registrar o buffer (GBM/dmabuf) como um FB scanout-able. Se falhar, o buffer é *poisoned* (`fb.c:132`) para não retentar todo frame.
- **Montagem atômica**: `atomic_connector_add` (`atomic.c:551`) faz `drmModeAtomicAddProperty` de: `CRTC_ID`, `MODE_ID`, `ACTIVE`, e por plano `FB_ID`, `SRC_*` (16.16 fixed), `CRTC_*`, `FB_DAMAGE_CLIPS`, `IN_FENCE_FD`.
- **Flags** (`atomic.c:649`): `DRM_MODE_ATOMIC_ALLOW_MODESET` (só em modeset), `DRM_MODE_ATOMIC_NONBLOCK`, e `DRM_MODE_PAGE_FLIP_EVENT` (para receber o callback de vblank).
- **`atomic_commit`** (`atomic.c:67`) = `drmModeAtomicCommit(fd, req, flags, page_flip_cookie)`.

Caminho **legacy** (contraste): `legacy_crtc_commit` (`backend/drm/legacy.c:85`) usa `drmModeSetCrtc` (modeset), `drmModeSetCursor`/`drmModeMoveCursor` (cursor HW), e `drmModePageFlip(fd, crtc, fb, DRM_MODE_PAGE_FLIP_EVENT, cookie)` para o flip. Sem damage (FB_DAMAGE_CLIPS é atomic-only). **Este legacy path é o análogo conceitual mais próximo do BitBlt: "aqui está o framebuffer inteiro, mostre-o".**

### 5.3 Page-flip completion — o vblank dirige o próximo frame

O fd do DRM é uma fonte de evento no loop; ao ficar readable, `handle_drm_event` → `drmHandleEvent` → **`handle_page_flip`** (`drm.c:2010`), chamado *no vblank quando o flip completa*:
1. **Libera o buffer antigo**: `drm_fb_move(&plane->current_fb, &plane->queued_fb)` (`drm.c:2042`) — o buffer recém-mostrado vira `current_fb`; o `current_fb` anterior é destrancado → **volta pro swapchain**. *É aqui que o buffer é reciclado.*
2. **Reporta present timing**: `wlr_output_send_present` (`output.c:878`) dispara `output->events.present` com `.when` = timestamp de vblank do kernel, `.refresh`, flags `WLR_OUTPUT_PRESENT_HW_CLOCK|VSYNC|...`. É o que alimenta o protocolo `presentation-time`.
3. **Dispara o próximo frame**: `wlr_output_send_frame` (`output.c:847`) → `output->events.frame` → o compositor renderiza o próximo. **O completion do present é o que anda o loop.**

**Lição dispd (crítica):** o *completion do present* deve dirigir o próximo frame. No BitBlt síncrono, o "flip" é imediato (BitBlt bloqueia até copiar), então você agenda o próximo frame por um timer alinhado ao refresh (`DwmGetCompositionTimingInfo`/`D3DKMTGetScanLine`/`DwmFlush` para sincronizar). No DXGI flip model, `IDXGISwapChain::Present(1,...)` + o waitable object (`GetFrameLatencyWaitableObject`) é o equivalente exato do page-flip event: espere nele, então componha e apresente o próximo.

### 5.4 Swapchain — double/triple buffer, acquire/release

`render/swapchain.c` (cap `WLR_SWAPCHAIN_CAP = 4`):
- `wlr_swapchain_acquire` (`:82`) retorna o primeiro slot livre com buffer (reuso) ou aloca um novo; marca `acquired`, registra listener em `buffer->events.release`, devolve **trancado** (`wlr_buffer_lock`).
- `slot_handle_release` (`:62`) limpa `acquired` quando o buffer é liberado — e isso acontece no page-flip completion (§5.3). Enquanto o frame N está em scanout (`current_fb`) e N+1 está na fila (`queued_fb`), o compositor pode adquirir um **terceiro** slot para N+2 — daí triple buffering. Não há contador de "age" no slot; a profundidade é o refcount lock/unlock.

**Lição dispd:** exponha um `dispd_swapchain` com 2 (BitBlt) ou 2-3 (DXGI) buffers, `acquire`/`release`, e libere o buffer só quando o present dele completar. Isso, combinado com o damage-ring (§4.2), é o mínimo para não ter tearing/fantasmas.

### 5.5 Cursor: hardware plane vs software

- **HW cursor** (`types/output/cursor.c:291 output_cursor_attempt_hardware`): renderiza o cursor num swapchain próprio e o programa num **plano de cursor** dedicado — mover o cursor não recompõe nada. Fallback para software se maior que o tamanho de HW permitido ou se travado.
- **SW cursor** (`cursor.c:91 wlr_output_add_software_cursors_to_render_pass`): compõe o cursor *dentro* do render pass principal, clipado ao damage (o cursor gera seu próprio damage ao mover, via `output->events.damage`).

**Lição dispd:** comece com **cursor de software** — desenhe o cursor por último no backbuffer, e gere damage no retângulo velho + novo do cursor a cada movimento (é a maior fonte de damage num terminal ocioso). Evolua para uma **camada/overlay** (DXGI overlay plane ou uma layered window) quando quiser mover o cursor sem recompor.

### 5.6 Damage no present (FB_DAMAGE_CLIPS)

`create_fb_damage_clips_blob` (`atomic.c:157`) transforma a região de damage num blob de retângulos e o anexa ao plano primário (`atomic.c:592`). É uma **dica** para o kernel/scanout re-ler só a área suja (economia de banda/energia). No BitBlt do dispd, o equivalente é literalmente só copiar os retângulos sujos (não a tela inteira). No DXGI flip, é o `pDirtyRects` de `IDXGISwapChain1::Present1` — **use-o**: passe o damage como dirty rects e o DWM/scanout copia menos.

---

## 6. Input routing / seat (item 5)

O **seat** (`wlr_seat`) modela "um usuário": até um teclado, um ponteiro, um toque. O compositor recebe input cru e decide para qual surface mandar. Em tinywl:

- **Ponteiro**: `wlr_cursor` agrega os devices; `server_cursor_motion` (`tinywl.c:496`) chama `wlr_cursor_move` e depois `process_cursor_motion` (`:453`), que faz **hit-testing** com `desktop_toplevel_at` → `wlr_scene_node_at` (§6.1) e roteia:
  ```c
  wlr_seat_pointer_notify_enter(seat, surface, sx, sy);  // dá "pointer focus" à surface sob o cursor
  wlr_seat_pointer_notify_motion(seat, time, sx, sy);
  ```
- **Botões**: `server_cursor_button` (`:528`) → `wlr_seat_pointer_notify_button`; no press, foca a janela (`focus_toplevel`).
- **Teclado**: `keyboard_handle_key` (`:201`) primeiro testa keybindings do compositor (Alt+F1 etc.); se não consumir, encaminha: `wlr_seat_keyboard_notify_key(seat, time, keycode, state)` (`:231`).
- **Foco de teclado**: `focus_toplevel` (`:113`) desativa a surface anterior, levanta a nova (`wlr_scene_node_raise_to_top`), e `wlr_seat_keyboard_notify_enter` — a partir daí o wlroots roteia as teclas para o cliente certo automaticamente.

### 6.1 Hit-testing pela árvore

`wlr_scene_node_at(node, lx, ly, &nx, &ny)` (`wlr_scene.c:1384`) faz `scene_nodes_in_box` numa box 1×1 no ponto e devolve o **nó topmost** ali, já com coordenadas locais à surface. tinywl (`desktop_toplevel_at:360`) sobe do nó até o `wlr_scene_tree` cujo `node.data` aponta pro toplevel. **dispd faz igual:** hit-test na árvore de surfaces top→bottom para achar qual terminal está sob o cursor.

### 6.2 Enter/leave e serials (o mecanismo interno)

`wlr_seat_pointer_enter` (`types/seat/wlr_seat_pointer.c:159`) é o núcleo do foco de ponteiro:
```c
if (focused_surface == surface) return;               // já focado, no-op
if (focused_client && focused_surface)
    seat_client_send_pointer_leave_raw(...);          // wl_pointer.leave à surface antiga (serial novo)
if (client && surface) {
    uint32_t serial = wlr_seat_client_next_serial(client);
    wl_pointer_send_enter(resource, serial, surface->resource, sx, sy); // enter à nova (serial)
}
seat->pointer_state.focused_client = client;          // atualiza foco
seat->pointer_state.focused_surface = surface;
```
Cada evento carrega um **serial** monotônico. Serials são o mecanismo anti-abuso do Wayland: um cliente só pode iniciar move/resize/set_cursor citando um serial de um input recente (senão qualquer app moveria janelas à vontade). tinywl comenta isso em `xdg_toplevel_request_move` (`:754`).

**Lição dispd:** modele um seat: rastreie `focused_surface` de ponteiro e de teclado separadamente; envie enter/leave ao trocar; use serials (ou tokens) para autorizar operações iniciadas pelo cliente (move/resize interativo). Raw Input entrega os eventos crus; o roteamento (hit-test → surface focada → mensagem ao app) é responsabilidade do dispd, igual ao seat.

---

## 7. O protocolo compositor↔cliente (item 6) — o que o app faz para aparecer

Conceitualmente, para um cliente aparecer na tela:

1. **Cria uma `wl_surface`** (via `wl_compositor`) — um retângulo de conteúdo, ainda **sem papel** (role) e invisível.
2. **Dá um role à surface** — via xdg-shell: `xdg_surface` + `xdg_toplevel` (janela de app) ou `xdg_popup` (menu). Sem role, a surface não pode ser mapeada.
3. **Initial commit vazio** → o compositor responde com um `configure` (tamanho sugerido). tinywl: `xdg_toplevel_commit` (`:694`) vê `base->initial_commit` e responde `wlr_xdg_toplevel_set_size(.., 0,0)` ("escolha você o tamanho"). O cliente **precisa** de um `configure` antes de mapear.
4. **Cliente desenha no buffer no tamanho acordado, `attach` + `damage` + `commit`** → dispara o evento `map` (surface fica visível). tinywl: `xdg_toplevel_map` (`:673`) insere na lista e foca.
5. Daí em diante, o ciclo é: cliente desenha → `commit` → compositor compõe → `frame done` → cliente desenha o próximo.

O "handshake configure/ack" é o que garante que **cliente e compositor concordam no tamanho antes de mostrar** — base do layout atômico (§8).

**Mapeamento para o boundary apps↔dispd:** defina um protocolo mínimo (ver `docs/PROTOCOLO.md`) com, no mínimo:
- `create_surface` → devolve um id de surface.
- `attach_dib(surface, dib_handle)` + `damage(surface, rect)` + `commit(surface)` — o cliente publica um frame.
- `configure(surface, w, h, serial)` (dispd→app) + `ack_configure(surface, serial)` (app→dispd) — negociação de tamanho.
- `frame_done(surface)` (dispd→app) — throttle ao refresh.
- `release_dib(surface, dib_handle)` (dispd→app) — devolve o buffer.
- eventos de input: `pointer_enter/motion/button/leave`, `key`, `keyboard_enter/leave`, todos com serial.

O DIB é o `wl_buffer`; `commit` é o ponto atômico; `release_dib` é o `wl_buffer.release`. Isso é uma tradução 1:1 do modelo Wayland para memória compartilhada Win32 (DIB section em shared memory / `CreateDIBSection` sobre um file mapping compartilhado entre app e dispd).

---

## 8. Estado atômico / no-tearing (item 7)

Há **dois** níveis de atomicidade, e o dispd precisa dos dois:

### 8.1 Atomicidade por-surface (o commit do Wayland)

O cliente nunca mostra um frame meio-desenhado porque o compositor **só consome buffers committed**. `wl_surface.attach` + `damage` + `commit` são acumulados no estado pending e aplicados juntos (`surface_commit_state`, §2). Não existe "o compositor leu o buffer enquanto o cliente ainda desenhava" — o cliente só faz `commit` quando o frame está completo, e o compositor lê o buffer *committed*, não o buffer "ao vivo". Isso é a diferença estrutural para o X11 (§9): lá o compositor *observa* damage e *amostra* o pixmap da janela, que pode estar meio-atualizado.

### 8.2 Atomicidade de layout (a transação do sway)

Quando várias janelas mudam de tamanho ao mesmo tempo (trocar de workspace, abrir uma janela num tiling), você não quer um frame onde a janela A já redimensionou e a B não. O sway resolve com um **sistema de transações** (`sway/desktop/transaction.c`):

- `transaction_commit` (`:820`): para cada view cujo tamanho mudou, envia um `configure` com um **serial** (`view_configure`), incrementa `num_waiting`, e **salva o buffer atual** (`view_save_buffer`, `:845`) — o conteúdo antigo continua sendo mostrado até o novo chegar.
- Espera todas as views responderem: `set_instruction_ready` por serial (`:887`) decrementa `num_waiting`. Um **timeout** (`handle_timeout`, `:777`, default ~200ms) evita travar se um cliente não responder.
- `transaction_progress` (`:756`): quando `num_waiting == 0`, `transaction_apply` (`:716`) faz `memcpy` do estado da instrução para `node->current` **de uma vez** (swap atômico), re-arranja a cena e re-renderiza.

Resultado: um resize multi-janela vira **uma** mudança visual coerente. dwl faz uma versão crua disso: em `rendermon` (`dwl.c:2152`) ele **pula** o commit do frame enquanto qualquer cliente tiled tem resize pendente (`if (c->resize && !c->isfloating && ...) goto skip;`) — não mostra o frame até todo mundo estar no tamanho certo.

**Lição dispd:** para o boundary apps↔dispd, adote o handshake configure/ack + save-buffer. Se dispd tiver tiling/multi-janela, junte mudanças de layout numa transação e só aplique quando todas as surfaces tiverem committed no tamanho novo (com timeout). Para no-tearing no *scanout*, use present sincronizado a vblank (BitBlt alinhado ao DwmFlush, ou `Present(1,...)` no flip model) — nunca copie para o front buffer no meio do scan.

---

## 9. O modelo X11 de redirection (Picom) — o contramodelo

Picom é um compositor **standalone** para X11 (não é o display server; o X server é). Ele existe *por cima* do X11, e por isso é "after-the-fact" e sujeito a races — exatamente o que Wayland/dispd evitam. Vale entender para saber **o que NÃO fazer**. (picom `6d67682`, paths em `src/`.)

### 9.1 Redirection: assumir a composição

- `init_overlay` (`src/picom.c:1067`): `xcb_composite_get_overlay_window` pega uma janela especial acima de todas; picom desenha o resultado composto nela. Ela é feita click-through (SHAPE vazio).
- `redirect_start` (`src/picom.c:1177`): a chamada load-bearing é `xcb_composite_redirect_subwindows(root, MANUAL)` (`:1187`) — diz ao X server para **parar de pintar as janelas direto no framebuffer** e redirecioná-las para **pixmaps offscreen**. A partir daí, picom é o único que leva pixels à tela.

### 9.2 Pixmap de janela → textura

- `win_process_image_flags` (`src/wm/win.c:445`): `xcb_composite_name_window_pixmap` (`:470`) dá um handle para o pixmap offscreen atual da janela; `bind_pixmap` (`:484`) o liga a uma textura GL (`glXBindTexImageEXT`) ou a um `Picture` XRender.
- **Gotcha estrutural:** o handle do NameWindowPixmap **fica stale a cada resize** — o X server realoca o pixmap. Picom precisa re-buscar (`WIN_FLAGS_PIXMAP_STALE`, set a partir de `SIZE_STALE`, `win.c:370`) e liberar o antigo *antes* de religar (senão o driver NVIDIA quebra, `win.c:482-483`). É uma corrida constante contra o cliente redimensionando.

### 9.3 Damage via XDamage

- `xcb_damage_create(..., XCB_DAMAGE_REPORT_LEVEL_NON_EMPTY)` por janela (`win.c:1265`).
- `ev_damage_notify` → `repair_win` (`src/event.c:625`): `xcb_damage_subtract` drena a região suja acumulada no servidor para uma XFixes region, `x_fetch_region` a lê de volta, e union em `w->damaged`.
- **Gotcha:** você **tem** que drenar todos os eventos e fazer `DamageSubtract` antes do loop dormir (`handle_x_events`, `src/picom.c:1300`), senão o modelo incremental do XDamage para de entregar eventos e o picom **congela**.

### 9.4 O loop e o present

- É um event loop libev; quase todo evento chama `queue_redraw` → `schedule_render` (`picom.c:301`), que usa estatísticas de vblank (Present extension) para começar o render "tarde o suficiente" para caber antes do próximo vblank (mesmo truque do sway, §5.3).
- `draw_callback_impl` (`picom.c:1570`) → `renderer_render` (`src/renderer/renderer.c:426`): `command_builder_build` gera comandos bottom-to-top (shadow/blur/blit com opacity/corner), `commands_cull_with_damage` (`src/renderer/damage.c:331`) subtrai regiões opacas (occlusion) e clipa ao damage, `backend_execute` roda os comandos, e `ops.present` apresenta.
- **buffer age** aqui é obtido do backend (`glXQueryDrawable(GLX_BACK_BUFFER_AGE_EXT)`, `xcb_present`) e o damage é computado *diffando* o layout atual contra o de `buffer_age` frames atrás (`layout_manager_damage`) — mesma ideia do damage-ring, implementação diferente.
- Present: xrender usa `xcb_present_pixmap` + espera `PRESENT_COMPLETE_NOTIFY` (vsync); GL usa `glXSwapBuffers` com swap interval. Sem vsync, o xrender desenha **direto no front buffer** (`xrender.c:821`) — rápido e **tearing**.

### 9.5 Por que é inerentemente "after-the-fact"

No X11 o app desenha na sua janela quando *ele* quer; o servidor só emite `DamageNotify` **async**; picom descobre *depois* que o cliente já desenhou, e então, no *seu próprio* schedule, re-lê um pixmap que **pode estar meio-atualizado**, e janelas diferentes estão em fases de update não relacionadas. Não há nada amarrando "o cliente terminou um frame coerente" a "o compositor pega esse frame". Toda a maquinaria do picom (redirect, XDamage, DamageSubtract-antes-de-dormir, rebind de pixmap no resize, workarounds de vsync, unredirect-if-possible) existe **só para remendar** o fato de o X11 nunca ter sido desenhado para um compositor sincronizado com os frames dos clientes.

**Lição para o dispd:** **não copie o modelo X11.** O modelo Wayland (`commit` atômico + o compositor é o display server) é o certo, e é o que o dispd já é por construção (dono da tela, apps publicam DIBs via `commit`). O único aprendizado útil do picom é técnico: buffer-age via diff de layout, occlusion culling top-down, e o "unredirect/direct-scanout when a single opaque window covers everything" (§5.2 direct scanout) — quando um único terminal fullscreen opaco cobre tudo, dispd pode pular a composição e apresentar o DIB dele direto.

---

## 10. Weston (software renderer) + Mutter/KWin (arquitetura)

### 10.1 Weston — o repaint loop dirigido por vblank

Weston **não** roda thread de render; um único `timerfd` por compositor dirige tudo pelo `wl_event_loop`. (Weston HEAD `29d5739`, paths em `libweston/`.) A máquina de estados `enum weston_repaint_status` (`include/libweston/libweston.h:405`): `NOT_SCHEDULED → BEGIN_FROM_IDLE → SCHEDULED → AWAITING_COMPLETION`.

- `weston_output_schedule_repaint()` (`compositor.c:4948`): marca `repaint_needed`; se ocioso, instala uma idle source `idle_repaint`.
- `idle_repaint()` (`:4721`) → `output->start_repaint_loop` → (DRM) `drm_output_start_repaint_loop` (`backend-drm/drm.c:1038`) faz um `drmWaitVBlank` instantâneo para pegar a base de tempo do vblank, e chama `weston_output_finish_frame`.
- `weston_output_finish_frame()` (`compositor.c:4577`): dado o timestamp de vblank, calcula `next_present = stamp + refresh_nsec` e `next_repaint = weston_output_repaint_from_present(...)` (`:4229`) — que **subtrai a "repaint window"** (`weston_output_repaint_msec`, `:4174`), ou seja, agenda o repaint da CPU para acontecer *uma janela antes* do vblank alvo (mesmo truque de latência do sway/§5.3). Arma o timerfd (`weston_repaint_timer_arm`, `:4283`), coalescendo múltiplos outputs numa só chamada.
- `output_repaint_timer_handler()` (`:4448`) → por output `weston_output_repaint()` (`:3976`): reconstrói a view list (scene graph), occlusion (`output_update_visibility`), atribui planos KMS (`output_assign_planes`), acumula damage (`output_accumulate_damage`), e chama o hook do backend `output->repaint(output)`. Depois dispara os `wl_callback` de frame das surfaces visíveis (`wl_callback_send_done`, `:4106`).
- **Fecha o loop**: o page-flip completa (`atomic_flip_handler`, `backend-drm/kms.c:2556`) → `drm_output_update_complete` (`drm.c:466`) → `weston_output_finish_frame` de novo com o timestamp de vblank do HW. Esse é o ciclo permanente.

### 10.2 Weston — refcount de buffer e release precoce

`weston_buffer_reference()` (`compositor.c:3175`) é um refcount de dois níveis: `busy_count` (`BUFFER_MAY_BE_ACCESSED`) e `passive_count` (`BUFFER_WILL_NOT_BE_ACCESSED`). Quando `busy_count` cai a zero e o `wl_buffer` ainda existe, envia **`wl_buffer_send_release()`** (`:3212`) — "pode reusar". O detalhe fino: em `output_accumulate_damage()` (`:3599`), depois de o renderer consumir a surface no frame, Weston **rebaixa** a referência para `WILL_NOT_BE_ACCESSED` (`:3630`) e solta o buffer-release ref — disparando o `wl_buffer.release` o mais cedo possível, para o cliente conseguir **single-buffering**. (Pulado para `multi_backend` e surfaces que o backend quer manter para direct scanout.)

### 10.3 Weston — o renderer pixman (o análogo mais limpo de um compositor GDI)

`libweston/pixman-renderer.c` é a **melhor referência de compositor puro-software** que existe — leia-a como espelho do dispd/GDI. Estruturas-chave (`:45`): `pixman_output_state{ shadow_image, hw_buffer, renderbuffer_list }`, `pixman_renderbuffer{ damage, image, stale }` (um renderbuffer = **um backbuffer com seu próprio damage acumulado**).

- **Attach = zero-copy**: `pixman_renderer_attach()` (`:689`) embrulha a memória SHM do cliente direto com `pixman_image_create_bits(fmt, w, h, wl_shm_buffer_get_data(shm), stride)` (`:755`). Sem cópia — compõe direto da memória do cliente (guardado por `wl_shm_buffer_begin/end_access`). Só suporta SHM e cor sólida.
- **Entrada**: `pixman_renderer_repaint_output()` (`:602`): primeiro **acumula `output_damage` no `rb->damage` de *cada* renderbuffer** (`:613`) — é assim que rastreia o damage por-backbuffer numa swap chain double/triple (uma região suja agora precisa ser repintada em cada buffer físico na próxima vez que ele for reusado — **é o buffer age, feito à mão**). Depois:
  - Com shadow buffer: `repaint_surfaces` compõe no `shadow_image`, então `copy_to_hw_buffer(output, &rb->damage)` (`:632`) blita **só o damage acumulado** shadow → hw_buffer.
  - Sem shadow: `repaint_surfaces(output, &rb->damage)` compõe direto no hw_buffer, usando o damage por-buffer.
  - `pixman_region32_clear(&rb->damage)` (`:639`).
- **`repaint_surfaces()` (`:500`)**: itera a z-order list **em reverso** (painter's algorithm, back-to-front), só nós no plano primário.
- **`draw_paint_node()` (`:440`)**: `repaint = pnode->visible ∩ damage` (`:470`); vazio → skip (o clip de damage por-surface).
- **`draw_node_translated()` (`:349`)**: divide a surface em **região opaca (`PIXMAN_OP_SRC`, sem blend)** e região de blend (`whole − opaque`, `PIXMAN_OP_OVER`) — pixels opacos pulam o caminho de alpha.
- **`repaint_region()` (`:279`) — o blit limitado a damage (o coração)**: `target = shadow ? shadow_image : hw_buffer`, então **`pixman_image_set_clip_region32(target, repaint_output)` (`:301`)** para o pixman só tocar os retângulos sujos, monta o `pixman_transform`, escolhe filtro NEAREST/BILINEAR, cria máscara de alpha se `alpha < 1.0`, e compõe via `pixman_image_composite32` (`:186`). Clip resetado a NULL depois.
- **Double buffer (DRM+pixman)**: `drm_output_render_pixman()` (`backend-drm/drm.c:533`): `current_image ^= 1` alterna entre **dois** dumb buffers KMS (`dumb[0/1]`/`renderbuffer[0/1]`), compõe no atual e devolve o FB. Fast path: `drm_output_render` (`:548`) **pula o renderer inteiro** e re-referencia o FB atual quando não há damage no plano primário (`:589`) — o "nada mudou, não repinta".

**Este é, linha por linha, o design que dispd deve seguir no modo BitBlt**: shadow buffer (composição) + 2 hw buffers (scanout) + damage acumulado por-buffer + clip region + OP_SRC/OP_OVER por região opaca. Troque `pixman_image_composite32` por `BitBlt`/`AlphaBlend` e `set_clip_region32` por `SelectClipRgn`.

### 10.4 Weston — presentation feedback

Cliente pede `wp_presentation.feedback`; na conclusão do flip, `weston_presentation_feedback_present()` (`compositor.c:1043`) envia `presented(tv_sec, tv_nsec, refresh_nsec, seq, flags)` (`:1081`) com o timestamp de vblank do HW, o `refresh_nsec` do modo, a sequência MSC e as flags KMS (`VSYNC|HW_COMPLETION|HW_CLOCK`). One-shot. É o mesmo mecanismo do wlroots (§5.3). dispd pode expor timing análogo com `DwmGetCompositionTimingInfo`/`IDXGISwapChain::GetFrameStatistics`.

### 10.5 Mutter (GNOME) — Clutter scene graph + frame clock por monitor

- **Scene graph = Clutter** (árvore retida de atores sobre Cogl, a abstração GL). `MetaCompositor` gerencia; cada janela vira um `MetaWindowActor` no stage. O desktop é **um único `ClutterStage`** cobrindo a união de todos os monitores. ([mutter compositor.c](https://github.com/endlessm/mutter/blob/master/src/compositor/compositor.c))
- **`ClutterStageView` ≈ um monitor**: guarda o framebuffer on-screen daquele monitor, shadow framebuffers opcionais, rotação, e — crucial — **o frame clock do monitor**. ([Splitting up the Frame Clock](https://blogs.gnome.org/shell-dev/2020/07/02/splitting-up-the-frame-clock/))
- **`ClutterFrameClock` por-view**: substituiu o clock global. Cada clock dirige um output com seu refresh, ciclando **scheduled → dispatch (update/paint) → presented**, onde o callback de page-flip/presentation realimenta o tempo real e o clock agenda o próximo update mirando o próximo vblank menos a duração estimada de render. Resultado: um monitor 144 Hz e um 60 Hz repintam em cadências independentes.
- **Backend native/KMS** (`src/backends/native/`): `MetaRendererNative`, cada framebuffer é um `MetaOnscreenNative` (CoglOnscreen), KMS via `MetaKms`/`MetaGpuKms`; page-flips dirigem a fase "presented" do frame clock.
- **Direct scanout**: buffer fullscreen scanout-compatível vai direto a um plano KMS, **bypassando Clutter/Cogl**; fallback para composição GL se o flip falhar. ([Gracefully handle page flipping direct scanouts failing](https://mail.gnome.org/archives/commits-list/2021-July/msg00289.html))

### 10.6 KWin (KDE) — Item/Scene tree + RenderLoop + RenderBackend

- **Scene = Item tree (estilo QtQuick, grafo retido)**: `WorkspaceScene`; cada janela é um `WindowItem` composto de `ShadowItem` + `DecorationItem` + `SurfaceItem` (`SurfaceItemWayland`/`X11`/`Internal`). Ponto de design: **um `Item` não renderiza nada** — ele gera nós de render que o `ItemRenderer` (`ItemRendererOpenGL`/`ItemRendererQPainter`) percorre. ([Scene Items in KWin](https://blog.vladzahorodnii.com/2021/04/12/scene-items-in-kwin/))
- **`Compositor` + `RenderLoop`**: o `RenderLoop` "avisa o compositor quando é boa hora de pintar o próximo frame". No **Wayland cada output tem seu `RenderLoop`**. ([Compositing Scheduling in KWin](https://blog.vladzahorodnii.com/2020/12/10/compositing-scheduling-in-kwin-past-now-and-future/))
- **Agendamento off-vblank**: rastreia `lastPresentationTimestamp`/`nextPresentationTimestamp`, faz **duas predições de tempo de render** (latência desejada configurada + duração medida dos ciclos anteriores) e usa a **maior**, agendando a composição o mais perto possível *antes* do vblank; anima efeitos até o timestamp previsto. Na conclusão, `RenderLoopPrivate::notifyFrameCompleted(...)` (timestamp do page-flip do `DrmOutput`) avança o loop. Suporta triple-buffering.
- **`RenderBackend` + caminho software**: OpenGL (`EglGbmBackend`/`ItemRendererOpenGL`) **ou software (`QPainterBackend`/`ItemRendererQPainter`)** para máquinas sem GL. No DRM: `DrmQPainterBackend` (software, `QPainter` em dumb buffers KMS) vs `EglGbmBackend`, ambos via `DrmOutput`/`DrmPipeline` + atomic KMS. **O caminho QPainter do KWin é o análogo do pixman-renderer do Weston** — e ambos são o análogo do dispd/GDI. ([drm_backend.cpp](https://github.com/KDE/kwin/blob/master/src/backends/drm/drm_backend.cpp))

### 10.7 Síntese cross-compositor (para o caso software/GDI)

Os cinco compositores compartilham o mesmo esqueleto: **scene graph retido** (paint-node/view list do Weston; atores Clutter do Mutter; Item tree do KWin; `wlr_scene`) + **regiões de damage clipadas por-surface e por-output** + **agendamento de frame ancorado no vblank/page-flip com um "render-window" de antecedência** + **presentation feedback** com o timestamp real de vblank + refresh + MSC. Para dispd, **`libweston/pixman-renderer.c` é a referência mais limpa**: embrulhe o buffer do cliente como imagem (zero-copy), `set_clip_region32` ao damage, pinte back-to-front com OP_SRC (opaco) / OP_OVER (blend), mantenha um shadow buffer de composição, e blite só o damage acumulado por-backbuffer no scanout double-buffered. O `QPainterBackend` do KWin é a mesma ideia com `QPainter` sobre dumb buffers KMS — exatamente o que `BitBlt` sobre DIBs será no dispd.

---

## 11. Lições para o dispd — mapeamento concreto

Mapa direto dos mecanismos lidos para a arquitetura do dispd (superfícies de terminal com DIB → backbuffer → BitBlt/DXGI). Ordenado por prioridade de implementação.

### Fase 1 — o mínimo correto (BitBlt + software, análogo ao renderer pixman)

| Mecanismo (fonte real) | O que fazer no dispd |
|---|---|
| Event loop dirigido por damage (`wlr_output_schedule_frame`, `output_frame`) | dispd não tem loop de render busy. `dispd_damage(rect)` marca sujo e agenda um frame; sem damage, nada é desenhado. |
| Estado pending vs current (`wlr_surface_state.pending`, `surface_commit_state`) | Cada `dispd_surface` tem estado pending (DIB anexado, damage) aplicado atomicamente em `commit`. |
| Scene graph (`wlr_scene_node`, `scene_entry_render`) | Uma árvore de `dispd_surface` com posição/z-order. `dispd_compose()` percorre top→bottom. Nunca render manual espalhado. |
| Damage clip no blit (`pixman_image_set_clip_region32` + `composite32`, `pass.c`) | No HDC do backbuffer: `SelectClipRgn(memDC, damageRgn)` e `BitBlt`/`AlphaBlend` por surface. Ou um `BitBlt` por retângulo de damage. |
| Blend por opacidade (`PIXMAN_OP_SRC` vs `OVER`, `pass.c:29`) | Surface opaca → `BitBlt(SRCCOPY)`. Surface com alpha → `AlphaBlend` (alpha **premultiplicado** no DIB). |
| Occlusion culling (`scene_node_opaque_region` → `subtract`) | Não pinte a área de um terminal coberta por outro opaco. Subtraia regiões opacas de cima do damage de baixo. |
| Fundo só onde aparece (`build_state:2547`) | Pinte o wallpaper/preto só na parte do damage não coberta por surfaces opacas. |
| Cursor de software no fim do pass (`add_software_cursors_to_render_pass`) | Desenhe o cursor por último no backbuffer. Gere damage no rect **velho ∪ novo** do cursor a cada movimento. |
| Frame throttle (`send_frame_done`) | Após compor, envie `frame_done` às surfaces visíveis; o app só desenha o próximo frame ao receber. |

### Fase 2 — double/triple buffer sem fantasmas

| Mecanismo | O que fazer |
|---|---|
| **Buffer age / damage ring** (`wlr_damage_ring_rotate_buffer`) | **Crítico.** Com 2 backbuffers, cada um repinta a **união dos últimos 2 frames de damage**. Mantenha damage acumulado por-buffer (ou um ring simples). Ignorar = fantasmas ao alternar. |
| Swapchain acquire/release (`wlr_swapchain_acquire`, release no page-flip) | `dispd_swapchain` de 2–3 DIBs de backbuffer. Libere um buffer só quando o present dele completar. |
| Present dirige o próximo frame (`handle_page_flip` → `send_frame`) | Agende o próximo frame no completion do present (BitBlt é síncrono → timer alinhado a `DwmFlush`/vblank; DXGI → waitable object). |
| Render tardio (`sway output_repaint_timer_handler`, `max_render_time`) | Não componha logo no vblank; espere até `vblank − custo_de_render` para pegar o conteúdo mais fresco do terminal. Menos latência. |

### Fase 3 — DXGI flip (o "present backend" definitivo) + atomicidade

| Mecanismo | O que fazer |
|---|---|
| Commit atômico de output (`wlr_output_commit_state`/`test_state`) | `dispd_present(state)` com um `test` antes de assumir modo/flip. Estado = buffer + dirty rects + mode. |
| Present com dirty rects (`FB_DAMAGE_CLIPS`, `IDXGISwapChain1::Present1 pDirtyRects`) | Passe o damage como `pDirtyRects`/`pScrollRect` no flip — o scanout copia menos. |
| Vsync sem tearing (`Present(1,...)` + `GetFrameLatencyWaitableObject`) | Waitable swapchain (`DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT`) é o análogo exato do page-flip event. Espere nele antes de compor. |
| Cursor em overlay (KMS cursor plane) | Evolua o cursor de software para um overlay/hardware plane (DXGI overlay) — mover sem recompor. |
| Direct scanout de janela única (`scene_entry_try_direct_scanout`, picom unredirect) | Terminal fullscreen opaco cobrindo tudo → pule a composição, apresente o DIB dele direto (flip). |

### Fase 4 — layout atômico multi-janela (se dispd virar WM)

| Mecanismo | O que fazer |
|---|---|
| Transação de layout (`sway transaction_commit`/`apply`, `view_save_buffer`) | Junte mudanças de layout, envie `configure(serial)` a cada surface, **guarde o DIB antigo** exibindo-o até o novo chegar, aplique tudo quando todas responderem (timeout ~200ms). |
| Skip cru (`dwl rendermon: goto skip`) | Versão mínima: não apresente o frame enquanto alguma surface tiled tiver resize pendente. |
| Handshake configure/ack (`xdg_surface` initial commit → configure) | O app só mostra buffer no tamanho que dispd mandou; dispd só mapeia após o ack + primeiro commit. |

### O boundary apps↔dispd (o "protocolo", item 6)

Traduza o modelo Wayland para memória compartilhada Win32 (DIB section sobre file mapping compartilhado):

- `create_surface` → id; `attach_dib(surf, dib)` + `damage(surf, rect)` + `commit(surf)` (ponto atômico).
- `configure(surf, w, h, serial)` (dispd→app) / `ack_configure(surf, serial)` (app→dispd).
- `frame_done(surf)` (dispd→app, throttle); `release_dib(surf, dib)` (dispd→app, reuso).
- input: `pointer_enter/motion/button/leave`, `key`, `kbd_enter/leave`, com **serial** (autoriza move/resize iniciado pelo cliente).

O DIB = `wl_buffer`; `commit` = ponto atômico; `release_dib` = `wl_buffer.release`; o handshake configure/ack = xdg-shell. É uma tradução 1:1 — o dispd é, essencialmente, um compositor Wayland cujo transporte é DIB+shared-memory em vez de socket+wl_shm, e cujo scanout é DXGI/GDI em vez de KMS.

---

## 12. Índice de referência — arquivo:função (para reler o código)

**wlroots — scene graph / composição (o núcleo):**
- `types/scene/wlr_scene.c:2286 wlr_scene_output_build_state` — monta o frame (render list, damage, occlusion, blit, cursor).
- `:2162 wlr_scene_output_commit` / `:2156 wlr_scene_output_needs_frame` — gate de "precisa renderizar?".
- `:1422 scene_entry_render` — cada nó → comandos de render clipados ao damage.
- `:356 scene_output_damage` / `:375 damage_whole` — funil de damage + schedule_frame.
- `:695 scene_node_update` / `:623 scene_update_region` — mutação da árvore gera damage (velho ∪ novo).
- `:1159 scene_buffer_get_texture` — upload lazy buffer→textura.
- `:1384 wlr_scene_node_at` — hit-testing.
- `:2032 scene_entry_try_direct_scanout` — bypass de composição p/ janela única.

**wlroots — damage ring / buffer age:**
- `types/wlr_damage_ring.c:78 wlr_damage_ring_rotate_buffer` — buffer age (o que repintar por backbuffer).
- `:32 wlr_damage_ring_add` / `:44 add_whole`.

**wlroots — renderer software (o análogo GDI):**
- `render/pixman/pass.c:39 render_pass_add_texture` — blit clipado (`clip_region32` + `composite32`).
- `:202 render_pass_add_rect` — retângulo de cor clipado.
- `:29 get_pixman_blending` — OP_OVER (alpha) vs OP_SRC (opaco).

**wlroots — surface/buffer lifecycle:**
- `types/wlr_compositor.c:575 surface_handle_commit` / `:511 surface_commit_state` — ponto atômico.
- `:407 surface_apply_damage` — upload + update in-place do damage.
- `:571` — `wlr_buffer_unlock` (release imediato pós-upload).
- `types/scene/surface.c:361 handle_scene_surface_surface_commit` / `:279 surface_reconfigure` — ponte protocolo→cena.

**wlroots — present / KMS (o "present backend"):**
- `types/output/output.c:807 wlr_output_commit_state` / `:732 wlr_output_test_state`.
- `backend/drm/drm.c:927 drm_connector_commit_state` / `:635 drm_commit`.
- `backend/drm/atomic.c:67 atomic_commit` / `:551 atomic_connector_add` / `:157 create_fb_damage_clips_blob`.
- `backend/drm/legacy.c:85 legacy_crtc_commit` (drmModeSetCrtc/PageFlip — análogo do BitBlt).
- `backend/drm/drm.c:2010 handle_page_flip` — completion → libera buffer + present timing + próximo frame.
- `render/swapchain.c:82 wlr_swapchain_acquire` / `:62 slot_handle_release`.
- `types/output/cursor.c:291 output_cursor_attempt_hardware` / `:91 add_software_cursors_to_render_pass`.

**wlroots — seat / input:**
- `types/seat/wlr_seat_pointer.c:159 wlr_seat_pointer_enter` / `:439 notify_enter` — foco de ponteiro + serials.
- tinywl `tinywl.c:453 process_cursor_motion` / `:113 focus_toplevel` / `:201 keyboard_handle_key`.

**tinywl — o esqueleto:**
- `tinywl/tinywl.c:887 main` (montagem) / `:573 output_frame` (loop) / `:673 xdg_toplevel_map` / `:694 xdg_toplevel_commit` (handshake configure).

**dwl — WM dwm-like:**
- `dwl.c:2152 rendermon` (frame + skip por resize pendente).

**sway — transação (layout atômico):**
- `sway/desktop/transaction.c:820 transaction_commit` / `:716 transaction_apply` / `:756 transaction_progress` / `:777 handle_timeout`.
- `sway/desktop/output.c:270 output_repaint_timer_handler` / `:313 handle_frame` (render tardio / max_render_time).

**picom — modelo X11 (contramodelo):**
- `src/picom.c:1177 redirect_start` / `:1067 init_overlay`.
- `src/wm/win.c:445 win_process_image_flags` (NameWindowPixmap rebind).
- `src/event.c:625 repair_win` (XDamage).
- `src/renderer/renderer.c:426 renderer_render` / `src/renderer/damage.c:331 commands_cull_with_damage`.
