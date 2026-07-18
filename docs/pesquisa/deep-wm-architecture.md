# Deep Window-Manager Architecture — reading the real source

Research notes for **ntwm** (a from-scratch tiling WM on Windows/NT that runs as a
separate process and *declares layout* to a display server, **dispd**, over a named pipe).

The goal of this document is not to describe WMs in the abstract but to trace, through
**actual source code**, (1) how a real WM boots, loops, and tears down; (2) the *substrate*
it stands on and exactly how it uses it; (3) its core data structures; (4) its layout math;
(5) its input path; (6) its IPC/extensibility; and (7) the reparenting question. Every claim
below is anchored to a file and function I read.

Source snapshots read (cloned `--depth 1`, versions pinned for citation):

| WM | family | lang | commit | what it teaches |
|----|--------|------|--------|-----------------|
| dwm | X11, non-reparenting | C / Xlib | `44dbc68` | the canonical minimal WM; substructure redirection in ~2100 LOC |
| dwl | Wayland, wlroots | C | `a2d03cf` | dwm's model ported to wlroots; the scene-graph compositor-WM |
| i3 | X11, reparenting | C / XCB | `f7d5b89` | the tree model + the unix-socket IPC + batched X pushes |
| bspwm | X11, non-reparenting | C / XCB | `e11eff4` | WM as a pure server driven *entirely* over a socket (closest to ntwm) |
| awesome | X11, reparenting | C / XCB + Lua | `26454fb` | thin C core exposing X objects to a Lua policy layer via signals |
| river | Wayland, wlroots | Zig | `d4fef52` | compositor that **delegates layout to an external process over a Wayland protocol** (ntwm's exact shape) |
| xmonad | X11 | Haskell / Xlib | `a9a8b5c` | the pure-functional `StackSet` zipper; layout as a pure function |

The two canonical minimal implementations (dwm, dwl) I read line-by-line; the larger
codebases (i3, bspwm, awesome, river, xmonad) were traced function-by-function in parallel.

---

## 0. The one idea that makes a WM possible: *policy on top of someone else's mechanism*

A window manager never draws application pixels and never owns the framebuffer directly. It is
a **policy process** that sits on top of a **mechanism** (the display server) and issues it
commands: *put this window here, at this size, in this stacking order; give keyboard focus to
that one*. The mechanism does the actual moving, clipping, compositing, and input delivery.

The single most important architectural fact is *how the mechanism lets one privileged client
override the default window behaviour*. In X11 this is **substructure redirection**; in Wayland
the mechanism and the policy are fused into one process (the compositor *is* the server). ntwm
sits in a third position: it is a **separate** policy process like an X WM, but it talks to a
custom server (dispd) — which is architecturally identical to **bspwm driven by bspc**, or to
**river driven by an external layout client**, or to **i3 driven by i3-msg**. Those three are
the load-bearing references for this project, so they get extra depth.

---

## 1. The X11 substrate and how a WM actually uses it

### 1.1 The shape of X

The X server owns the display, the framebuffer, and all windows. Windows form a **tree** rooted
at the *root window* (the desktop). Clients (applications) create child windows and ask the
server to map (show) them. Clients talk to the server over a socket using the **X11 wire
protocol**: asynchronous *requests* (client→server), *replies* (server→client, only for
round-trip requests), *events* (server→client), and *errors*.

Two client libraries:

- **Xlib** — the old, synchronous-feeling API. dwm, xmonad (via Haskell `X11` bindings) use it.
  Calls like `XMapWindow`, `XConfigureWindow` map ~1:1 onto protocol requests but Xlib hides the
  socket, buffers requests, and blocks on anything that needs a reply (`XGetWindowAttributes`,
  `XQueryTree`). This request/reply blocking is the classic Xlib latency trap.
- **XCB** — the modern, explicitly-asynchronous API. i3, bspwm, awesome use it. You *issue* a
  request and get a *cookie*; you later *redeem* the cookie for the reply, so you can pipeline
  many requests before blocking. Function names are `xcb_map_window`, `xcb_configure_window`,
  `xcb_grab_key`, etc. This is why i3/bspwm feel snappier and can batch.

### 1.2 Substructure redirection — the crux

Normally, when a client calls `XMapWindow` or `XConfigureWindow` on its own top-level window,
the server just does it. That would make a tiling WM impossible — apps would place and size
themselves. X solves this with **redirection**: a single client (the WM) selects
`SubstructureRedirectMask` on the **root window**. From then on, whenever *any other client*
tries to map, configure, or restack a **child of the root**, the server does **not** perform the
operation. Instead it **converts the request into an event** and sends it *only to the WM*:

- a client's `XMapWindow(child_of_root)` → the server sends the WM a **`MapRequest`** event and
  does nothing else.
- a client's `XConfigureWindow(child_of_root, ...)` (move/resize/restack) → the server sends the
  WM a **`ConfigureRequest`** event carrying the requested geometry, and does nothing else.
- a client's `XCirculateWindow` → **`CirculateRequest`**.

The WM is now the *arbiter*. It reads the request, decides the real geometry per its layout
policy, and issues the operation itself (a `XConfigureWindow`/`XMoveResizeWindow` from the WM is
honoured because the WM is the redirecting client). This is exactly why a WM is "a policy engine
in front of a mechanism": redirection turns every window-management action by every app into a
message the WM can veto or rewrite.

Two consequences visible in the code:

1. **Only one client at a time may hold `SubstructureRedirectMask` on the root.** So the way to
   detect "another WM is already running" is to *try* to select it and see if you get a
   `BadAccess` error. dwm's `checkotherwm()` (dwm.c:459) installs a temporary error handler
   `xerrorstart` (dwm.c:2124) that `die()`s, calls
   `XSelectInput(dpy, DefaultRootWindow(dpy), SubstructureRedirectMask)`, then `XSync` — if
   another WM holds it, the server sends `BadAccess`, the handler fires, and dwm exits. i3 and
   bspwm do the identical trick over XCB (checking the error on the `change_window_attributes`
   that sets the mask).

2. **Override-redirect windows bypass all of this.** A client can set the `override_redirect`
   attribute on a window (menus, tooltips, dmenu, OSDs) to tell the server "do not redirect me to
   the WM". dwm's `maprequest()` (dwm.c:1101) checks `wa.override_redirect` and returns without
   managing; `scan()` (dwm.c:1394) skips them too. So a WM must always test that flag.

`SubstructureNotifyMask` is the sibling mask: it doesn't redirect, it just *informs*. The WM
selects it too, to receive `MapNotify`, `UnmapNotify`, `DestroyNotify`, `ConfigureNotify`,
`CreateNotify`, `ReparentNotify` for the root's children — i.e. to learn when windows actually
appeared, vanished, or were destroyed. dwm sets both in `setup()`:
`wa.event_mask = SubstructureRedirectMask|SubstructureNotifyMask|ButtonPressMask|
PointerMotionMask|EnterWindowMask|LeaveWindowMask|StructureNotifyMask|PropertyChangeMask`
then `XChangeWindowAttributes(dpy, root, CWEventMask|CWCursor, &wa)` (dwm.c:1607–1611).

### 1.3 The concrete X requests a WM issues

Reading dwm's `manage()`, `resizeclient()`, `restack()`, `setfocus()`:

- **`XConfigureWindow(dpy, win, valuemask, &XWindowChanges)`** — the workhorse. Sets `x,y,width,
  height,border_width,sibling,stack_mode`. dwm's `resizeclient()` (dwm.c:1286) fills a
  `XWindowChanges wc` and calls `XConfigureWindow(dpy, c->win, CWX|CWY|CWWidth|CWHeight|
  CWBorderWidth, &wc)`. Restacking uses the same call with `CWSibling|CWStackMode` in
  `restack()` (dwm.c:1357): it walks the focus stack and stacks each tiled client `Below` the
  previous sibling, so the layout order is enforced in the server.
- **`XMoveResizeWindow` / `XMoveWindow`** — Xlib convenience wrappers over ConfigureWindow. dwm
  *hides* windows not by unmapping them but by moving them off-screen: `showhide()` (dwm.c:1629)
  does `XMoveWindow(dpy, c->win, WIDTH(c) * -2, c->y)` for invisible clients. This is a
  deliberate trick — see §7.3.
- **`XMapWindow` / `XUnmapWindow`** — show/hide at the server level. dwm maps in `manage()`
  after everything is set up.
- **`XSetInputFocus(dpy, win, RevertToPointerRoot, CurrentTime)`** — hands the keyboard to a
  window. dwm's `setfocus()` (dwm.c:1472). Note the ICCCM subtlety: a client can advertise it
  does *not* want the server to force focus onto it (`InputHint`=False in WM_HINTS) and instead
  be told via a `WM_TAKE_FOCUS` client message. dwm honours this: `setfocus()` skips
  `XSetInputFocus` if `c->neverfocus` and always sends `WM_TAKE_FOCUS` via `sendevent()`.
- **`XGrabKey` / `XGrabButton`** — register global hotkeys / click bindings (see §1.5).
- **`XReparentWindow`** — reparent a client window under a WM-created *frame* window. dwm does
  **not** use this (non-reparenting); i3 and awesome do (see §7).
- **`XSendEvent`** — synthesise events to clients, used for ICCCM protocol messages
  (`WM_DELETE_WINDOW`, `WM_TAKE_FOCUS`) in dwm's `sendevent()` (dwm.c:1447) and to tell a client
  its final geometry via a synthetic `ConfigureNotify` in `configure()` (dwm.c:533).
- **`XChangeProperty` / `XGetWindowProperty`** — read/write window properties (atoms). Used for
  ICCCM/EWMH state (below).

### 1.4 ICCCM and EWMH — the conventions layer

X's protocol says nothing about *what* windows mean. Two conventions fill the gap, implemented
as **properties** (named `Atom`s carrying typed data) on windows and the root:

- **ICCCM** (Inter-Client Communication Conventions Manual): the old, mandatory baseline.
  - `WM_NAME` / title, `WM_CLASS` (instance+class, used for rules — dwm's `applyrules()` reads it
    via `XGetClassHint`, dwm.c:289).
  - `WM_NORMAL_HINTS` (an `XSizeHints`): the app's min/max/base size, resize increments, aspect
    ratio. dwm's `updatesizehints()` (dwm.c:1961) parses every flag (`PMinSize`, `PMaxSize`,
    `PResizeInc`, `PBaseSize`, `PAspect`) into the `Client`, and `applysizehints()` (dwm.c:313)
    applies them — this is why terminals snap to character cells. A window whose min==max is
    `isfixed` and forced floating.
  - `WM_HINTS` (an `XWMHints`): the input-focus model (`InputHint`) and the **urgency** flag.
    dwm's `updatewmhints()` (dwm.c:2034).
  - `WM_PROTOCOLS`: which client-message protocols the app understands, notably
    `WM_DELETE_WINDOW` (graceful close) and `WM_TAKE_FOCUS`. dwm's `killclient()` (dwm.c:1015)
    sends `WM_DELETE_WINDOW` if supported, else falls back to `XKillClient` (a hard kill of the
    client's X connection).
  - `WM_STATE`: the WM tells the app whether it is Normal/Iconic/Withdrawn. dwm's
    `setclientstate()` (dwm.c:1438).
- **EWMH** (Extended Window Manager Hints, the `_NET_*` atoms): the modern layer that pagers,
  taskbars, and fullscreen apps rely on. dwm interns a handful in `setup()` (dwm.c:1567):
  `_NET_SUPPORTED`, `_NET_WM_NAME`, `_NET_WM_STATE` + `_NET_WM_STATE_FULLSCREEN`,
  `_NET_ACTIVE_WINDOW`, `_NET_WM_WINDOW_TYPE` + `..._DIALOG`, `_NET_CLIENT_LIST`,
  `_NET_SUPPORTING_WM_CHECK`. It advertises support by writing `_NET_SUPPORTED` on the root, and
  publishes the managed set in `_NET_CLIENT_LIST` (`updateclientlist()`, dwm.c:1853). Fullscreen
  is driven by clients sending a `_NET_WM_STATE` **client message**, handled in
  `clientmessage()` (dwm.c:515) → `setfullscreen()` (dwm.c:1482).

**Lesson for ntwm:** dispd needs its own equivalent of these conventions — a way for apps to
declare title/class, min/max/increment size hints, "I am a dialog / floating", "please close",
"I want fullscreen", and for ntwm to publish the client list and the focused window. Design this
as a small typed property protocol from day one; retrofitting ICCCM-style hints is painful.

### 1.5 Input: grabs and the key→action path

A WM does not read the keyboard device. It asks the server to **grab** specific key/modifier
combinations on the root window; the server then delivers those as `KeyPress` events to the WM
instead of the focused client.

dwm's `grabkeys()` (dwm.c:952) is the whole story:
1. `updatenumlockmask()` finds which modifier bit is NumLock (dwm.c:1945) so bindings work
   whether or not NumLock/CapsLock are on.
2. `XUngrabKey(dpy, AnyKey, AnyModifier, root)` clears old grabs.
3. It fetches the whole keymap (`XGetKeyboardMapping`) and, for each configured `Key` (matched by
   **keysym**, the layout-independent symbol like `XK_p`), converts to every **keycode**
   (hardware position) that produces that keysym, and calls
   `XGrabKey(dpy, keycode, key.mod | lockcombo, root, True, GrabModeAsync, GrabModeAsync)` for
   each of the four lock combinations `{0, LockMask, numlockmask, numlockmask|LockMask}`.

When a grabbed key fires, the server sends `KeyPress`; dwm's `keypress()` (dwm.c:1000) converts
the event's keycode back to a keysym with `XKeycodeToKeysym`, then linear-scans the `keys[]`
table comparing keysym and `CLEANMASK(mod)` (which strips lock/numlock bits), and calls the
matching `func(&arg)`. That's the entire "keybind becomes an action" pipeline: **grab → KeyPress
→ table match → dispatch a C function pointer with a union `Arg`**. The config *is* that table
(`config.def.h:64` `static const Key keys[] = { {MODKEY, XK_p, spawn, {.v=dmenucmd}}, ... }`).

Mouse bindings work the same way via `XGrabButton` in `grabbuttons()` (dwm.c:931) and dispatch
in `buttonpress()` (dwm.c:417), which additionally figures out *where* the click landed (tag bar,
layout symbol, status text, title, client, or root — the `Clk*` enum) so one button can mean
different things in different regions.

Modifiers are just a bitmask (`Mod1Mask`=Alt, `Mod4Mask`=Super, `ShiftMask`, `ControlMask`).
`CLEANMASK` (dwm.c:49) removes the "don't care" lock bits before comparing.

---

## 2. The Wayland substrate and how a compositor-WM uses it

### 2.1 The architectural inversion

Wayland deletes the separate X server. In Wayland the **compositor is the display server** and
the window manager, fused into one process. There is no external mechanism to send requests to —
*you are the mechanism*. This is the deepest structural difference from X and the thing to keep
straight while reading dwl/river:

- In X, the WM is a *client* of the server and steers it via redirection.
- In Wayland, the "WM" *is* the server; apps are its clients. Apps hand the compositor **buffers**
  (pixels) via `wl_surface`; the compositor decides where each surface goes, composites them, and
  drives the actual display via **DRM/KMS**; it reads input via **libinput** and routes it.

Nobody writes the raw Wayland protocol + DRM + GL by hand. They use **wlroots** (in C; river uses
it from Zig): a library of composable building blocks. dwl is essentially "dwm re-expressed on
wlroots".

### 2.2 wlroots building blocks (what the compositor delegates)

From dwl's includes and `setup()` (dwl.c:2447) and river's `Server.zig`:

- **`wlr_backend`** (`wlr_backend_autocreate`, dwl.c:2467) — abstracts the hardware/session. It
  picks DRM/KMS + libinput on a TTY, or nests inside an existing X11/Wayland window for dev. It
  emits `new_output` (a monitor appeared) and `new_input` (keyboard/mouse appeared) signals, and
  becomes DRM master when started (`wlr_backend_start`, dwl.c:2250).
- **`wlr_renderer`** + **`wlr_allocator`** (`wlr_renderer_autocreate`, `wlr_allocator_autocreate`,
  dwl.c:2482/2507) — GLES2/Vulkan/Pixman rendering and buffer allocation. The compositor almost
  never touches these directly once the scene graph is set up.
- **`wlr_scene`** (`wlr_scene_create`, dwl.c:2471) — the **scene graph**: a retained tree of
  nodes (trees, surfaces, rects) with positions and z-order. You describe *what is where*; wlroots
  computes damage and does the actual rendering/compositing on each output frame. This is the
  Wayland analogue of "issuing ConfigureWindow" — instead you `wlr_scene_node_set_position` and
  `wlr_scene_node_reparent`. dwl builds a fixed stack of layer-trees in `setup()`:
  `enum { LyrBg, LyrBottom, LyrTile, LyrFloat, LyrTop, LyrFS, LyrOverlay, LyrBlock }` (dwl.c:88),
  and z-order *is* which layer-tree a client's node is parented under.
- **`wlr_xdg_shell`** (`wlr_xdg_shell_create`, dwl.c:2560) — the protocol for normal application
  windows ("toplevels") and their popups. Emits `new_toplevel` → dwl's `createnotify()`
  (dwl.c:1121). This is the Wayland replacement for "a child of the root appeared".
- **`wlr_layer_shell_v1`** — the protocol for bars/wallpapers/panels (things pinned to screen
  edges with exclusive zones). dwl handles these in `arrangelayers()` (dwl.c:567).
- **`wlr_seat`** (`wlr_seat_create`, dwl.c:2637) — the input-routing abstraction: at most one
  keyboard, pointer, touch. The compositor tells the seat *who has focus*
  (`wlr_seat_keyboard_notify_enter`) and forwards raw input; the seat delivers it to the focused
  client's surface with correct enter/leave semantics. This replaces `XSetInputFocus` + event
  delivery.
- **`wlr_cursor`** + **`wlr_xcursor_manager`** — cursor position/image across outputs.
- **`wlr_output_layout`** — the arrangement of monitors in a virtual coordinate space (the
  Wayland analogue of Xinerama), emits `change` → dwl's `updatemons()`.
- **Xwayland** (`wlr_xwayland_create`, dwl.c:2660) — an embedded X server so legacy X apps run.
  dwl abstracts the xdg-vs-X11 difference behind `client.h` (see §5-dwl).

### 2.3 The Wayland event loop

There is no `XNextEvent`. libwayland provides **`wl_event_loop`**, an epoll-based reactor.
`setup()` grabs it via `wl_display_get_event_loop(dpy)` (dwl.c:2461). Everything is wired with
**listeners**: a `struct wl_listener` holds a `notify` function pointer and is added to a
`wl_signal` with `wl_signal_add`. dwl wraps this in the `LISTEN(E, L, H)` macro (dwl.c:82). The
entire compositor is a graph of these callbacks; there is no central dispatch table like dwm's
`handler[]`. `run()` (dwl.c:2240) creates the socket (`wl_display_add_socket_auto`), starts the
backend, then calls **`wl_display_run(dpy)`** — the blocking epoll loop that fires listeners until
someone calls `wl_display_terminate`. The loop also multiplexes: it watches the Wayland client
socket(s), the libinput fd, the DRM fd (frame/vblank events → `rendermon`, dwl.c:2152), and any
timers (e.g. key-repeat, `wl_event_source_timer`, dwl.c:1660).

**Lesson for ntwm:** the epoll/`wl_event_loop` reactor multiplexing *input fds + client sockets +
render timing + control IPC* in one loop is precisely the shape dispd wants — one loop selecting
over the app-facing pipe(s), the ntwm control pipe, and the input/vsync sources.

---

## 3. Case study: dwm — the canonical X11 non-reparenting WM

dwm is ~2100 lines and the clearest possible statement of "a WM is an event loop over redirected
requests". Read `main()` bottom-up as the file's header comment instructs (dwm.c:21).

### 3.1 Startup → loop → teardown

`main()` (dwm.c:2143):
1. `XOpenDisplay(NULL)` — connect to the X server.
2. `checkotherwm()` (dwm.c:459) — the `SubstructureRedirectMask`+`BadAccess` probe (§1.2).
3. `setup()` (dwm.c:1539):
   - `SIGCHLD` set to `SIG_IGN` with `SA_NOCLDWAIT` so spawned children don't become zombies, then
     reap any inherited ones.
   - query screen geometry, create the `Drw` drawing context and load fonts.
   - `updategeom()` (dwm.c:1867) — build the `Monitor` list, using **Xinerama**
     (`XineramaQueryScreens`) to discover physical outputs and de-duplicate mirrored ones
     (`isuniquegeom`).
   - intern all the ICCCM/EWMH atoms; create cursors and color schemes.
   - create the bar windows (`updatebars()`), the `_NET_SUPPORTING_WM_CHECK` window, publish
     `_NET_SUPPORTED`.
   - **select the root event mask** (`SubstructureRedirect|SubstructureNotify|...`), then
     `grabkeys()` and `focus(NULL)`.
4. `scan()` (dwm.c:1394) — **adopt pre-existing windows**. `XQueryTree` the root's children; for
   each viewable, non-override-redirect, non-transient window (or one in IconicState), call
   `manage()`. A second pass manages the transients (so their parents exist first). This is what
   lets a WM be started or restarted with apps already open.
5. `run()` (dwm.c:1382) — the loop: `while (running && !XNextEvent(dpy, &ev)) if
   (handler[ev.type]) handler[ev.type](&ev);`. **O(1) dispatch** through the `handler[]` array
   indexed by event type (dwm.c:245), the same idea the header comment sells.
6. `cleanup()` (dwm.c:470) — unmanage everything, ungrab keys, free monitors/cursors/schemes,
   destroy the check window, reset input focus, `XCloseDisplay`.

### 3.2 Managing a window: `manage()` (dwm.c:1031)

When `maprequest()` (dwm.c:1101) fires for an unmanaged, non-override-redirect window it calls
`manage(w, &wa)`:
- allocate a `Client`, copy initial geometry from `XWindowAttributes`.
- decide monitor+tags: if the window is transient-for another (`XGetTransientForHint`), inherit
  the parent's monitor/tags; else `applyrules()` (dwm.c:277) matches `WM_CLASS`/title against the
  `rules[]` table to set tags, floating, monitor.
- clamp geometry into the monitor; set border width (`XConfigureWindow` with `CWBorderWidth`) and
  border color; send a synthetic `configure()`.
- `updatewindowtype()` / `updatesizehints()` / `updatewmhints()` — read EWMH/ICCCM properties.
- **`XSelectInput(dpy, w, EnterWindowMask|FocusChangeMask|PropertyChangeMask|
  StructureNotifyMask)`** — subscribe to the client's own events (enter for focus-follows-mouse,
  focus changes, property changes for title/hints, structure for unmap/destroy).
- `grabbuttons()`; attach to the `clients` list (`attach`, dwm.c:403) and the focus `stack`
  (`attachstack`); append to `_NET_CLIENT_LIST`.
- **the offscreen trick**: `XMoveResizeWindow(dpy, w, c->x + 2*sw, c->y, ...)` places it far off
  to the right first, then maps, so it never flashes at the wrong spot before `arrange()` runs.
- `arrange(c->mon)` then `XMapWindow` then `focus(NULL)`.

Note: **no `XReparentWindow`**. dwm manages the client window *in place*, only adding a border and
moving it. That makes dwm a **non-reparenting** WM (§7).

### 3.3 Data structures

- **`Client`** (dwm.c:86): `win`, geometry (`x,y,w,h` + `old*`), all the size hints
  (`basew,baseh,incw,inch,maxw,maxh,minw,minh,mina,maxa`), `bw` border width, `tags` bitmask,
  the boolean flags (`isfixed,isfloating,isurgent,neverfocus,isfullscreen`), `next` (tiling
  order in `clients`), `snext` (focus order in `stack`), `mon`.
- **`Monitor`** (dwm.c:113): geometry split into `mx,my,mw,mh` (whole monitor) vs
  `wx,wy,ww,wh` (window area minus bar), `tagset[2]` + `seltags` (the current-view double-buffer),
  `mfact` (master-area fraction), `nmaster`, `sel` (focused client), `clients` and `stack` list
  heads, `lt[2]`+`sellt` (the two selectable layouts), the bar window.
- **Tags, not workspaces.** A client's `tags` is a **bitmask**; a monitor's current view
  `tagset[seltags]` is also a bitmask. A client is visible iff `c->tags & mon->tagset[seltags]`
  (`ISVISIBLE`, dwm.c:52). This means a window can be on several tags at once and a view can show
  several tags at once — strictly more general than i3's "one window on one workspace". river and
  dwl copy this bitmask model exactly.
- Two linked lists per monitor: `clients` (stable, defines tiling order) and `stack`
  (MRU focus order). Focus operations reorder `stack`; `zoom`/`pop` reorder `clients`.

### 3.4 Layout engine: `tile()` (dwm.c:1687)

Tiling in dwm is a pure geometry function per monitor:
```
n = count of visible tiled clients
mw = (n > nmaster) ? (nmaster ? ww*mfact : 0) : ww       // master column width
for each visible tiled client i:
    if i < nmaster:  resize into left column, height split evenly among masters
    else:            resize into right column, height split evenly among the rest
```
`resize()` (dwm.c:1278) runs the geometry through `applysizehints()` and, if it actually changed,
`resizeclient()` (dwm.c:1286) issues the `XConfigureWindow`. `arrange()` (dwm.c:381) is the
entry point: `showhide()` maps/hides clients by tag, then `arrangemon()` calls the current
layout's function pointer, then `restack()` fixes z-order. Layouts are just
`{symbol, void (*arrange)(Monitor*)}` (dwm.c:108); a `NULL` arrange means floating.
`monocle()` (dwm.c:1113) stacks every client full-size. Adding a layout = adding a function.

### 3.5 Focus & stacking

`focus(c)` (dwm.c:788): if `c` is NULL/invisible, pick the topmost visible client from the focus
`stack`; unfocus the old selection (revert its border, ungrab its buttons); for the new one move
it to the top of `stack`, set the selected border color, and `setfocus()` which does
`XSetInputFocus` (unless `neverfocus`), writes `_NET_ACTIVE_WINDOW`, and sends `WM_TAKE_FOCUS`.
`restack()` (dwm.c:1357) raises floating/selected windows and, for tiled ones, walks the focus
stack issuing `XConfigureWindow` with `CWSibling|CWStackMode=Below` so the server's stacking
matches policy, then drains stale `EnterNotify` events (`XCheckMaskEvent(dpy, EnterWindowMask)`)
so restacking doesn't cause spurious focus-follows-mouse jumps — a subtle but important gotcha.

---

## 4. Case study: dwl — dwm's model on wlroots (Wayland)

dwl (~3200 lines in `dwl.c`) deliberately mirrors dwm's structure (same `Client`/`Monitor`
names, same `tile()`, same tag bitmask) but sits on wlroots, so it is the cleanest way to see the
X-vs-Wayland substrate difference with the *policy held constant*.

### 4.1 Startup → loop → teardown

`main()` (dwl.c:3188) → `setup()` (dwl.c:2447) → `run()` (dwl.c:2240) → `cleanup()`.
`setup()` is a long constructor that news-up every wlroots subsystem and wires a listener to each
signal (see §2.2/§2.3): backend, scene graph + the 8 layer-trees, renderer/allocator, the pile of
protocol globals (`wlr_compositor`, `wlr_xdg_shell`, `wlr_layer_shell_v1`,
`wlr_xdg_decoration_manager_v1`, `wlr_session_lock_manager_v1`, output management, etc.), the
cursor + seat, the keyboard group, and optionally Xwayland. `run()` opens the Wayland socket,
starts the backend, optionally forks a startup script wired to a pipe, then `wl_display_run`.
There is **no scan()**: there are no pre-existing Wayland clients when the compositor starts (it
*is* the server), so window adoption doesn't exist — a genuinely simpler startup than X.

### 4.2 The event model: signals, not a dispatch table

Every state change is a wlroots **signal** you subscribe to. New window:
`xdg_shell.events.new_toplevel` → `createnotify()` (dwl.c:1121) allocs a `Client`, stashes it as
`toplevel->base->data`, and `LISTEN`s to that toplevel's own signals: `commit`, `map`, `unmap`,
`destroy`, `request_fullscreen`, `request_maximize`, `set_title`. The window only becomes visible
on **`map`** → `mapnotify()` (dwl.c:1739).

### 4.3 There is no reparenting and no ConfigureWindow — there is the scene graph

This is the key substrate contrast. In `mapnotify()` (dwl.c:1739) dwl:
- creates a **scene tree** for the client: `c->scene = wlr_scene_tree_create(layers[LyrTile])`.
- creates the surface node under it (`wlr_scene_xdg_surface_create`), and stores `c` as the
  node's `.data` for hit-testing.
- creates **four `wlr_scene_rect` border rectangles** (`c->border[0..3]`) — decorations are just
  colored rects in the scene graph, not a frame window.
- inserts into the same two lists dwm has: `clients` (tiling) and `fstack` (focus).

Applying geometry is `resize()` (dwl.c:2207): it sets `c->geom`, then
`wlr_scene_node_set_position(&c->scene->node, x, y)`, sizes the four border rects with
`wlr_scene_rect_set_size`, and tells the client its new size with `client_set_size()` (which
becomes `wlr_xdg_toplevel_set_size` or, for Xwayland, `wlr_xwayland_surface_configure`). Crucially
`client_set_size` returns a **configure serial**, stored in `c->resize`. dwl then refuses to
render a frame for that monitor until the client has committed a buffer matching the new size —
`rendermon()` (dwl.c:2152) scans clients and `goto skip`s the `wlr_scene_output_commit` if any
tiled client still has an outstanding `c->resize`. **This is how Wayland avoids the X flicker of a
window being visible at the old size during a resize**: the compositor waits for the client's ack.

Stacking order = which layer-tree a node is parented under, plus
`wlr_scene_node_raise_to_top`. `setfloating()`/`setfullscreen()` (dwl.c:2334/2349) literally
`wlr_scene_node_reparent` the client between `LyrTile`, `LyrFloat`, `LyrFS`.

`tile()` (dwl.c:2715) is **the same master/stack math as dwm** — same `mfact`, `nmaster`, even
structured identically — but emits `resize(c, (struct wlr_box){...}, 0)` instead of
`XConfigureWindow`. Reading dwm.tile and dwl.tile side by side is the single best illustration
that *layout policy is substrate-independent; only the "apply" call differs*.

### 4.4 Input: libinput → seat, and the keybind path

New input device: `backend.events.new_input` → `inputdevice()` → `createkeyboard()` (dwl.c:942)
adds the keyboard to a **`wlr_keyboard_group`** (so multiple physical keyboards share one xkb
state and one repeat timer). Keys arrive via the group's `key` signal → `keypress()` (dwl.c:1630):
- translate the libinput keycode (`+8` to get the X/xkb keycode), get keysyms from the xkb state.
- if not locked and this is a *press*, run `keybinding(mods, sym)` (dwl.c:1610) which linear-scans
  the same kind of `keys[]` table dwm has (`CLEANMASK` compare on mods + keysym) and calls the
  bound `func(&arg)`.
- **if handled, the key is swallowed**; if not, it is forwarded to the focused client with
  `wlr_seat_keyboard_notify_key`. Key repeat for compositor bindings is done by the compositor
  itself via a `wl_event_source` timer (dwl.c:1660, `keyrepeat` dwl.c:1689).

Focus is `focusclient()` (dwl.c:1404): raise the node if asked, move the client to the front of
`fstack`, recolor borders, and hand the keyboard over with
`client_notify_enter()` → `wlr_seat_keyboard_notify_enter(seat, surface, keycodes, mods)`. Note
there is no `XSetInputFocus`; the *seat* owns focus and does the enter/leave bookkeeping.

### 4.5 The suck-abstraction: `client.h`

Because Wayland toplevels (`wlr_xdg_toplevel`) and Xwayland windows
(`wlr_xwayland_surface`) are different types, dwl hides the difference behind a set of
`client_*` inline functions in `client.h` (e.g. `client_surface`, `client_get_appid`,
`client_send_close`, `client_set_size`, `client_set_fullscreen`, `client_notify_enter`). The
first field of `Client` is `unsigned int type` (`XDGShell`/`X11`/`LayerShell`), and every
`client_*` function branches on it under `#ifdef XWAYLAND`. This is a clean pattern for ntwm if
dispd ever needs to manage more than one kind of surface.

**dwl takeaways for ntwm:** (a) the scene-graph model — *declare* node positions/parenting and let
the server composite — is closer to ntwm's "declare layout" design than X's imperative
ConfigureWindow; (b) the **configure-serial handshake** that gates rendering on the client's ack
is the correct way to make resizes flicker-free and is worth replicating in the dispd protocol;
(c) borders/decorations as first-class scene rects (not frame windows) keeps the model simple.

---

## 5. Case study: bspwm — the WM as a pure server driven over a socket (the ntwm reference)

bspwm (`e11eff4`, C99/XCB) is the most important reference for this project because its
architecture *is* "a long-running WM daemon with no input logic of its own, driven entirely over
a socket by an external client." It cleanly separates into **three** processes:

- **`bspwm`** — the daemon: owns the X connection, the BSP trees, and an `AF_UNIX` socket.
- **`bspc`** (`bspc.c`, ~117 lines) — a stateless CLI that serializes its `argv` onto that socket
  and prints the reply. It shares **no** window-management code with the daemon (the Makefile
  links `bspc` against only `helpers.o`).
- **`sxhkd`** (a *separate* project) — a standalone hotkey daemon that grabs keys and execs
  `bspc …` on each chord.

**bspwm contains no keyboard code whatsoever.** Grepping the tree: no `xcb_grab_key`, no
`XCB_KEY_PRESS` case in the dispatcher, no keysym→action table. The only keysym use is
`modfield_from_keysym()` (pointer.c:125) to learn the NumLock/CapsLock modifier bits for *mouse*
grabs. The keymap lives in another process entirely; bspwm only ever sees a finished `bspc`
command arriving on the socket. This is exactly ntwm↔dispd, inverted (there ntwm is the policy
client and dispd the server; in bspwm the policy is even further out in sxhkd).

### 5.1 Startup → the dual-fd loop → teardown

`main()` (bspwm.c:94): parse opts (`-c` config, `-s` state path for restart, `-o` inherited
socket fd for restart); `xcb_connect`; `load_settings()`; `setup()`.

`setup()` (bspwm.c:349) seizes the WM role: `ewmh_init()`; grab the root; then the decisive
`register_events()` (bspwm.c:465) does
`xcb_change_window_attributes_checked(dpy, root, XCB_CW_EVENT_MASK, {ROOT_EVENT_MASK})` wrapped in
`xcb_request_check` — where `ROOT_EVENT_MASK = SUBSTRUCTURE_REDIRECT | SUBSTRUCTURE_NOTIFY |
STRUCTURE_NOTIFY | BUTTON_PRESS | FOCUS_CHANGE` (bspwm.h:38). **If that request errors, another WM
holds redirection and bspwm exits** ("Another window manager is already running"). It also creates
two `INPUT_ONLY` helper windows: `meta_window` (the `_NET_SUPPORTING_WM_CHECK` window) and
`motion_recorder` (a 1×1 window with `POINTER_MOTION` mask for focus-follows-pointer). Monitor
discovery tries RandR, then Xinerama, then a single fullscreen fallback.

The socket: path is `$BSPWM_SOCKET` or the template `SOCKET_PATH_TPL =
"/tmp/bspwm%s_%i_%i-socket"` (common.h:28) filled from host/display/screen;
`socket(AF_UNIX, SOCK_STREAM)` → `unlink` → `bind` → `listen(sock_fd, SOMAXCONN)` → set
`FD_CLOEXEC`. Then `run_config()` (settings.c:76) `fork`+`execl`s the user's `bspwmrc` — which is a
plain **executable shell script**, not a parsed config file; it typically launches sxhkd and fires
the initial `bspc config …` calls. bspwm never parses a config format.

**The crux — one `select()` over three fd kinds** (bspwm.c:212–273):
```c
while (running) {
    xcb_flush(dpy);
    FD_SET(sock_fd, &fds);  FD_SET(dpy_fd, &fds);         // dpy_fd = xcb_get_file_descriptor(dpy)
    for (pending_rule ...) FD_SET(pr->fd, &fds);          // async external-rule pipes
    if (select(max_fd+1, &fds, ...) > 0) {
        if (FD_ISSET(sock_fd, &fds)) {                     // an IPC client connected
            cli_fd = accept(sock_fd, ...);
            n = recv(cli_fd, msg, ...);  msg[n] = 0;
            handle_message(msg, n, fdopen(cli_fd, "w"));   // dispatch + write reply
        }
        if (FD_ISSET(dpy_fd, &fds))                        // X events pending
            while ((ev = xcb_poll_for_event(dpy))) { handle_event(ev); free(ev); }
    }
    if (!check_connection(dpy)) running = false;
    prune_dead_subscribers();
}
```
Single-threaded, no callbacks. X events and IPC commands drain in the same iteration, so a
control command and an X event **can never run concurrently** — no locks needed. `xcb_flush()` at
the top pushes out all requests queued by the previous iteration's handlers. `handle_event()`
(events.c:40) switches on `MAP_REQUEST`, `DESTROY/UNMAP/CONFIGURE/PROPERTY/CLIENT_MESSAGE`,
`ENTER/MOTION/BUTTON_PRESS/FOCUS_IN/MAPPING_NOTIFY`, RandR — never a key event.

Existing windows are adopted by `adopt_orphans()` (window.c:428). **Restart is a highlight**: on
exit with `restart`, bspwm serializes full state to `/tmp/bspwm..._-state` via `query_state()`,
then clears `FD_CLOEXEC` on `sock_fd` and `execvp`s **itself** with `-s state -o sock_fd`, so the
socket and all connected subscriber clients survive the exec. A model worth stealing for a
zero-downtime ntwm reload.

### 5.2 The BSP tree (types.h) — strictly binary

`node_t` (types.h:254): `id` (== the client `xcb_window_t` for leaves, `XCB_NONE` for internal
nodes), `split_type` (`TYPE_HORIZONTAL`/`TYPE_VERTICAL` — a **two-value** enum, which is what
makes the tree binary), `split_ratio` (default 0.5), `presel`, `rectangle`, `first_child`,
`second_child`, `parent`, `client`. A node is a **leaf** iff `first_child == NULL`; internal nodes
carry only a fence (`split_type`+`split_ratio`) and have `client == NULL`. A *receptacle* is an
empty leaf (`client == NULL` but a leaf).

Contrast with i3: bspwm nodes are strictly **binary** (one fence → two parts), whereas i3
containers are **n-ary** (a split container holds N children). Five windows side by side in bspwm
are five leaves reached through four internal vertical-split nodes; there is no "container with N
children."

`desktop_t` (types.h:282) **owns its own BSP tree** (`node_t *root`) plus its own `focus` leaf and
`layout` (`LAYOUT_TILED`/`LAYOUT_MONOCLE`); switching desktops swaps which tree is shown.
`monitor_t` (types.h:297) holds a linked list of desktops and the currently shown one. The whole
hierarchy is intrusive doubly-linked lists (`prev`/`next` in each struct) rooted at globals
`mon_head/mon_tail/mon/pri_mon`. There is no global window→node hash; `locate_window()`
(query.c:943) walks the trees.

### 5.3 Layout: `insert_node()` and `apply_layout()`

`insert_node()` (tree.c:291): to add leaf `n` at anchor leaf `f`, allocate a **new internal node
`c`**, splice `c` into `f`'s slot under `f`'s parent, and give `c` the two children `f` and `n`
(order by `initial_polarity`). Orientation: under the default `SCHEME_LONGEST_SIDE`,
`c->split_type = (f->rect.width > f->rect.height) ? TYPE_VERTICAL : TYPE_HORIZONTAL`;
`SCHEME_ALTERNATE` uses the opposite of the nearest ancestor's split; `SCHEME_SPIRAL` rotates the
subtree. In manual mode (`f->presel` set) the direction/ratio come straight from the presel.

`arrange()` (tree.c:43) computes the root rect (monitor minus paddings/gap) then recurses via
`apply_layout()` (tree.c:73): set `n->rectangle`; at an **internal node**, split the rect into two
along one fence — `TYPE_VERTICAL`: `fence = width * split_ratio`, `first = {x,y,fence,h}`,
`second = {x+fence,y,w-fence,h}` (clamped to child `constraints.min_width`, `split_ratio` adjusted
back if it had to move) — and recurse into the two children; at a **leaf**, shrink the rect by
`bleed = window_gap + 2*border` and, **only if it changed**, push it with
`window_move_resize()` = `xcb_configure_window(win, X_Y_WIDTH_HEIGHT, ...)`. Monocle short-circuits:
every child gets the full rect. So layout is a recursive single-fence subdivision emitting one
`configure_window` per changed leaf.

### 5.4 The IPC protocol (the direct model for the dispd wire format)

**Wire format: NUL-separated argv, raw over the stream socket; reply streamed back.**
`bspc node -f west` goes on the wire literally as `node\0-f\0west\0` (bspc.c:78–81). `bspc`
connects, `send`s the blob, `poll`s for the reply, prints it to stdout until EOF.

**Error channel = one sentinel byte.** `FAILURE_MESSAGE = "\x07"` (BEL, common.h:31). The daemon's
`fail()` prints BEL then the message; `bspc` checks `if (rsp[0] == '\x07')` → routes the rest to
**stderr** and returns non-zero, else prints to **stdout** (bspc.c:98–105).

**Receive + tokenize + dispatch.** `handle_message()` (messages.c:46) walks the buffer rebuilding
an `argv` array at each `\0`, then `process_message()` (messages.c:86) dispatches on the first
token into "domains": `node`/`desktop`/`monitor`/`query`/`rule`/`wm`/`config`/`subscribe`/`quit`,
each a `cmd_*` function; then `fflush(rsp); fclose(rsp)`. Inside e.g. `cmd_node()` the first token
resolves to a target via a **selector**, then a `while` loop consumes imperative options
(`-f`/focus, `-s`/swap, `-t`/state, `-l`/layer, `-d`/to-desktop …), each calling into the tree
engine. **Commands mutate the trees synchronously in the same thread that owns X**, so no locking.

**SUBSCRIBE — the push feed.** `cmd_subscribe()` (messages.c:1329) parses event-class flags into a
`subscriber_mask_t` bitmask (e.g. `SBSC_MASK_NODE_FOCUS`, `SBSC_MASK_NODE_GEOMETRY`,
`SBSC_MASK_REPORT`) and then **deliberately does not close the reply stream** — it parks the
client's `FILE*` in a global subscriber list (`process_message` returns early to skip the
`fclose`). Emission is `put_status(mask, fmt, …)` (subscribe.c:144), called from all over the WM
(e.g. `focus_node` emits `node_focus`, tree.c:674), which `vfprintf`s the event line into each
matching subscriber and `fflush`es. `SBSC_MASK_REPORT` calls `print_report()` (subscribe.c:97),
producing the compact one-line panel string (e.g. `WMi:Ofocused:free:LT:TT:G`) that lemonbar/
polybar parse. `prune_dead_subscribers()` does a zero-length `write()` probe each loop iteration
to reap dead panels.

### 5.5 Selectors — a server-side text DSL (query.c, parse.c)

A selector like `@parent`, `west`, `.focused`, `north#@brother`, or `0x0040001a` resolves to
`coordinates_t {monitor, desktop, node}` relative to a reference. `node_from_desc()` (query.c:533):
`#` rebases the reference (left side resolved recursively — `north#@brother` = "brother of the node
north of me"); `.MODIFIERS` are tri-state filters (`.tiled/.floating/.focused/.local/.window/…`,
each negatable with `!`) checked by `node_matches()`; the **descriptor** head is matched in
priority order — spatial **directions** `north/west/south/east` via `find_nearest_neighbor()`
(tree.c:1124, scans leaves by boundary distance), cycle `next/prev`, history `older/newer`,
keywords `biggest/pointed/focused/…`, the **`@` tree-path** language (`@/1/2` = root→first→second,
`@parent`, `@brother`), or a numeric window id. The whole addressing language is resolved
**server-side**; `bspc` knows nothing about it.

### 5.6 X substrate: non-reparenting, borders only

bspwm **never reparents** — no per-client frame windows. It manages the client window in place,
selecting `CLIENT_EVENT_MASK = PROPERTY_CHANGE | FOCUS_CHANGE` on it. Geometry is a direct
`xcb_configure_window` (`window_move_resize`, window.c:843). The **entire** focus cue is the
server-side border pixel: `window_draw_border()` (window.c:423) is one
`xcb_change_window_attributes(win, XCB_CW_BORDER_PIXEL, &color)`; `get_border_color()` picks among
`focused`/`active`(focused-but-on-unfocused-monitor)/`normal`. No titlebars, no text. For a
**tiled** client that asks to resize itself (`XCB_CONFIGURE_REQUEST`, events.c:98), bspwm refuses
and instead sends the client a synthetic `XCB_CONFIGURE_NOTIFY` with the *tiled* geometry — the
ICCCM-correct "no, you are this size." Input focus honors ICCCM `input_hint`/`WM_TAKE_FOCUS` in
`set_input_focus()` (window.c:925). EWMH is fully maintained (ewmh.c) without ever reparenting.

**bspwm → ntwm takeaways (the strongest single reference):**
1. One privileged daemon owns the shared resource and runs a **single-threaded `select()` loop**
   multiplexing the resource fd + a control socket, serializing commands and events without locks.
2. The control channel is a dead-simple `AF_UNIX SOCK_STREAM` with **NUL-separated-argv requests**,
   a **streamed reply**, and a **one-byte failure sentinel** — the whole client (`bspc`) is ~117
   lines of forwarding. Consider this literal wire format for the ntwm↔dispd pipe.
3. Policy (the keymap) lives entirely outside; anything that can `write()` the socket drives the WM.
4. A **subscribe** side-channel turns the same socket into an event feed by *not closing* the reply
   stream and parking it in a list; push formatted lines; reap dead clients with a 0-byte write.
5. **Restart-in-place** via state serialization + `execvp(self, -o sock_fd)` keeps clients alive.

---

## 6. Case study: i3 — the n-ary tree, reparenting frames, and a deferred X-push engine

i3 (4.19.1, `f7d5b89`, pure XCB) is the heavyweight X WM: a **reparenting** WM organized around a
single **N-ary tree of `Con` nodes**, a **libev** event loop, a **table-driven command parser**
generated at build time, a rich unix-socket **IPC**, and — its signature engineering — a
**two-phase deferred/batched** update engine that separates in-memory geometry computation from
X11 mutation.

### 6.1 Startup → loop → teardown (src/main.c)

`main()` (main.c:277). If given non-option args it acts as `i3-msg` instead of starting (opens the
IPC socket from the `I3_SOCKET_PATH` root atom and sends `RUN_COMMAND`). The WM path:
`xcb_connect`; `EV_DEFAULT` libev loop; prefetch XKB/SHAPE/BIG-REQUESTS/RandR; an **ICCCM
timestamp bootstrap** (a zero-length property append, then block for the resulting
`PROPERTY_NOTIFY` to capture a valid server `last_timestamp` for selection ownership); intern all
atoms via **X-macros**; `load_configuration`.

Becoming the WM is a **two-part handshake**: (a) the modern ICCCM **`WM_Sn` selection** — check
`xcb_get_selection_owner`, honor `--replace`, `xcb_set_selection_owner` on a tiny owner window,
announce via a `MANAGER` ClientMessage; **and** (b) the classic **SubstructureRedirect grab** —
`xcb_change_window_attributes_checked(root, XCB_CW_EVENT_MASK, {ROOT_EVENT_MASK})` (with
`SUBSTRUCTURE_REDIRECT` in the mask) checked by `xcb_request_check`; a returned `BadAccess` means
another WM already holds redirection → exit.

**The event loop is libev, not `xcb_wait_for_event`** (main.c:1075). Two watchers: an `ev_io` on
`xcb_get_file_descriptor(conn)` whose callback (`xcb_got_event`) is an **empty dummy** that only
wakes the loop; and an **`ev_prepare`** watcher (`xcb_prepare_cb`, main.c:129) that runs *just
before the loop sleeps* and drains **all** queued events with a `while(xcb_poll_for_event)` loop,
strips the generated-bit (`type = response_type & 0x7F`), and dispatches via `handle_event`, then
`xcb_flush`. Using `ev_prepare` guarantees XCB's queues are empty before blocking. Signals use
libev `ev_signal` watchers. Existing windows are adopted under an `xcb_grab_server` by
`manage_existing_windows` (manage.c:44) → `xcb_query_tree` → `manage_window` per child. Teardown is
an `atexit(i3_exit)` handler.

### 6.2 i3 is a reparenting WM — the frame model (the big contrast with dwm/bspwm)

Every `Con` owns an X11 **frame window** created once in **`x_con_init`** (x.c:129): an
`override_redirect` window (i3's own frames must not be redirected) carrying `FRAME_EVENT_MASK`
which **includes `SUBSTRUCTURE_REDIRECT`**. When `manage_window` (manage.c) adopts a client it
`xcb_reparent_window`s the client **into its leaf Con's frame** (blanking the client's event mask
first so the reparent's `UnmapNotify` isn't misread as the window closing, then restoring it), and
`xcb_change_save_set(INSERT)` so the client survives i3 dying. Because the frame holds
`SUBSTRUCTURE_REDIRECT`, the reparented client's own configure/map requests come back to i3 as
requests it arbitrates. Decorations (title bars, tabs, stacked headers) are **Cairo-drawn to an
off-screen pixmap** per frame and blitted — so split/stacked/tabbed *containers* are themselves
frames that draw the title stack. The `ignore_unmap` counter on each Con (data.h:655) swallows
i3's self-inflicted UnmapNotify events. This is the fundamental difference from dwm/bspwm:
**i3 interposes a WM-owned window between root and every client**, which is what buys it real
titlebars, tabs, and per-container decoration.

### 6.3 The tree: `struct Con` (data.h:643)

One node type for **everything from the X root down to a single window**.
`enum type = {CT_ROOT, CT_OUTPUT, CT_CON, CT_FLOATING_CON, CT_WORKSPACE, CT_DOCKAREA}`. The
hierarchy is uniform: `CT_ROOT → CT_OUTPUT (per monitor) → CT_CON "content" → CT_WORKSPACE →
nested CT_CON split containers → CT_CON leaf (has ->window)`; floating windows hang off a
workspace's `floating_head` under a `CT_FLOATING_CON` wrapper; docks under `CT_DOCKAREA`. Outputs,
workspaces, floating wrappers, dock areas are **all just Cons**, which is why one recursion handles
all of them.

Each Con carries **three intrusive BSD `TAILQ` lists** of children:
- `nodes_head` — tiling children in **layout order**;
- `focus_head` — the same children in **focus (MRU) order**;
- `floating_head` — floating children (workspaces only).

Plus membership entries linking each Con into its parent's `nodes_head`/`focus_head` **and** the
global `all_cons` list (used for tree-wide criteria scans). The dual layout-order/focus-order split
is the backbone of "focus stays where you left it" and of stacked/tabbed "focused child on top".
Key geometry fields: `rect` (frame, absolute), `window_rect` (client, relative to frame),
`deco_rect` (decoration). **`double percent`** is a node's fraction of its parent's split axis.
`layout_t layout ∈ {L_DEFAULT, L_STACKED, L_TABBED, L_DOCKAREA, L_OUTPUT, L_SPLITV, L_SPLITH}`.
`enum floating` is *ordered* (`AUTO_OFF/USER_OFF/AUTO_ON/USER_ON`) so `>= AUTO_ON` means "floating"
while still recording whether the app or the user chose it.

Contrast bspwm: i3 Cons are **n-ary** (a split container holds N children directly; five tabs =
one tabbed container with five leaf children), whereas bspwm is strictly binary (N windows = N
leaves through N−1 internal fence nodes).

### 6.4 Layout: the two-phase deferred pipeline (i3's signature design)

`tree_render()` (tree.c:452) is the whole story:
```c
mark_unmapped(croot); croot->mapped = true;
render_con(croot);      // PHASE 1: compute every rect in memory, ZERO xcb calls
x_push_changes(croot);  // PHASE 2: diff against cached X state, push MINIMAL changes
```

**Phase 1 — `render_con`** (render.c:43): recursive, side-effect-free. Insets `window_rect` by the
border style; short-circuits fullscreen; dispatches by layout. `precalculate_sizes`
(render.c:224) is where `percent` becomes pixels: `size[i] = lround(child->percent * total)` across
the split axis, then distributes ±1px rounding error until the sum matches exactly.
`render_con_split` walks children advancing x/y; **stacked** stacks title bars vertically and pushes
content down by `deco_height * children`; **tabbed** places tabs side-by-side at
`deco_rect.width = width/children`. Floating windows are rendered **last and above** everything in
`render_root`.

**Phase 2 — `x_push_changes`/`x_push_node`** (x.c:1281 / x.c:939): the batched update engine. i3
keeps `con_state` records caching **what X currently believes** (mapped, rect, window_rect,
stacking) in `state_head` (desired) vs `old_state_head` (current). `x_push_changes`: (1) narrows
all frame masks to *just* `SUBSTRUCTURE_REDIRECT` so i3's own restacking doesn't echo back; (2)
pushes the window stack bottom-to-top emitting `xcb_configure_window(SIBLING|STACK_MODE=ABOVE)`
**only for siblings whose order changed**, updating `_NET_CLIENT_LIST[_STACKING]`; (3) `x_push_node`
recurses and, **only when a cached rect differs**, resizes the frame (recreating the pixmap),
resizes the child, sets `_NET_FRAME_EXTENTS`/`WM_STATE`, maps as needed; (4) computes focus and
**only if it changed** issues `xcb_set_input_focus` (or `WM_TAKE_FOCUS`), masking off the client's
`FOCUS_CHANGE` so it doesn't echo; (5) does all unmaps **after** maps/focus (so a new fullscreen
client is mapped+focused before the old one is unmapped — no black flash); (6) copies desired order
into cached order and flushes. **Net: one `tree_render()` computes the entire desired world in
memory, then emits the minimal batched set of X requests.** This is why i3 feels instant and doesn't
flicker — and it's a strong model for how ntwm should diff declared layout against dispd's current
state rather than blindly re-pushing everything.

### 6.5 Input: keysym translation, grabs, dispatch

A `struct Binding` (data.h:306) stores an `event_state_mask` (low 16 bits = modifiers, high 16 =
XKB group), a `symbol` (keysym name) or raw `keycode`, a `keycodes_head` list of resolved
`(keycode, modifiers)` pairs, and a `command` string. `translate_keysyms` (bindings.c:449) uses
**libxkbcommon** to expand each symbolic binding into every keycode that produces that keysym under
base/shift/numlock states and each XKB group — re-run on `MappingNotify`/XKB notifies.
`grab_all_keys` (bindings.c:155) issues `xcb_grab_key` on the **root** for each `(keycode, mods)`
**four times** (mods, +NumLock, +CapsLock, +both). A `KeyPress` → `handle_key_press`
(key_press.c:18) → `get_binding_from_xcb_event` (strips CapsLock, folds XKB group bits) →
`get_binding` (matches state+keycode against `keycodes_head`, handles the press/release state
machine) → `run_binding` → **`parse_command`** → `tree_render()`. It also emits an IPC `"binding"`
event.

### 6.6 IPC and the generated command parser

**IPC wire protocol** (include/i3/ipc.h): a **UNIX socket** (path advertised via the
`I3_SOCKET_PATH` root atom). Every message is a packed header
`{ char magic[6]="i3-ipc"; uint32_t size; uint32_t type; }` + payload. Request types:
`RUN_COMMAND=0, GET_WORKSPACES, SUBSCRIBE, GET_OUTPUTS, GET_TREE, GET_MARKS, GET_BAR_CONFIG,
GET_VERSION, …`. **Events set the high bit** (`I3_IPC_EVENT_MASK = 1<<31`) so a subscriber can tell
an async event (`WINDOW`, `WORKSPACE`, `BINDING`, `SHUTDOWN`, `TICK`…) from a reply on the same
socket. The server (ipc.c) watches each client fd with libev, reads header+payload, and dispatches
through a **handlers table** indexed by message type (`handle_run_command`, `handle_tree`,
`handle_subscribe`…). `i3-msg`, i3bar, and third-party bars all speak this. i3bar is spawned per
configured bar and connects back over this socket.

**The command parser is a build-time-generated table-driven state machine.** `parser-specs/
commands.spec` (a list of `state NAME:` blocks with `'literal'/token -> [call func();] NEXT_STATE`
lines) is compiled by `generate-command-parser.pl` into three headers (state enums, per-state token
tables, and a `GENERATED_call` dispatch `switch`). `parse_command` (commands_parser.c:158) starts in
`INITIAL` and, per token position, tries each token of the current state (case-insensitive literal,
number, quoted string, or end-boundary), pushing captured `$identifier` values onto a parser stack,
transitioning states, and firing `GENERATED_call` (which invokes the real `cmd_*` in commands.c
with args popped off the stack) at `__CALL`. Worked example — **"move container to workspace 3"**:
INITIAL:`move`→MOVE; MOVE consumes `container`,`to` (no-ops), `workspace`→MOVE_WORKSPACE;
MOVE_WORKSPACE: `3` matches `workspace = string` → `call cmd_move_con_to_workspace_name("3")`.
Criteria (`[class="..."]`) reset a `current_match` and, at `]`, walk global `all_cons` running
`match_matches_window` to build the target set subsequent commands iterate. The **config parser
(config.spec + config_parser.c) reuses the exact same machinery**, dispatching to `cfg_*` directives
— which is why i3's command and config languages share syntax.

### 6.7 i3 → ntwm takeaways

1. The **deferred two-phase render** (compute-in-memory then diff-and-push-minimal against cached
   server state) is the single best idea to steal: ntwm should compute a full desired layout and
   have dispd (or ntwm's push layer) apply only the deltas, batched.
2. The **generated table-driven command parser** shared between the IPC command language and the
   config language is a clean way to get one grammar, good errors, and easy extension.
3. **Reparenting** is what enables real titlebars/tabs/stacked decorations; it costs the
   `ignore_unmap`/save-set bookkeeping. Decide early whether ntwm/dispd wants decoration frames
   (i3/awesome style) or borders-only (dwm/bspwm style).
4. The libev **`ev_prepare`-drains-then-flushes** loop discipline (empty the queues before sleeping)
   generalizes to any reactor multiplexing a server fd + control IPC.

---

## 7. Case study: awesome — a thin XCB core exposing X objects to a Lua policy layer

awesome (`26454fb`, post-4.3, C99/XCB + Lua 5.x) inverts the "config" idea entirely: the C core is
an **object-and-signal server for X**, and *all policy* — layouts, keybindings, rules, the entire
bar — lives in Lua. `rc.lua` is not parsed as data; it is **executed** as a program. The connective
tissue is a hand-rolled signal/emit bus plus a lazy **once-per-mainloop refresh** that batches all X
writes.

### 7.1 Startup → loop → teardown (awesome.c)

`main()` (awesome.c:569): SIGCHLD self-pipe (`signal_child` writes one byte; a GLib fd-watch does the
`waitpid` reaping — the classic async-signal-safe trick); GLib signal watchers; `xcb_connect` with
ARGB-visual selection (32-bit visual + own colormap → true titlebar/wibox transparency without a
compositor); `acquire_timestamp()` (the same zero-length-property-append → wait-for-PropertyNotify
timestamp bootstrap i3 uses); extension prefetch. Becoming the WM is a **two-gate** handshake just
like i3: (a) `acquire_WM_Sn()` (awesome.c:268) — the polite ICCCM `WM_S<n>` selection with
`--replace` support and a `MANAGER` ClientMessage; (b) the enforced gate — under `xcb_grab_server`,
`xcb_change_window_attributes_checked(root, EVENT_MASK, {SUBSTRUCTURE_REDIRECT})` + immediate
`xcb_request_check`; `BadAccess` ⇒ "another window manager is already running". Then `luaA_init`
loads the Lua VM; `screen_scan`; `scan()` (awesome.c:199) adopts existing windows (pipelined
`get_window_attributes`+`get_geometry`, skipping override-redirect/unmapped/withdrawn); `luaA_parserc`
executes `rc.lua`.

**The loop is GLib with a replaced poll function** (awesome.c:890): `g_main_context_set_poll_func(...,
a_glib_poll)` then `g_main_loop_run`. `a_glib_poll()` (awesome.c:447) wraps each iteration:
`awesome_refresh()` (flush all deferred work + `xcb_flush`) → assert the Lua stack is empty (a real
correctness guard: every C→Lua call must net-zero the stack) → `xcb_poll_for_event` (if an event is
pending, set poll timeout 0 so we don't sleep) → `g_poll` → `a_xcb_check()` drains and dispatches all
X events (coalescing consecutive `MOTION_NOTIFY` to the last). `event_handle` (event.c:1095) switches
on the response type via an `EVENT(type, cb)` table. Teardown (`awesome_atexit`, awesome.c:110) emits
the global `"exit"` signal, **reparents every client back to root** so they survive, saves client
order into the `AWESOME_CLIENT_ORDER` root property, `lua_close`. Restart = `awesome_atexit(true)` +
`execvp(self)` — a hard re-exec; there is no in-place config reload, so all state round-trips through
X properties.

### 7.2 The C↔Lua object system (the crux)

Two layers glued through the Lua registry: **classes** (`common/luaclass.c`, one static
`lua_class_t` per core type, with a parent chain `client→window`) and **objects**
(`common/luaobject.c`, one Lua userdata per C struct). Every exposed C struct begins with
`LUA_OBJECT_HEADER` (just `signal_array_t signals;`) so `client_t`, `tag_t`, … are castable to a
common `lua_object_t` base.

- **The C struct IS the Lua userdata.** `prefix_new(L)` (macro, luaobject.h:164) does
  `lua_newuserdata(L, sizeof(type))` — the struct bytes *are* the userdata payload — sets its
  metatable (stored in the registry keyed by the `lua_class_t*` pointer, bidirectionally), attaches
  an env "uservalue" table, and emits the class-level `"new"` signal. `luaA_class_get` recovers the
  class from any userdata by looking up its metatable in the registry.
- **Properties** are `{name, new, index, newindex}` callbacks kept in a name-sorted array per class
  (binary-searched). The metatable's `__index`/`__newindex` (`luaA_class_index`/`_newindex`,
  luaclass.c:412/476) resolve a Lua read `c.name` → property "name" → `luaA_client_get_name(L, c)`,
  and a write `c.fullscreen=true` → `luaA_client_set_fullscreen`. A miss falls through to a
  Lua-registered miss-handler, which is how the `awful` library bolts derived properties onto core
  objects. (`client.focus = c` is different: `focus` is a field of the *module table*, whose
  `__newindex` string-matches `"focus"` and calls `client_focus(c)` in C.)
- **The signal bus** (`common/signal.h`): a `signal_array_t` is a sorted array of
  `{id = a_strhash(name), sigfuncs = [Lua refs]}` — signals are keyed by a **hash of the name**,
  binary-searched. Handlers are Lua references anchored either in the object's env table
  (object-level connect) or the class (class-level connect).
- **`luaA_object_emit_signal`** (luaobject.c:266) is the engine: emit on the object first (run its
  own handlers) **then on its class** — so `rc.lua`'s `client.connect_signal("mouse::enter", …)`
  registers one class handler that services every client instance. Handlers are snapshotted before
  calling so a handler may disconnect mid-emission safely.
- **GC + strong refs.** C holds strong references to Lua-visible objects via a refcount side-table
  (`luaA_object_incref`/`decref` in a dedicated registry, luaobject.c:58/111) — e.g. `client_manage`
  does `client_array_push(&globalconf.clients, luaA_object_ref(L, -1))` so a managed client survives
  even with no Lua variable holding it. When Lua *does* collect a dead object, its metatable is
  swapped for a **poison** metatable whose `__index`/`__newindex` throw "already garbage collected"
  — except `.valid`, which returns `false` (hence the idiomatic `if c.valid then …`).

X events (`event.c`) and X **property changes** (`property.c`, via `HANDLE_PROPERTY` macros that
re-read `WM_HINTS`/`WM_CLASS`/`_NET_WM_ICON`/… and emit `property::…`) are *all* translated into
signal emissions. The C core emits `request::manage`, `request::titlebars`, `request::activate`, …;
Lua reacts. This signal-driven reactivity is the whole architecture.

### 7.3 Data structures — and the set-membership tag model

- **`client_t`** (objects/client.h:101) starts with `WINDOW_OBJECT_HEADER` (carrying `window`,
  `frame_window`, `border_width`, …). **awesome is reparenting**: `frame_window` is the WM-created
  parent, `window` the client. Geometry is a triple: `geometry` (logical, the Lua-facing source of
  truth) plus `x11_client_geometry`/`x11_frame_geometry` (a **cache of what X believes**, used to
  skip no-op ConfigureWindows). `titlebar[4]` (TOP/RIGHT/BOTTOM/LEFT) are `{size, drawable_t*}`
  strips of the frame, cairo-painted — **not** separate windows. There is **no `tags` field on the
  client**.
- **`tag_t`** (objects/tag.h:36) is `{name, activated, selected, client_array_t clients}`. The
  authoritative membership is stored on the **tag** (`t->clients`), and a client's tag list is
  *computed* by scanning all `globalconf.tags`. This is a true **many-to-many set**: several tags may
  be `selected` at once *and* a client may be in several tags, so a client is visible iff
  `c->sticky || any(selected tag containing c)` (`client_on_selected_tags`, client.c:1684) — the
  union-of-viewed-tags behavior i3's single-workspace tree literally cannot express. (dwm/dwl/river
  encode the same idea more cheaply as a bitmask.)
- **`drawin_t`/`drawable_t`** back the wibox/bar: a real root-level X window + an off-screen
  `xcb_pixmap` wrapped in a `cairo_xcb_surface`. Lua widgets render into the cairo surface;
  `drawin_refresh()` blits the pixmap onto the window with `xcb_copy_area` once per loop.

### 7.4 Layout — Lua computes, C applies and coalesces

Layouts are pure Lua (`lib/awful/layout/suit/tile.lua`, `max.lua`, `fair.lua`, …). The dispatcher
`layout.arrange(screen)` (init.lua:240) calls the pure-Lua arranger which fills a weak-keyed table
`p.geometries[c] = {x,y,w,h}`, then loops writing each back with `c:geometry(g)`. `tile.lua`'s
`do_tile` does master/stack column math and **never touches X**. Arranging is *reactive* — triggered
by `property::geometry`, tag, and `raised`/`lowered` signals. On the C side `c:geometry(g)` →
`client_resize` → `client_resize_do` sets `c->geometry`, emits fine-grained `property::*` signals,
repositions titlebar drawables — **but issues no `xcb_configure_window`**. The X write is deferred to
`client_geometry_refresh()` (client.c:1945), run from `awesome_refresh`, which compares against the
`x11_*_geometry` cache, **skips if unchanged**, and otherwise emits exactly two ConfigureWindows
(frame + inner window) plus the ICCCM synthetic ConfigureNotify. Visibility is **banning**:
`client_ban`/`client_unban` (client.c:1789/2896) `xcb_unmap`/`xcb_map` the *frame*; orchestration is
lazy (`banning_need_update` just sets a flag; `banning_refresh` unbans-then-bans once per loop to
avoid flicker), all bracketed by the enter/leave-suppression "Bob Marley Algorithm" that records the
grab-server sequence range and drops enter/leave events within it (`should_ignore`, event.c:1065) so
the WM's own map/unmap churn doesn't cause spurious focus-follows-mouse.

### 7.5 Input — keys/buttons are Lua objects

A `keyb_t` (objects/key.h:28) = `{modifiers, keysym, keycode}`, a GC'd Lua object created by
`awful.key`. Grabbing is `xcb_grab_key` per resolved keycode on root (global) or on the client
window (per-client) via `xwindow_grabkeys`. A `KeyPress` → `event_handle_key` (event.c:757) resolves
the base keysym, finds the owning client's key array (or root's), and `event_key_callback` matches
each binding by keycode-or-keysym + modifiers and **emits `"press"`/`"release"` on the matching key
object** — whose Lua handler runs the action. Buttons are the same via `button_t` and
`event_handle_button`, using `xcb_allow_events(REPLAY_POINTER)` so a click on a client also reaches
the app. Exclusive `keygrabber`/`mousegrabber` modes back menus and interactive mouse-resize.

### 7.6 IPC / extensibility — D-Bus surfaced as signals

`a_dbus_init` (dbus.c:648) rides the **same GLib loop** (bus fd → `GIOChannel` watch). An incoming
D-Bus message is turned into a Lua table and emitted as a signal keyed by **interface name**
(`signal_object_emit(L, &dbus_signals, interface, …)`); if a reply is expected, the handler's return
values are marshalled back. The Lua-facing `dbus.*` primitives (`request_name`, `connect_signal`,
`emit_signal`) are all the C side knows; `awesome-client` and the `org.awesomewm.awful` interface are
implemented **in Lua** on top — external code injects Lua by sending a method call whose handler
`loadstring(code)()`s it. Config = executed `rc.lua`; reload = restart.

**awesome → ntwm takeaways:** (a) the **object+signal** decomposition — expose each server entity as
a handle with typed properties and a name-hashed signal bus, translate every server event into a
signal, and let an out-of-core policy layer react — is a very clean extensibility model if ntwm ever
wants scripting; (b) awesome independently arrives at the **same deferred-batch discipline** as i3
(`awesome_refresh` once per loop, geometry cache to skip no-op writes) — three of the serious WMs
(i3, awesome, and implicitly dwl via the configure-serial gate) converge on "mutate in memory, diff,
push minimal once per loop," which is a strong signal ntwm should adopt it; (c) reparented frames buy
cairo titlebars/tabs at the cost of `ignore_unmap`/save-set/enter-leave-suppression bookkeeping.

---

## 8. Case study: river — a display server that delegates the *entire* WM to an external process (the ntwm blueprint)

> **Version note.** The snapshot cloned (`d4fef52`) is the **rewritten, non-monolithic river**, not
> the older dynamic-tiling river. This matters enormously and is a *better* reference than the
> classic version. `river/README.md:11-23` states it: *"river is a non-monolithic Wayland
> compositor. Unlike other Wayland compositors, river does not combine the compositor and window
> manager into one program. Instead, users can choose any window manager implementing the
> **river-window-management-v1** protocol… If you are looking for the old dynamic tiling version of
> river, see river-classic."* `main.zig:239-281` (`detectClassic`) will even `fatal()` and refuse to
> start if it finds `riverctl` in your init. So the old `river-layout-v3`/`rivertile`/`riverctl`/
> `Layout.zig`/`Control.zig` are all **gone**; there are no tags, no layout, no focus policy, and no
> keybindings *in the compositor at all*. river is now the purest existing implementation of exactly
> ntwm's architecture: a policy-free display server + a separate WM process over an IPC protocol.

### 8.1 Two processes, one socket

- **`river`** = the compositor = pure mechanism. Owns GPU/DRM, the wlroots scene graph, input
  hardware, and all standard Wayland globals **plus** a set of private `river_*` globals.
- **The window manager** = a *separate, ordinary Wayland client* (e.g. `tinyrwm`) that connects to
  river's `$WAYLAND_DISPLAY` and binds the **`river_window_manager_v1`** global. Only one may bind at
  a time (a second gets `sendUnavailable()`, `WindowManager.zig:118-121`).

river launches the WM exactly as X launches a session: `main.zig:166-207` `fork()`+`execve("/bin/sh",
init)` on `$XDG_CONFIG_HOME/river/init` **after** putting `WAYLAND_DISPLAY` in the child's env; that
init script spawns your WM binary. On exit river kills the child's whole process group. **The load-
bearing idea: the "window manager" is just another client of the display server** — which is
precisely ntwm↔dispd.

### 8.2 Startup → loop (Server.zig, main.zig)

`Server.init()` (Server.zig:117-246) is the canonical wlroots bringup, same order as dwl:
`wl.Server.create()` (the `wl_display`, owns the socket + event loop) → `getEventLoop()` →
`wlr.Backend.autocreate(loop, &session)` → `wlr.Renderer.autocreate` → `wlr.Allocator.autocreate` →
`wlr.Compositor.create` → dozens of protocol globals as struct fields → river's own subsystems
(`wm.init()`, `xkb_bindings`, `layer_shell`, `scene`, output manager, input manager, lock manager) →
`wlr.Scene.create()`. Then `main.zig`: `addSocketAuto` → `backend.start()` → **`wl_server.run()`**
(the blocking epoll dispatch). Signals are folded into the same loop via `loop.addSignal(SIG.INT,
terminate, …)` rather than async handlers. The listener pattern is dwl's in Zig clothing: a
`wl.Listener(T)` struct field initialized `.init(handler)` (= `wl_listener` with `.notify` set),
registered with `signal.add(&listener)` (= `wl_signal_add`), and the handler recovers its owner via
`@fieldParentPtr("field", listener)` (= `wl_container_of`). Protocol bindings are generated from XML
by zig-wayland's `Scanner` (`build.zig:90-95` adds `protocol/river-window-management-v1.xml`), so the
interface `river_window_manager_v1` becomes the Zig type `river.WindowManagerV1`, requests become a
tagged union, events become `sendXxx()` methods.

### 8.3 The protocol — `river-window-management-v1` (the whole point)

8 interfaces (`protocol/river-window-management-v1.xml`, v5): `river_window_manager_v1` (handshake),
`river_window_v1` (one window), `river_node_v1` (a positionable/stackable node),
`river_output_v1` (an output + its rectangle), `river_seat_v1` (focus + pointer),
`river_pointer_binding_v1`, `river_shell_surface_v1` (the WM's own surfaces, e.g. bars),
`river_decoration_v1`.

**The core model is a globally double-buffered transaction** split into two disjoint state
categories (xml:38-110): *window-management state* (window dimensions, fullscreen, keyboard focus,
keybindings — changed only in a **manage** sequence) and *rendering state* (node position, render
order, borders, hidden/shown — changed in manage **or** render). The loop, verbatim (xml:85-104):

```
1. Server sends all state changes since last sequence, then `manage_start`.
2. WM sends requests changing window-mgmt/rendering state, then `manage_finish`.
3. Server pushes new state to the actual apps and waits for their acks.
4. Server sends the resulting real window dimensions to the WM, then `render_start`.
5. WM sends requests changing rendering state, then `render_finish`.
6. If dimensions changed, loop to 4; if window-mgmt state changed, loop to 1.
```

This is a *generalized* form of the classic layout protocol's `layout_demand → push_view_dimensions
→ commit(serial)`: instead of one demand river streams typed events (`window`, `output`, `seat`,
`dimensions_hint`, `app_id`, key `pressed`, `pointer_enter`, …); instead of `push_view_dimensions`
the WM issues typed requests (`propose_dimensions`, `node.set_position`, `focus_window`,
`set_borders`, …). **The "commit serial" role is played by the phase handshake itself** — the phase
is tracked server-side and an out-of-order `*_finish` is a protocol error.

Server-side state machine (`WindowManager.zig:31-39`):
```zig
state: union(enum) { idle, manage, inflight_configures: u32, render } = .idle,
```
- Any policy-relevant change calls `dirtyWindowing`/`dirtyRendering` (:240-263), which sets a flag and
  schedules **one idle callback** (`event_loop.addIdle`) — coalescing many changes into one
  transaction per loop iteration (the same "batch once per loop" discipline as i3/awesome).
- `manageStart()` (:303-349): `idle→manage`, emits all pending events across outputs/windows/seats,
  then `wm_v1.sendManageStart()` **and arms a 3-second watchdog** (`postError(.unresponsive)` +
  disconnect if the WM hangs — crash isolation the in-process WMs can't have).
- `manageFinish()` (:351-386): on the WM's `manage_finish`, turn each window's requested state into a
  real `xdg_toplevel.configure` to the app, count acks needed → `inflight_configures`.
- `notifyConfigured()` (:421-427): each app's `ack_configure` decrements; at zero → `renderStart()`.
- `renderFinish()` (:457-562): on the WM's `render_finish`, **this is where rendering state finally
  hits the scene graph** — reparent nodes into scene layers, apply stacking, `commitOutputState()`,
  and only *then* `input_manager.processEvents()` (:561) releasing buffered input, so the WM's
  focus/binding decisions can never race incoming keystrokes.
- **With no WM bound**, `manageStart`/`renderStart` see `wm.object == null` and call the `*Finish`
  immediately — the transaction completes but no `propose_dimensions`/`set_position` ever arrive, so
  windows are never positioned. A running WM is mandatory for river to be usable.

### 8.4 How geometry is applied — triple-buffered windows and list-order stacking

`Window` (`Window.zig`) is **triple-buffered**, the signature data-structure idea:
`wm_scheduled`/`wm_sent` (state to send the WM), `wm_requested` (what the WM asked of us),
`configure_scheduled`/`configure_sent` (state to send the app), `rendering_scheduled`/
`rendering_sent`/`rendering_requested` (geometry). The WM's `propose_dimensions(w,h)` request
(Window.zig:616-626) merely **stores** into `wm_requested` (after checking we're mid-manage, else
`postError(.sequence_order)`); `manageFinish()` turns it into a real `xdg` configure to the app;
when the app commits its actual size that flows back to the WM as a `dimensions` event; and only
`renderFinish()` (Window.zig:935-985) calls **`window.tree.node.setPosition(box.x, box.y)`** with the
coordinates the WM set. **Position and stacking are set on a `river_node_v1`, not the window**:
`WmNode.handleRequest` (WmNode.zig:80-136) is the entire "apply the layout" surface —
`set_position` stores x/y; `place_top`/`place_above`/`place_below` splice an intrusive `wl.list.Link`
in `wm.rendering_requested.list`. **Stacking order is literally the order of that linked list**, which
`renderFinish` walks front-to-back calling `raiseToTop()`. Borders are `set_borders` +
`rendering_requested.border`, drawn as scene rects (like dwl), and a window's scene node is enabled
only once its first real buffer exists (frame perfection).

**Tags do not exist in river.** Grep-confirmed: zero `tags` in `river/*.zig` and in the protocol.
Tags/workspaces are entirely the WM-client's concept — it keeps its own bitfields and decides which
windows to `hide`/`show` and where to `set_position` each sequence. This is the maximal expression of
"the display server encodes no policy" and the single most important lesson for ntwm.

### 8.5 Input and control — everything is a protocol object

Keyboard events are **queued, not handled inline** (`Keyboard.zig:216-218`
`seat.queueEvent(.keyboard_key)`) and drained only at the end of a transaction — the deliberate
anti-race design. Keybindings are a *separate protocol* `river-xkb-bindings-v1`: the WM calls
`get_xkb_binding(seat, id, keysym, modifiers)` and `enable`s it; river matches hardware keys
(`Seat.matchXkbBinding`), **eats** matched keys, and sends the WM a `pressed` event → the WM reacts in
the ensuing manage sequence. Unbound keys pass through to the focused app. Focus is the WM's
`river_seat_v1.focus_window(window)` request, applied server-side via
`seat.wlr_seat.keyboardNotifyEnter(surface, keycodes, &mods)` — the real `wlr_seat`. **There is no
`riverctl` and no separate control socket**: "drive the WM from outside" is subsumed — `exit_session`,
`stop`, `manage_dirty` are all protocol requests; any external control is the WM client's own affair.
Security is enforced at the socket: `globalFilter`/`blocklist` (Server.zig:292-397) ensure sandboxed
clients never even *see* the privileged `river_window_manager_v1`/`river_xkb_bindings`/screencopy
globals — only the trusted WM does.

**river → ntwm (the closest blueprint that exists):** ntwm is river's external WM; dispd is river's
compositor. Steal wholesale: (1) the **two-phase double-buffered transaction** (`manage`→apply→
`render`) with an explicit phase machine instead of ad-hoc serials; (2) **triple-buffered per-window
state** (what to tell the WM / what the WM asked / what the app actually did) so both sides diff
instead of clobber; (3) **wait-for-the-app-to-ack** before compositing the new size (frame
perfection); (4) an **unresponsive-WM watchdog** so a crashed ntwm can't wedge dispd (and can be
hot-swapped); (5) **buffer input during a transaction** so ntwm's focus/keybinding decisions never
race live keystrokes; (6) keybindings as **declared (keysym,mods) registrations** where dispd eats
matched keys and notifies ntwm, passing everything else to the app; (7) **stacking as an ordered
node list** the WM splices, not per-window z-indices; (8) a **capability filter at the pipe boundary**
so only ntwm may speak the privileged control protocol.

---

## 9. Case study: xmonad — the pure-functional `StackSet` and layout as a pure function

xmonad (`a9a8b5c`, Haskell over the `X11` Xlib FFI) matters for one idea: **the entire window-
management state is a single immutable value, and a layout is a pure function** — the cleanest
possible statement of the policy/mechanism split, held in-process.

### 9.1 The `X` monad and a textbook Xlib loop

`newtype X a = X (ReaderT XConf (StateT XState IO) a)` (Core.hs:170). `XConf` (read-only) holds the
Xlib `Display`, root window, config, and the resolved `keyActions :: Map (KeyMask,KeySym) (X ())`.
`XState` (mutable) holds one signature field: **`windowset :: !WindowSet`** — all window-management
state as one pure value. Startup (Main.hs:162-215): `openDisplay`, then the WM-making move
`selectInput dpy root (substructureRedirectMask .|. substructureNotifyMask)` (same X substrate as
dwm), `grabKey` per binding. The loop (Main.hs:254) is the quintessential Xlib model that river was
contrasted against: `forever $ handle =<< io (nextEvent dpy e >> getEvent e)` — one blocking
`nextEvent`, marshal to a Haskell `Event` sum, and `handle` `switch`es on the constructor
(`KeyEvent`→lookup in `keyActions`; `MapRequestEvent`→`manage`; `DestroyWindowEvent`/`UnmapEvent`→
`unmanage`).

### 9.2 `StackSet` — the zipper (the signature idea)

The whole state is an immutable, `Show`/`Read`/`Eq` value (StackSet.hs):
```haskell
data StackSet i l a sid sd = StackSet { current :: Screen…, visible :: [Screen…],
                                        hidden :: [Workspace…], floating :: Map a RationalRect }
data Workspace i l a = Workspace { tag :: i, layout :: l, stack :: Maybe (Stack a) }
data Stack a = Stack { focus :: a, up :: [a], down :: [a] }   -- the zipper
```
`Stack` is a **list zipper**: `focus` = the focused window, `up` = windows before it (reversed, so the
neighbour is O(1)), `down` = windows after. "Focus left" swaps one element between `up` and `focus`;
no index, no mutation, every op returns a *new* `Stack`. Because it is pure and immutable, xmonad
serializes it across a `--restart` (re-`exec` preserving state via `Read`), property-tests it with
QuickCheck, and reasons about it equationally. A **layout is a pure function** via `LayoutClass`
(Core.hs:282): `pureLayout :: layout a -> Rectangle -> Stack a -> [(a, Rectangle)]`. `Tall`
(Layout.hs:64) is the archetype: flatten the zipper, `tile frac r nmaster n` recursively splits the
screen rectangle (this is dwm's master/stack math expressed functionally), `zip ws rs`. Same inputs →
same geometries, no IO.

### 9.3 Diffing pure state onto X — `windows`

Every change funnels through `windows :: (WindowSet -> WindowSet) -> X ()` (Operations.hs:158). A
command (e.g. focus-down) is a pure `WindowSet -> WindowSet`; `windows` applies it and reconciles X:
compute `ws = f old`; commit it into `XState`; per screen run the pure `runLayout` → `[(Window,
Rectangle)]`; `restackWindows`; `moveResizeWindow` each window (Operations.hs:334); `mapWindow` newly
visible / `unmapWindow` now-hidden (with `waitingUnmap` accounting so xmonad ignores its *own*
UnmapNotify — the same self-inflicted-event problem dwm/i3 solve differently); `setInputFocus` to the
zipper's focus. So xmonad is: **keep the truth as one pure `WindowSet`; on every change recompute
geometry with a pure function, then issue the minimal Xlib calls to make the server match** — the same
"compute-then-diff-then-apply-minimal" shape as i3/awesome/river, expressed as pure-functional
state instead of mutable structs + dirty flags.

**xmonad → ntwm:** model ntwm's authoritative state as a single serializable value and layouts as
**pure functions `(usable_rect, window_list) -> [(window, rect)]`**. This is exactly what river moves
across the socket, and what makes layouts trivially testable and restart-safe. Keep the pure layout
core free of any IPC/side effects; a thin "apply/diff" layer talks to dispd.

---

## 10. Cross-cutting synthesis

### 10.1 The event loop, five ways

| WM | loop primitive | dispatch | multiplexes |
|----|----------------|----------|-------------|
| dwm | `XNextEvent` blocking | `handler[ev.type]` O(1) array | X socket only |
| xmonad | `nextEvent` blocking | `case` on `Event` constructor | X socket only |
| bspwm | **`select()`** | `switch` in `handle_event` / `process_message` | **X fd + control socket + rule pipes** |
| i3 | **libev** (`ev_io`+`ev_prepare`) | handlers table; `ev_prepare` drains XCB | X fd + IPC client fds + signals |
| awesome | **GLib loop** w/ custom `a_glib_poll` | `EVENT()` table; refresh once/iter | X fd + D-Bus fds |
| dwl / river | **`wl_event_loop`** (`wl_display_run`) | per-object `wl_listener` callbacks | Wayland client sockets + libinput + DRM + timers + signals |

Two shapes matter for ntwm. The **single-`select()`/reactor multiplexing a server fd + a control
socket** (bspwm, and the Wayland loops) is dispd's shape — one loop over the app pipe(s), the ntwm
control pipe, input, and vsync. The **"drain everything, then flush once"** discipline (i3's
`ev_prepare`, awesome's `awesome_refresh`, river's idle-scheduled transaction) is how all the serious
implementations avoid partial/interleaved updates.

### 10.2 Layout engines

- **Geometry is always a recursive subdivision of a rectangle**, and it is *substrate-independent*:
  dwm's `tile()` and dwl's `tile()` are line-for-line the same master/stack math over different apply
  calls; xmonad's `Tall` is the same math as a pure function; bspwm subdivides per binary fence
  (`apply_layout`); i3 subdivides per n-ary split by `percent` (`render_con`). ntwm's layout core
  should be pure geometry with a pluggable "apply."
- **The apply step is where they differ**: dwm/bspwm/xmonad/i3/awesome emit `XConfigureWindow`
  (i3/awesome batched+cached against a shadow of server state; dwm/bspwm only-if-changed);
  dwl/river mutate scene-graph node positions and wait for the client's configure-ack before
  compositing.
- **Binary (bspwm) vs n-ary (i3) vs flat master/stack (dwm/dwl/river/xmonad)** are three genuinely
  different structural choices. Flat + a "layout function" is by far the simplest and is what four of
  seven chose; a tree buys arbitrary nesting/tabs at real complexity cost.

### 10.3 Input and the key→action path

Every X WM does the same three steps: **grab** (`XGrabKey`/`xcb_grab_key` on root, for each of the
4 lock-modifier combinations, resolving keysym→all keycodes), **receive** a `KeyPress`, **match**
(keysym + cleaned modifier mask) against a table, **dispatch** a function. Wayland inverts the grab:
there is no passive grab — the compositor sees *all* keys via libinput and *chooses* to consume them
(dwl's `keybinding()`; river's declared xkb-bindings), forwarding the rest to the focused surface via
the seat. bspwm/river push this furthest: the keymap lives in a *different process* (sxhkd; the WM
client) and the WM/compositor only ever receives a finished command or a `pressed` notification.
**For ntwm, river's model is ideal:** ntwm *declares* (keysym,mods) to dispd; dispd eats matches and
notifies ntwm; unbound keys go to the app.

### 10.4 IPC / extensibility, four philosophies

- **Compile-time table** (dwm, dwl): config *is* C arrays; "reload" = recompile+restart. Zero runtime
  IPC. Simplest, least flexible.
- **Executed script + object/signal bus** (awesome): config is a running Lua program; the C core
  exposes X entities as scriptable objects with a signal bus; external drive via D-Bus surfaced as
  signals. Most flexible, heaviest.
- **Unix socket + command language** (i3, bspwm): a long-lived daemon takes structured commands over
  a socket (i3's `i3-ipc` framed binary + generated command parser; bspwm's NUL-separated argv +
  domain dispatch), plus a **subscribe** side-channel that pushes events to bars. This is the family
  ntwm↔dispd belongs to.
- **Typed IPC protocol objects** (river): "control" isn't a socket bolted on — it *is* the WM
  protocol; every external action is a typed request with server-side validation and capability
  filtering.

The recurring **subscribe/event-feed** pattern (bspwm's parked reply streams, i3's high-bit event
types, river's event stream, awesome's signals) is universal: status bars and external tools need a
push feed of state changes. dispd should offer one from the start.

### 10.5 Reparenting vs non-reparenting, and decorations

- **Non-reparenting** (dwm, bspwm, xmonad): manage the client window in place; the only decoration is
  the server-side **border pixel**; focus cue = border color. Simplest; no titlebars/tabs; no
  `ignore_unmap` bookkeeping. Hiding = move off-screen (dwm) or unmap.
- **Reparenting** (i3, awesome): interpose a WM-owned **frame window** (itself holding
  `SubstructureRedirect`) between root and each client; draw **titlebars/tabs/stacked headers** into
  the frame (Cairo pixmap → blit). Costs: reparent choreography, an `ignore_unmap`/save-set counter to
  swallow self-inflicted UnmapNotify, and enter/leave-event suppression during the WM's own window
  shuffles ("Bob Marley Algorithm"). Buys: real decorations and container-level tabs.
- **Wayland has no reparenting at all** (dwl, river): there is no window tree to reparent into;
  "decoration" is either client-side (the app draws it) or server-side rectangles/scene nodes the
  compositor draws. dwl draws 4 border rects per client; river's WM sets borders via protocol.

**For ntwm/dispd:** since you control the server, prefer the Wayland/dwl model — **decorations as
first-class scene primitives the server draws from ntwm's declared style**, not frame windows. It
sidesteps the entire reparenting-bookkeeping class of bugs while still allowing titlebars/tabs if you
want them later.

### 10.6 Becoming the WM / single-owner enforcement

Every X WM enforces "only one WM" the same way: try to select `SubstructureRedirect` on root and
treat `BadAccess` as "someone else owns it" (dwm's `checkotherwm`, bspwm's `register_events`, i3's and
awesome's checked `change_window_attributes`). The mature ones *also* run the ICCCM `WM_Sn` selection
handshake (i3, awesome) for graceful `--replace`. Wayland enforces it at bind time (river's
`sendUnavailable` on a second `river_window_manager_v1` bind). **dispd should reject a second ntwm at
the pipe-connect/bind boundary and expose a clean single-owner error** — the Wayland style, which is
simpler and race-free compared to the X probe.

---

## 11. Lessons for ntwm (the deliverable)

Ordered by leverage. The two references that map most directly onto "a WM process declaring layout to
a display server over a pipe" are **bspwm** (the socket-driven daemon split) and **new river** (the
external-WM-over-a-protocol compositor); most items below are distilled from those two, cross-checked
against i3/awesome/dwl/xmonad.

1. **Model dispd as pure mechanism, ntwm as pure policy — and put *no* layout, tags, focus policy, or
   keybindings in dispd.** river proves this is not only possible but cleaner: the server encodes
   nothing, the WM decides everything. Tags/workspaces are ntwm-side bitfields (dwm/river style); dispd
   only ever hears "show/hide window W" and "place node N at (x,y,w,h)".

2. **Make the ntwm↔dispd exchange a double-buffered, two-phase transaction, not a stream of
   fire-and-forget commands.** Adopt river's `manage → (ntwm mutates) → manage_finish → (dispd tells
   apps) → render → (ntwm sets positions) → render_finish` shape. Track the phase in a small state
   machine on the dispd side; an out-of-order finish is a protocol error. This makes multi-window
   relayouts **atomic** and is what prevents half-applied frames.

3. **Triple-buffer per-window state on both sides** (river's `wm_scheduled`/`wm_requested`/
   `rendering_requested`; i3's/awesome's desired-vs-cached shadow of server state). Never blindly
   re-push: compute the desired world, **diff against what dispd currently believes, and send only the
   deltas, batched once per loop iteration.** i3, awesome, dwm, bspwm, and xmonad all independently do
   only-if-changed pushes; it is the universal performance/flicker fix.

4. **Wait for the app to ack its new size before compositing it** (dwl's configure-serial gate held in
   `c->resize` + the `rendermon` skip; river's `inflight_configures`). This is how you get flicker-free
   resizes; a naive "resize then draw" always tears.

5. **One reactor loop in dispd multiplexing app pipes + the ntwm control pipe + input + vsync**, with a
   strict **drain-then-apply-then-flush-once** discipline (bspwm's `select`; i3's `ev_prepare`;
   awesome's `awesome_refresh`; the `wl_event_loop`). Single-threaded ⇒ commands and events never race,
   no locks.

6. **Keybindings: ntwm declares (keysym/vk, modifiers); dispd owns the keyboard, eats matches and
   notifies ntwm, forwards everything else to the focused app** (river's `river-xkb-bindings-v1`;
   dwl's `keybinding()`). Do **not** put the keymap in dispd. On NT you'll register a low-level keyboard
   hook or `RegisterHotKey`-equivalent in dispd; the *binding table* lives in ntwm.

7. **Buffer input during a transaction** (river queues keys, releases them only in `renderFinish`) so
   ntwm's focus/binding decisions can't race live keystrokes.

8. **Give the control channel a subscribe/event-feed** from day one (bspwm's parked reply streams +
   `put_status`; i3's high-bit event types; river's event stream). Panels/bars and any external tool
   need a push feed of focus/window/tag changes. Reuse the same pipe: a subscribe request that keeps
   the reply channel open.

9. **Wire format: keep it dead simple.** bspwm's entire client is ~117 lines: NUL-separated-argv
   request, streamed reply, one sentinel byte for failure. That is a fine literal spec for the ntwm↔
   dispd pipe if you don't need the richer typed-object protocol; if you do want typed objects, copy
   river's generated-from-schema approach so both sides stay in sync.

10. **Decorations as server-drawn scene primitives, not reparented frames.** Because you own dispd,
    follow dwl/river: borders/titlebars are rectangles/nodes dispd draws from a style ntwm declares.
    You avoid the entire X reparenting bookkeeping class (`ignore_unmap`, save-set, enter/leave
    suppression) that i3/awesome must carry.

11. **Enforce single-owner at the bind/connect boundary** (river's `sendUnavailable`), not via a racy
    probe. dispd rejects a second ntwm with a clean error.

12. **Stacking = an ordered node list ntwm splices** (river's `wm.rendering_requested.list` with
    `place_above`/`place_below`), which dispd walks to set z-order — simpler and less error-prone than
    per-window z-indices.

13. **An unresponsive-ntwm watchdog in dispd** (river's 3s timer → disconnect). A crashed or hung WM
    must not wedge the display; ideally dispd can survive an ntwm restart and let it re-attach — mirror
    bspwm's restart-in-place (serialize state, re-exec, keep the socket + clients alive) and river's
    hot-swappable WM.

14. **Model ntwm's authoritative state as one serializable value; layouts as pure functions
    `(usable_rect, [windows]) -> [(window, rect)]`** (xmonad's `StackSet`+`pureLayout`). Keeps layout
    trivially testable and makes ntwm restart/state-transfer clean. The pure core never touches the
    pipe; a thin apply/diff layer does.

15. **Adopt an ICCCM/EWMH-equivalent property layer early** — a typed way for apps to declare
    title/class, min/max/increment **size hints** (dwm's `applysizehints` shows how much apps depend on
    this — terminals snap to cells), dialog/floating type, "please close", "fullscreen", and for ntwm to
    publish the client list + active window. Retrofitting hints later is painful; every X WM read
    treats these as load-bearing.

16. **Adopt/graceful-close protocol**: a WM-delete-window equivalent (ask nicely, fall back to a hard
    kill) — dwm's `killclient`/`sendevent`, bspwm/river `close`. And an **adopt existing windows** path
    if dispd can start with clients already present (dwm's `scan`, bspwm's `adopt_orphans`); on the pure
    Wayland/dispd model this may be unnecessary since dispd is always up before any app, which is a
    simplification worth keeping.

---

## 12. Citation index (files/functions actually read)

**dwm** (`44dbc68`, `dwm.c`): `checkotherwm`:459, `setup`:1539, `scan`:1394, `run`:1382, `manage`:1031,
`maprequest`:1101, `configurerequest`:581, `arrange`:381/`tile`:1687/`monocle`:1113, `resize`:1278/
`resizeclient`:1286, `restack`:1357, `focus`:788/`setfocus`:1472, `grabkeys`:952/`keypress`:1000/
`grabbuttons`:931/`buttonpress`:417, `applysizehints`:313/`updatesizehints`:1961, `showhide`:1629,
`unmanage`:1779, `updategeom`:1867, `handler[]`:245, `Client`:86/`Monitor`:113; config tables in
`config.def.h` (`keys`:64, `buttons`:103, `layouts`:41, `rules`:24).

**dwl** (`a2d03cf`, `dwl.c`): `setup`:2447, `run`:2240, `createnotify`:1121, `mapnotify`:1739,
`resize`:2207, `tile`:2715, `arrange`:507, `focusclient`:1404, `keypress`:1630/`keybinding`:1610,
`createmon`:1040, `rendermon`:2152 (the configure-serial render-skip), `setfloating`:2334/
`setfullscreen`:2349, `Client`:105/`Monitor`:187, layer enum:88; abstraction in `client.h`
(`client_set_size`:341, `client_send_close`:302, `client_notify_enter`:292).

**bspwm** (`e11eff4`): `main`/select-loop `bspwm.c:94,212-273`, `setup`:349/`register_events`:465,
`node_t`/`desktop_t`/`monitor_t` `types.h:254,282,297`, `insert_node` `tree.c:291`/`arrange`:43/
`apply_layout`:73, `handle_message`/`process_message` `messages.c:46,86`, `cmd_subscribe`:1329/
`put_status`/`print_report` `subscribe.c:144,97`, selectors `query.c:533`, `window_draw_border`
`window.c:423`, `bspc.c` wire format :78-105.

**i3** (`f7d5b89`): `main`/loop `main.c:277,1075,129`, `manage_window`/`manage_existing_windows`
`manage.c`, `x_con_init`/`x_push_changes`/`x_push_node` `x.c:129,1281,939`, `render_con`/
`precalculate_sizes` `render.c:43,224`, `tree_render` `tree.c:452`, `Con` `data.h:643`, `Binding`
`data.h:306`, `translate_keysyms`/`grab_all_keys` `bindings.c:449,155`, `handle_key_press`
`key_press.c:18`, IPC `include/i3/ipc.h` + `ipc.c`, `parse_command` `commands_parser.c:158` +
`parser-specs/commands.spec` + `generate-command-parser.pl`.

**awesome** (`26454fb`): `main`/`a_glib_poll`/`awesome_refresh` `awesome.c:569,447` + `event.h:42`,
`acquire_WM_Sn`:268, `scan`:199, `awesome_atexit`/`awesome_restart`:110,539, object system
`common/luaobject.c` + `common/luaclass.c` (`luaA_object_emit_signal`:266, `luaA_class_index`:412),
signal bus `common/signal.h`, `client_t` `objects/client.h:101`, `tag_t` `objects/tag.h:36`,
`client_resize`/`client_geometry_refresh` `client.c:2522,1945`, `client_ban`/`banning_refresh`
`client.c:1789`+`banning.c:48`, input `objects/key.c`/`event.c:757`, D-Bus `dbus.c`, config
`luaa.c:1223,1265`.

**river** (`d4fef52`, new non-monolithic): `Server.init` `Server.zig:117-246`, `main.zig:41,161-211,
214-281`, transaction engine `WindowManager.zig:31-39,240-562`, `Window.zig:207-265,393-460,616-985`,
`WmNode.zig:80-136`, `Seat.zig:591-596,706-782,790-795`, `Keyboard.zig:212-222`, `Scene.zig:16-50`,
`Output.zig`, protocol `protocol/river-window-management-v1.xml:38-110,85-104` +
`protocol/river-xkb-bindings-v1.xml`, `README.md:11-26`, `build.zig:90-95`.

**xmonad** (`a9a8b5c`): `X` monad/types `Core.hs:82-171,282-317`, `StackSet`/`Stack`/`Workspace`
`StackSet.hs`, main loop/startup `Main.hs:162-215,254,447-467`, `windows`/apply
`Operations.hs:158-230,278-334,391-431`, `Tall`/`tile` `Layout.hs:56-106`.

