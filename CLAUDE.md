# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project purpose

This repository is a port of **fbc**, the FreeBASIC compiler
(https://github.com/freebasic/fbc), to **Haiku**. Work happens on the local
`haiku` branch (off `master`, which stays an unmodified upstream mirror).

**Current status: a working, self-hosted fbc exists and runs natively on real
Haiku hardware, with full console features, ThreadCall, and native graphics
drivers (both plain 2D and OpenGL).** All 4 language-dialect log-test suites
(`fb`/`fblite`/`qb`/`deprecated`) pass with zero failures, and the full
fbcunit unit-test suite passes 1,154,483 / 1,154,485 assertions (the 2
failures are a benign libm precision difference, see below — not a port
bug). See "Port status" below for exactly what's done, what's
known-missing, and where.

A Haiku dev machine is reachable over SSH: `ssh -i ~/.ssh/id_ed25519_haiku
user@192.168.1.38`. (The `haiku` alias in `~/.ssh/config` points at a stale
DHCP address, `192.168.1.30` — don't use it; use the explicit command above,
or fix the alias.) Always validate compiler/rtlib changes for Haiku by
actually building and running on that box — cross-compiling from this Linux
checkout only gets you the C-emission step, not proof it works.

Upstream references:
- fbc source/issues: https://github.com/freebasic/fbc
- Language & compiler docs: https://www.freebasic.net/wiki/DocToc
- `readme.txt` — user-facing overview, licensing, credits
- `todo.txt` / `changelog.txt` — upstream's own task list and history (not Haiku-specific)

## Big-picture architecture

fbc is **self-hosting**: the compiler itself is written in FreeBASIC
(`src/compiler/*.bas`), and building it from source requires an existing fbc
binary (bootstrapping — see Build workflow below). The repo has three main
components:

- **`src/compiler/`** — the fbc compiler, ~145 `.bas`/`.bi` files. Pipeline
  roughly: lexer/parser (`lex-*`, `parser-*`) → AST (`ast*.bas`) →
  intermediate representation (`ir-tac.bas`, `ir-hlc.bas`, `ir-gas64.bas`,
  `ir-llvm.bas` — one IR backend per code generator) → emission
  (`emit*.bas`, `emit_x86.bas`, `emit_SSE.bas`). `fbc.bas` contains
  command-line/target parsing, the `FB_COMPTARGET_*` table, and (critically
  for porting) all the per-OS linker-flag/crt-object logic in `hLinkFiles()`.
- **`src/rtlib/`** — the runtime library (`libfb`/`libfbmt`), written in C
  and some ASM. Structured as common code plus per-OS and per-arch
  subdirectories that get combined at build time: `src/rtlib/<os>/`,
  `src/rtlib/<arch>/` (e.g. `unix/`, `linux/`, `darwin/`, `haiku/`, `x86/`).
- **`src/gfxlib2/`** — the graphics library (`libfbgfx`). Every other
  Unix-family gfxlib2 backend is X11-based, and Haiku has no X11 server, so
  `src/gfxlib2/haiku/` is a **from-scratch native driver** against Haiku's
  BeAPI (`BApplication`/`BWindow`/`BView`/`BBitmap`) — no existing fbc
  gfxlib2 backend was a usable template (the closest precedent for "a
  from-scratch native windowed driver" is win32's GDI driver, structurally,
  not code-reusably). Since BeAPI has no C bindings, these drivers are
  **C++** (`gfx_driver_haiku.cpp`, `gfx_driver_opengl_haiku.cpp`), the only
  `.cpp` files anywhere in gfxlib2/rtlib — see "gfxlib2 driver" below for
  the build-system and design details. Scope: 32bpp truecolor and 8bpp
  indexed/palette SCREEN modes, keyboard, mouse, and a separate OpenGL
  driver against Haiku's OpenGL Kit (`BGLView`), both 2D-via-GL-texture and
  raw GL primitives.

Supporting trees:
- **`inc/`** — FreeBASIC header (`.bi`) files: stdlib-style (`crt/`,
  `dir.bi`, `datetime.bi`) and third-party library bindings (`SDL/`, `X11/`,
  `GL/`, etc.). `crt/` has its own per-OS dispatch separate from the
  compiler's — see "crt headers" below.
- **`tests/`** — the FB test suite: `fbcunit`-based "unit tests" (one
  monolithic linked binary covering every test `.bas` file) and
  directory-based "log tests" per language feature/dialect. Has its own
  `Makefile`/`common.mk`.
- **`doc/`** — man page, docs tooling (`fbdoc`, `fbchkdoc`, `libfbdoc`).
- **`contrib/`** — packaging (`contrib/rpm`, `contrib/deb`,
  `contrib/unix-installer`, and now `contrib/haiku/` for the haikuporter
  recipe), CI scripts, shell completion.

## Build workflow

fbc requires an existing fbc binary to build itself (chicken-and-egg
bootstrap). On Linux, a system fbc is normally available and can build this
tree directly: `make compiler FBC="fbc -i inc"`. For a brand-new target
platform where no native fbc exists yet (this is how the Haiku port was
bootstrapped), the flow is:

```sh
# 1. On a host with a working fbc (e.g. this Linux checkout), cross-emit
#    portable C source for the new target — this is the *same* mode
#    `bootstrap-dist` uses for every officially supported target:
./bin/fbc src/compiler/*.bas -m fbc -i inc -e -r -v -target haiku-x86_64
mkdir -p bootstrap/haiku-x86_64 && mv src/compiler/*.c bootstrap/haiku-x86_64/

# 2. Build rtlib natively on the target (needs the new src/rtlib/<os>/ files
#    and OS detection already wired into makefile/fb_config.h — see below):
make rtlib          # run ON the target, uname-driven auto-detection

# 3. Compile+link the cross-emitted .c files against that rtlib using the
#    *target's own* gcc — this is exactly what `make bootstrap-minimal` does:
make bootstrap-minimal
# -> bin/fbc now exists and runs natively on the new target

# 4. Self-host: rebuild fbc from source using itself
mv bin/fbc bin/fbc1 && rm -rf src/compiler/obj
make compiler FBC='bin/fbc1 -i inc'
```

Common make targets (see the header comment in `makefile` for the full list):
- `make` / `make all` — build compiler + rtlib + gfxlib2
- `make compiler` / `make rtlib` / `make gfxlib2` — build one component
- `make clean[-component]`, `make install[-component]`, `make uninstall[-component]`
- `make unit-tests`, `make log-tests`, `make warning-tests`, `make clean-tests` — wrappers around `tests/Makefile`

Useful makefile variables: `TARGET` (GNU triplet for cross-compiling),
`TARGET_OS`/`TARGET_ARCH` (override auto-detection directly), `FBC`/`CC`/`AR`,
`V=1` (verbose command lines), `CFLAGS`/`FBCFLAGS`/`FBLFLAGS`.

## Running tests

From `tests/`:

```sh
cd tests
make unit-tests FBC="<path-to-fbc> -i <path-to-inc>"
make log-tests  FBC="<path-to-fbc> -i <path-to-inc>"
```

Failures are written to `tests/failed-*.log`. To scope to one language dialect
use `FB_LANG=fb|fblite|qb|deprecated` (or `make log-tests-fb` etc. directly).
Warning-message and syntax-diagnostic tests live separately under
`tests/warnings/test.sh` and `tests/syntax/test.sh`, run with `FBC=<...>
./test.sh`, and compare against committed expected output (`git diff` shows
real regressions).

On Haiku: both `log-tests` (100% across all 4 dialects) and `unit-tests`
(1,154,483 / 1,154,485 assertions — the 2 failures are a benign `acos(-1.0f)`
libm precision difference, see "Confirmed" below) pass. `unit-tests` links
**every** test `.bas` file — including `tests/gfx/*.bas` — into one binary,
so it doubles as an end-to-end gfxlib2 smoke test.
**Gotcha specific to `unit-tests`**: `tests/fbcunit/lib/` is not
auto-created by its own makefile (it's assumed to already exist) — don't
`rm -rf` it when cleaning between runs, only `rm -rf fbcunit/obj`, or the
`ar` step silently fails with "No such file or directory" for reasons that
have nothing to do with the actual test content.

## Working on the Haiku port

### Port status (what's done)

- **makefile**: `haiku` OS detection (both `TARGET=` triplet and `uname`
  paths) and all recurring Unix-family filter lists updated. Haiku
  `ALLCFLAGS` sets `-DDISABLE_X11` only (no X11 server; OpenGL is supported,
  via Haiku's own OpenGL Kit, not X11/GLX — see the "OpenGL" bullet below).
  libffi and ncurses6
  (`termcap.h`/`curses.h`, `libncurses.so` → `libncursesw.so`, bundles the
  classic termcap API directly, no separate libtinfo split like on Linux)
  both work once `ncurses6_devel`/`libffi_devel` are installed — full
  console features (COLOR/LOCATE/INKEY$/WIDTH/etc.) and ThreadCall are both
  verified working. ncurses6 isn't part of the base OS install, unlike
  libffi — see the haikuporter recipe's `BUILD_PREREQUIRES`/`REQUIRES`.
  Also added `CXX`/`ALLCXXFLAGS` and parallel `LIBFBGFX_CXX` build rules (4
  variants: plain/PIC/MT/MTPIC) alongside the existing `_C`/`_S` ones — the
  only `.cpp` files in the whole tree are the two Haiku gfxlib2 drivers (see
  below), so
  this is otherwise a no-op for every other target.
- **`src/compiler/fb.bi`/`fb.bas`/`fbc.bas`**: `FB_COMPTARGET_HAIKU` added
  throughout. The important, Haiku-specific (not just "add to the Unix-family
  case list") logic lives in `hLinkFiles()` in `fbc.bas`:
  - **Haiku links every binary, including plain executables, as
    `-shared -no-undefined` ET_DYN** — confirmed via `gcc -v` on real
    hardware. There's a dedicated `case FB_COMPTARGET_HAIKU` for this (not
    shared with the Linux/BSD case), and `hTargetNeedsPIC()` forces PIC
    unconditionally for Haiku regardless of CPU family. **The PIC-forcing
    only actually takes effect for `FB_OUTTYPE_DYNAMICLIB`/Android** at the
    call site (fbc.bas step "4.5"), so plain `-c` (object-only) compiles on
    Haiku weren't getting `-fPIC` until `FB_COMPTARGET_HAIKU` was added to
    that condition too — found via a real link failure (`relocation
    R_X86_64_PC32 ... can not be used when making a shared object`) on a
    test file using raw inline asm, compiled separately via `-c` then linked
    into a `-shared` binary later.
  - **No `crt1.o` exists on Haiku at all.** Startup objects are `crti.o` +
    `crtbeginS.o` + `start_dyn.o` + `init_term_dyn.o` (verified by hand-
    linking on the box before encoding it) — also its own dedicated case,
    not the generic Unix crt-object block.
  - No explicit `-dynamic-linker` override is passed (Darwin's precedent —
    Haiku's own `ld` already defaults correctly).
  - Default libs are `-lgcc -lgcc_s -lroot` (Haiku has no separate libc; ffi
    lives in libroot too, but plain `libffi.so`/`libffi.a` are separate
    packages).
  - `hSetDefaultLibPaths()` adds `/boot/system/develop/lib` explicitly for
    Haiku — **this was a real, non-obvious bug**: Haiku's own binutils
    package bundles just enough (`libroot.so`, `libgcc_s.so`) under
    `/packages/binutils-*/.self/lib` for a bare `ld` to work at all, but
    anything from a separate HaikuPorts package (libffi, and presumably
    anything else linked via `-lfoo` in the future) only lives under
    `/boot/system/develop/lib`, which is **not** one of `ld`'s default
    search dirs (there is no `/usr/lib` or `/lib` on Haiku).
- **`src/rtlib/`**: `fb_config.h` recognizes `__HAIKU__` → `HOST_HAIKU`. New
  `src/rtlib/haiku/` (6 files): `io_mouse.c`/`io_multikey.c`/`io_serial.c`
  copied verbatim from the BSD family (they're pure OS-agnostic
  "unsupported" stubs there too); `sys_fmem.c`/`sys_getexename.c`/
  `sys_getexepath.c` written fresh against Haiku's native `image_info`
  (`get_next_image_info`, `<kernel/image.h>`) and `system_info`
  (`get_system_info`, `<OS.h>`) APIs. `src/rtlib/unix/time_setdate.c` and
  `time_settime.c` gained `#ifdef HOST_HAIKU` guards returning failure —
  Haiku has no `settimeofday()` at all.
- **`inc/crt/`**: Haiku isn't POSIX-identical to Linux/FreeBSD at the ABI
  level, so three headers needed real (not copy-paste) Haiku variants,
  written against measured `sizeof`/`offsetof` values from a live Haiku box:
  `inc/crt/sys/haiku/types.bi` (dev_t/ino_t/off_t/etc. widths — e.g.
  `off_t`/`ino_t`/`time_t` are 8 bytes signed, `mode_t`/`uid_t`/`gid_t` are 4
  bytes unsigned), `inc/crt/haiku/time.bi` (`struct tm` layout verified
  byte-for-byte against Haiku's real header, 48 bytes), `inc/crt/haiku/
  wchar.bi` (`mbstate_t`, 1112 bytes, opaque blob layout matched to Haiku's
  `_mbstate_t.h`), and `inc/crt/haiku/stdio.bi` (Haiku's own `<stdio.h>`
  declares `FILE` as a genuinely incomplete/opaque type — `sizeof(FILE)`
  doesn't even compile there — so the FB binding's `FILE` is an empty
  placeholder type too; the concrete bug this fixed was `stdin`/`stdout`/
  `stderr`, which the FreeBSD binding this used to fall back to aliases to
  `__stdinp`/`__stdoutp`/`__stderrp` (BSD naming) — Haiku exports plain
  `stdin`/`stdout`/`stderr` (confirmed via `nm -D libroot.so`), so anything
  touching those globals (fbcunit's own console output, for one) failed to
  link until this had a real Haiku binding instead of the generic-Unix
  fallback).
- **`src/gfxlib2/haiku/`** — the native graphics drivers. `gfx_driver_haiku.cpp`
  is the plain `GFXDRIVER` implementation — see fb_gfx.h's doc comments for
  the interface fbc expects: gfxlib2's core does all software rasterization
  into `__fb_gfx->framebuffer`, a plain malloc'd buffer; the driver's whole
  job is get a window on screen, blit that buffer into it, and turn OS input
  events into `fb_hPostEvent()` calls. `gfx_driver_opengl_haiku.cpp` is the
  separate OpenGL `GFXDRIVER` (see the "OpenGL" bullet under Port status
  below for its design). `gfx_haiku.c` (plain C, provides
  `__fb_gfx_drivers_list[]`/`fb_hScreenInfo()`, mirrors `gfx_unix.c`'s role),
  `fb_gfx_haiku.h`/`fb_gfx_opengl_haiku.h` (the `extern "C"` bridges
  declaring each driver struct symbol for the C file), `haiku_input.h`
  (keyboard-scancode/mouse-button mapping helpers shared by both drivers —
  the only code actually shared between them; extern "C" functions have
  plain, unmangled linkage regardless of any enclosing C++ namespace, so the
  two drivers' otherwise-identically-named `driver_*` hook functions can't
  coexist in the same static lib without one side renaming its own — the
  OpenGL driver's are all prefixed `gl_driver_*`).
  Design: a `BApplication`+`BWindow` pair runs on its own `pthread` (spawned
  via raw `pthread_create`, matching precedent in the X11/fbdev/win32
  drivers) since `BApplication::Run()` blocks pumping messages and FB
  programs don't yield control on their own; a `BBitmap` (colorspace
  `B_RGB32`) mirrors the framebuffer (their byte-order/channel-mask
  conventions match exactly — `MASK_A_32` in fb_gfx.h is `0xFF000000`, the
  same BGRA-in-memory layout Haiku's `B_RGB32` uses, so `driver_unlock()`'s
  screen refresh is a straight `memcpy` per scanline, no pixel-format
  conversion); an `FBMUTEX` (`fb_MutexCreate`/`Lock`/`Unlock`, reused from
  rtlib rather than raw pthread mutexes) guards the bitmap between the app
  thread's writes and the window's own looper-thread `Draw()`. `fbc.bas`
  links `-lbe -lstdc++` for Haiku's gfx case (not the X11 libs the other
  Unix targets use). Verified end-to-end on real hardware: compiled a
  program doing `ScreenRes`/`Cls`/`Line ... BF`/`Circle`/`PSet`, ran it
  detached over SSH, captured a screenshot with Haiku's `screenshot -s`
  CLI tool, pulled it back, and visually confirmed correct rendering (right
  colors, right shapes, right window title).
  - **Mouse and function keys**: `FBHaikuView` overrides `MouseDown`/
    `MouseUp`/`MouseMoved` and handles `B_MOUSE_WHEEL_CHANGED`, tracking
    position/buttons/wheel in `HaikuDriverState` under the same `FBMUTEX`,
    read back synchronously by `driver_get_mouse()` (confirmed via
    `gfx_getmouse.c`: `GetMouse` polls the driver directly, it's not
    event-queue-based like keyboard input) — button-press/release events
    post one `fb_hPostEvent` per button that actually changed, mirroring
    the X11 driver's `.button`-per-event convention (not the full bitmask).
    F1–F12 map via the key message's `"key"` field against
    `B_F1_KEY`..`B_F12_KEY` (gated on the `B_FUNCTION_KEY` modifiers bit),
    since `raw_char` for function keys is just a generic marker, unlike the
    other special keys.
  - **A real deadlock was found and fixed here**: `driver_exit()` calling
    `window->Quit()` from a different thread than the window's own looper
    (always true — the window runs on the driver's dedicated app thread)
    routes through `BWindow::QuitRequested()`, same as a user clicking the
    close box. The close-box handler is deliberately written to return
    `false` (so a user closing the window doesn't force-exit an FB program
    that wants to catch `EVENT_WINDOW_CLOSE` and decide for itself) — but
    that also silently blocked `driver_exit()`'s *own* shutdown, hanging
    the whole process forever with no window ever appearing. Fixed with an
    `exiting` flag set before calling `Quit()`, checked first thing in
    `QuitRequested()`. A second, related issue: `BApplication::Run()`
    doesn't return just because the last window closed — `driver_exit()`
    also has to `PostMessage(B_QUIT_REQUESTED)` to the `BApplication`
    itself, or `pthread_join()` on the app thread hangs too. Caught by
    running compiled test programs to completion and checking (via a
    *separate* SSH command, not just eyeballing a screenshot) that the
    process actually exited — a screenshot alone would have shown a normal
    -looking window and missed this entirely.
  - **8bpp indexed/palette `SCREEN` modes work too** (e.g. QB-style
    `SCREEN 13`-equivalent via `ScreenRes ..., 8`), turned out to need far
    less code than expected: `__fb_gfx->device_palette[256]` (packed
    `r | g<<8 | b<<16`) is already maintained by gfxlib2's own core
    (`gfx_palette.c`) independently of the driver, so `driver_set_palette()`
    stays a no-op, and `fb_hGetBlitter(32, is_rgb)` (`gfx_blitter.c`) hands
    back a ready-made, dirty-scanline-aware conversion function — no
    driver-side palette storage or per-pixel loop needed, just call it from
    `driver_unlock()` instead of the 32bpp path's direct `memcpy`. The
    `is_rgb` flag needed empirical, not theoretical, determination: a
    4-color-band test compiled and run with `is_rgb=FALSE` swapped red and
    blue (confirmed by screenshot — band 1 rendered blue instead of red,
    band 4, yellow, rendered cyan); `is_rgb=TRUE` renders all four bands
    correctly, meaning `device_palette`'s byte order already matches
    `B_RGB32` directly, consistent with the 32bpp truecolor path also
    needing no swap.
  - **A second real deadlock, found via user report of a window that
    wouldn't close**: `driver_unlock()` acquires `g_state.mutex` (the
    framebuffer/bitmap lock, via the caller's `driver_lock()`) and, *while
    still holding it*, calls `g_state.window->Lock()` (the BWindow lock) to
    safely blit and `Invalidate()`. Meanwhile `FBHaikuView::MouseMoved()`/
    `MouseDown()`/`MouseUp()`/the mouse-wheel `MessageReceived()` case all
    run on the window's own looper thread, which *implicitly* already holds
    the BWindow lock (that's how `BLooper` dispatches any message) — and
    they used to acquire the *same* `g_state.mutex` to update
    `mouse_x`/`y`/`z`/`buttons`. Two threads, two locks, opposite acquisition
    order (`mutex`→window vs. window→`mutex`): a classic lock-order-inversion
    deadlock, triggered whenever the mouse moved over the window while a
    draw call was in flight — confirmed with `gdb -p <pid> -batch -ex
    "thread apply all bt"` on the actual hung team, which showed the main
    thread stuck in `driver_unlock()`'s `BWindow::Lock()` and the window's
    looper thread stuck in `MouseMoved()`'s `fb_MutexLock()`, each waiting on
    the lock the other held. **Not caught by any prior verification in this
    port** — every earlier gfx test either didn't involve real mouse movement
    during a draw call, or the process was killed/screenshotted before the
    race window came up; it surfaced only when a human was watching the box
    live (moving the mouse) while a compiled test program drew. Fixed by
    giving mouse state its own `mouse_mutex`, entirely separate from the
    framebuffer/bitmap `mutex`, so no code path ever wants both locks in
    conflicting order. Reproduced reliably (5/5 runs) after the fix using a
    small helper (`set_mouse_position()` from `<os/game/WindowScreen.h>`,
    linked against `-lgame` — **not** `-lbe`, confirmed via `nm -D` on both;
    this symbol lives in `libgame.so`) run concurrently with a tight
    `Line`-drawing loop, verified via `ps`/screenshot that no window or
    process was left behind. **Lesson**: verifying a gfx driver's exit path
    alone isn't enough — any two independently-lockable resources (here: the
    BWindow lock and a custom app-level mutex) touched from both the main
    thread and window-event callbacks need one consistent acquisition order,
    or a dedicated per-resource lock, checked explicitly, not just "it
    rendered right and the process exited in my simple test."
- **OpenGL** — `src/gfxlib2/haiku/gfx_driver_opengl_haiku.cpp`, a *separate*
  `GFXDRIVER` (`fb_gfxDriverHaikuOpenGL`) from the plain one, registered
  after it in `gfx_haiku.c`'s `__fb_gfx_drivers_list[]` (the plain driver
  already rejects `DRIVER_OPENGL` in its `driver_init()`, so a `GFX_OPENGL`
  `ScreenRes` request falls through to this one, matching how
  `src/gfxlib2/unix/gfx_driver_x11.c`/`gfx_driver_opengl_x11.c` split the
  same way). Built against Haiku's OpenGL Kit (`BGLView`, `<opengl/
  GLView.h>`), **not** GLX/X11 — `-lGL` is now linked unconditionally
  alongside `-lbe -lstdc++` for any gfx-using Haiku program (`fbc.bas`);
  `-DDISABLE_OPENGL` is no longer set for Haiku in the makefile. A small
  shared header, `haiku_input.h`, holds the keyboard-scancode/mouse-button
  mapping helpers both drivers use (the only code shared between them —
  window/app/view lifecycle is intentionally separate, see below).
  gfxlib2's OpenGL support (`gfx_opengl.c`) has two independent modes,
  selected via `ScreenControl(SET_GL_2D_MODE, ...)` *before* `ScreenRes`:
  `OGL_2D_NONE` (the default: the FB program does real OpenGL rendering
  itself, e.g. via `inc/GL/gl.bi`, and calls `Flip` to swap buffers) and
  `OGL_2D_MANUAL_SYNC`/`OGL_2D_AUTO_SYNC` (the FB program keeps using
  ordinary `LINE`/`CIRCLE`/`PSET`/etc, and gfxlib2 core uploads
  `__fb_gfx->framebuffer` as a texture and draws a fullscreen quad —
  `MANUAL_SYNC` only on explicit `Flip`, `AUTO_SYNC` automatically after
  every drawing primitive). All three modes verified end-to-end on real
  hardware (Mesa's software `llvmpipe` renderer, confirmed via the
  `GalliumContext: ... llvmpipe` line every GL program prints on first
  context creation — this box has no hardware-accelerated GL): a 2D
  auto-sync test (rectangles + a circle via `LINE`/`CIRCLE`) rendered
  correctly and closed cleanly; a manual-sync animation (20 `Flip` calls)
  completed cleanly; a raw-primitives test (`glClear` + a `glBegin`
  triangle with per-vertex colors, linked straight against `-lGL`, no
  `ScreenControl` call at all) rendered a correctly gradient-shaded
  triangle. `GL_SCALE` (`ScreenControl(SET_GL_SCALE, n)` before `ScreenRes`)
  is supported too — the driver creates its window at `w*scale`×`h*scale`
  physical pixels while the logical framebuffer stays at `w`×`h`;
  `fb_hGL_SetupProjection()`/`fb_hGL_ScreenCreate()` (`gfx_opengl.c`,
  shared generic code, not driver-specific) already size the GL viewport by
  `scale` and upload the logical-size framebuffer as a `GL_LINEAR`-filtered
  texture, so the upscale-with-smoothing needed no driver-side pixel work
  at all — the only change needed was creating the window at the scaled
  size. Verified with `SET_GL_SCALE 2` at a 200×150 logical resolution: the
  window came up at a visibly ~400×300 physical size with correctly
  smoothed/upscaled content (screenshot-confirmed), `GET_GL_SCALE` read
  back `2`, and the default (`scale`=1, unscaled) path re-verified
  unaffected by the same code path.
  - **No BBitmap, no `Draw()` override, no framebuffer mutex** — a deliberate
    design difference from the plain driver, not an oversight. GL content
    lives entirely in the `BGLView`'s own front buffer, touched only from
    `gl_driver_unlock()`/`gl_driver_flip()`, both always called on the FB
    program's own thread (gfxlib2 core already serializes driver lock/unlock
    calls via `FB_GRAPHICS_LOCK`, `gfx_access.c`) — there's no second thread
    reading GL-drawn content the way the plain driver's window-looper thread
    reads the `BBitmap` in `Draw()`. This sidesteps that whole class of bug
    entirely rather than re-solving it; a `mouse_mutex` is still needed for
    the same reason as the plain driver (mouse callbacks run on the window's
    own looper thread, implicitly holding the `BWindow` lock).
  - **A real, non-obvious bug found via an actual test, not theory**: a
    raw-GL test program (`glClear`+`glBegin` triangle, no gfxlib2 2D calls
    at all) initially rendered a **solid black window** — no triangle, not
    even the clear color. Root cause: unlike GLX's `MakeCurrent` (a
    persistent per-thread binding that survives until explicitly released),
    `BGLView`'s context is only current for the calling thread **while
    `LockGL()` is held** — `gl_driver_init()` was calling `LockGL()`/
    `UnlockGL()` as a transient bracket around its own setup calls
    (`fb_hGL_Init()`/`fb_hGL_ScreenCreate()`), same as every other GL call
    in the file, so by the time `driver_init()` returned and the FB
    program's own (directly-linked, not routed through this driver at all)
    `glClear`/`glBegin` calls ran, there was no current context for them to
    affect. Fixed by having `gl_driver_init()` call `LockGL()` once and
    **deliberately never call the matching `UnlockGL()`** before returning,
    keeping the context current for the calling thread (the FB program's own
    thread) for the program's entire lifetime; `gl_driver_exit()` releases
    it. This is safe with `gl_driver_unlock()`/`gl_driver_flip()`'s own
    `LockGL()`/`UnlockGL()` brackets on top of that persistent hold because
    `BLocker` (what `LockGL()` wraps) is recursive for the owning thread.
    After the fix, the same test rendered a correctly per-vertex-shaded
    triangle. This is the clearest example yet in this port of the general
    lesson from the plain driver's deadlock: an OS's async/threading
    primitives (`BLocker`-based `LockGL()` here, `BWindow`'s lock there)
    often have real, non-obvious semantics that only surface by actually
    running code, not by pattern-matching against how the analogous GLX/X11
    upstream driver does it.
  - **Symbol collision across the two Haiku driver files**: an anonymous
    C++ namespace does **not** give `extern "C"` functions internal
    linkage/uniqueness — `extern "C"` always produces a plain, unmangled,
    externally-visible symbol, regardless of any enclosing namespace. Both
    driver files initially defined identically-named `extern "C"` functions
    (`driver_init`, `driver_exit`, `driver_lock`, etc., each only referenced
    via its own file's `GFXDRIVER` struct, never called by name from
    elsewhere), which linked fine individually but failed with "multiple
    definition of `driver_init`" etc. the moment both `.o` files landed in
    the same `libfbgfx.a`. Fixed by prefixing every one of the OpenGL
    driver's `extern "C"` driver-hook functions with `gl_` (`gl_driver_init`,
    `gl_driver_exit`, ...) — `fb_hGL_GetProcAddress` didn't need renaming, it
    was already uniquely named and only ever defined once across the whole
    gfxlib2 build.
  - Regression-verified after landing: full `log-tests` (zero failures, all
    4 dialects) and `unit-tests` (1,154,483/1,154,485, the same pre-existing
    benign libm-precision count) both re-run against a full self-hosted
    rebuild including this driver, no new failures.
- **Packaging**: `contrib/haiku/fbc-1.20.0.recipe`, a haikuporter recipe.
  **The full pipeline is now verified end-to-end, not just the underlying
  `make` commands**: built a real local tarball, ran actual `haikuporter`
  against a test copy of this recipe in a pre-existing local haikuports tree
  (`/boot/home/git/haikuports` on the box, already configured in
  `haikuporter.conf`), produced a genuine `fbc-1.20.0-1-x86_64.hpkg`,
  installed it with `pkgman install`, and confirmed `fbc` on `PATH` compiles
  and runs console, gfxlib2 (window opens, draws, closes, process exits
  cleanly), and ThreadCall (libffi) programs using **only** the packaged
  install — no build-tree paths involved. `pkgman uninstall` afterward
  restored the box.
  - **The chicken-and-egg bootstrap problem**: a clean haikuporter chroot has
    *no* existing fbc to self-host from (there's no prior Haiku fbc package —
    this port creates the first one). Fixed by vendoring pre-generated C
    (`bootstrap/haiku-x86_64/*.c`, ~353k lines / 10MB, committed to git,
    cross-emitted from this same source via
    `fbc -target haiku-x86_64 -e -r -m fbc -i inc <compiler sources>` on
    Linux) into the source tree — the same convention upstream fbc itself
    uses for new-target bring-up. `BUILD()` now does the real two-stage
    dance: `make bootstrap-minimal` (compiles the vendored `.c` with the
    *native* gcc against a freshly-built Haiku `libfb.a`, giving a first
    working `bin/fbc`), then `mv bin/fbc bin/fbc1 && make clean-compiler &&
    make compiler FBC="bin/fbc1 -i inc"` to self-host rebuild from the real
    `.bas` sources (mirrors `contrib/travis/build-and-test.sh`), then
    `make rtlib gfxlib2`. **Regenerate `bootstrap/haiku-x86_64/` whenever
    `src/compiler/*.bas` changes** (stale bootstrap C would still build, just
    from outdated compiler logic) — `rm -rf bootstrap && mkdir -p
    bootstrap/haiku-x86_64 && ./bin/fbc src/compiler/*.bas -m fbc -i inc -e
    -r -v -target haiku-x86_64 && mv src/compiler/*.c bootstrap/haiku-x86_64/`.
    This vendored bootstrap can be dropped in a future revision once a prior
    fbc-for-haiku release exists in HaikuPorts to build against instead.
  - **haikuporter's `file://` local-source handling has a real quirk**, worth
    knowing before ever testing a recipe locally again:
    `SourceFetcherForLocalFile.fetch()` (`HaikuPorter/SourceFetcher.py`)
    does `portBaseDir + '/' + uri` where `portBaseDir` is the recipe's *own*
    directory — it does **not** treat a `file:///abs/path` URI as absolute
    despite the factory correctly stripping exactly 7 chars (`file://`) off
    the front first. A path starting with `/` just gets string-concatenated
    onto `portBaseDir`, producing a nonexistent double-slash path. The
    reliable local-test pattern: copy (or symlink) the tarball **into the
    port's own recipe directory** and reference it by bare filename, e.g.
    `SOURCE_URI="file://fbc-1.20.0.tar.gz"` with the file sitting next to the
    `.recipe`. (A single-slash `file:/...` form is *not* an accepted scheme
    at all — falls through to "protocol ... is unsupported, sorry".)
  - **`PROVIDES`/`REQUIRES` naming for `_devel` sub-packages**: the `lib:`
    prefix names a *runtime* library provide (e.g. `lib:libffi`), not a
    devel-package provide — writing `lib:libffi_devel` in
    `BUILD_PREREQUIRES` fails dependency resolution ("Name not found") even
    though the package itself exists and is installable. The devel
    sub-package's own plain name works directly: `libffi_devel`,
    `ncurses6_devel` (confirmed by reading `PROVIDES_devel=` in the real
    `libffi`/`ncurses6` recipes — both list the bare `<name>_devel = $portVersion`
    form alongside a `devel:<libname>` form; either of those two forms
    resolves, `lib:<name>_devel` does not).
  - **`$jobArgs` already contains the full `-j N` flag** (or is empty for
    serial builds) — recipes should write `make target $jobArgs`, not
    `make target -j$jobArgs` (the latter fails with "the '-j' option requires
    a positive integer argument" whenever `$jobArgs` is empty, and would
    double up the flag when it isn't).
  - `INSTALL()`'s file layout (`make install-compiler install-includes
    install-rtlib install-gfxlib2 prefix=$prefix`) needed no changes — a
    `hello.bas` compiled and ran using only the staged install tree from the
    very first local test.
  - **Published and fully resolved**: the `haiku` branch is pushed to
    `github.com/yann64/fbc` and tagged `haiku-1.20.0`; the committed recipe's
    `SOURCE_URI` points at the real GitHub archive URL for that tag
    (`.../archive/refs/tags/haiku-$portVersion.tar.gz`), with the real
    `CHECKSUM_SHA256` filled in. **One non-obvious gotcha**: GitHub's
    archive tarball's top-level directory is named after the *repo*, not the
    upstream project — `fbc-haiku-$portVersion/` (repo `fbc`, tag
    `haiku-1.20.0`), not `fbc_haiku-haiku-$portVersion/` as an earlier
    placeholder guess assumed; `SOURCE_DIR` must match this exactly or
    haikuporter's unpack step silently looks in the wrong place. Verified
    end-to-end for real this time: downloaded the actual tag tarball from
    GitHub, ran haikuporter against the committed recipe completely
    unmodified (no local-file substitution), and it fetched over the network,
    built, and packaged successfully — installed and smoke-tested via
    `pkgman` same as every earlier local-tarball test.

### Known gaps / deliberately out of scope

- `set_window_pos`, `wait_vsync`, and `fetch_modes` are all implemented.
  `ScreenControl(SET_WINDOW_POS, x, y)`/`GET_WINDOW_POS` work via
  `BWindow::MoveTo()`/`Frame()`, verified by moving a window and confirming
  both the reported coordinates and the actual on-screen position.
  `ScreenSync` (`wait_vsync`) tries real `BScreen::WaitForRetrace()` first,
  falling back to sleeping `1/refresh_rate` seconds if that's unavailable.
  `SCREENLIST` (`fetch_modes`) returns a curated list of common resolutions
  capped to the desktop size when known. **All three hit the same real bug
  along the way**: `BScreen`'s default constructor silently returns a
  garbage 1×1 `Frame()` (not an error) when queried before any
  `BApplication` exists — which is exactly when `SCREENLIST` is normally
  called, to help a program pick a resolution *before* `ScreenRes`. Caught
  by comparing a pre-`ScreenRes` call (returned one bogus 1×1 "mode") against
  a post-`ScreenRes` call (returned 8 correct entries including the real
  desktop size) with the exact same code. Fixed by checking
  `BScreen::IsValid()` and skipping the desktop-size cap/entry entirely when
  it's false, rather than trusting `Frame()` blindly; `wait_vsync` and
  `fetch_modes` both guard on it now (`wait_vsync` doesn't hit this in
  practice, since it's only ever called after a screen already exists, but
  guards anyway since the cost of checking is negligible).
- **`ir-gas64.bas`'s `ctx.systemv` is `FALSE` for Haiku** (`_emitbegin()`
  hardcodes it `TRUE` only for `FB_COMPTARGET_LINUX`/`FB_COMPTARGET_FREEBSD`,
  matching a `!!!TODO!!! add to target options` comment already in
  `symb-struct.bas` next to the same hardcoded pair). On paper this selects
  a *different* code path for scalar-argument register assignment
  (`param_analyze()` in `ir-gas64.bas`: an independent-per-class SysV
  counter — up to 6 integer args in `RDI`/`RSI`/`RDX`/`RCX`/`R8`/`R9`, up to
  8 float args in `XMM0`-`XMM7` — vs. a shared-positional-counter Windows
  x64-style scheme when `ctx.systemv` is false) and struct-parameter
  classification (the same Linux/FreeBSD-only pair also gates
  `hGetReturnTypeGas64SystemV()` for struct *returns*, in `symb-struct.bas`).
  **Despite that, no incorrect result was found** in a dedicated real-
  hardware test battery built specifically to expose exactly this kind of
  divergence — see "Confirmed since the initial port" below. This is now a
  documented, low-priority puzzle rather than an open verification gap: the
  flag's theoretical effect doesn't reproduce as an observed bug, so either
  a separate/correct code path is actually used for genuine
  `Cdecl`/`Alias`-declared external calls (decoupled from
  `param_analyze()`), or `ctx.systemv`'s practical effect is narrower than
  its own code comments suggest. Worth adding `FB_COMPTARGET_HAIKU` to
  both hardcoded pairs anyway next time this file is touched, purely to
  remove the discrepancy and match Linux/FreeBSD's already-verified,
  principled path instead of relying on this empirical result forever — but
  not urgent given the test results.

### Confirmed since the initial port (no longer open questions)

- **`SetMouse` can now reposition the system cursor, in both gfx drivers.**
  The earlier limitation was that Haiku's `set_mouse_position()` (in
  `<os/game/WindowScreen.h>`, exported from `libgame.so` — **not**
  `libbe.so`, confirmed via `nm -D`) is documented as part of the
  fullscreen/game API (`BWindowScreen`), and wasn't confirmed safe to call
  from a plain windowed `BView`. Resolved in two steps: first, a standalone
  helper program (`moveit.cpp`, no window at all, just `main()` calling
  `set_mouse_position()` in a loop, built while diagnosing the mouse/draw
  lock-order-inversion deadlock — see above) proved the function works
  fine outside a `BWindowScreen` context. Second, wired it into both
  drivers' `driver_set_mouse()`/`gl_driver_set_mouse()`: `set_mouse
  _position()` takes *absolute* screen coordinates, but the `GFXDRIVER`
  contract's `x`/`y` are relative to the graphics drawing area (per
  `fb_gfx.h`), so the driver converts via `BView::ConvertToScreen()`
  (which needs the window locked) before calling it. Verified end-to-end
  with `SetMouse`/`GetMouse` round-trip tests in both drivers: moved the
  cursor to two different window-relative positions, and the driver's own
  mouse-tracking (updated from the real `MouseMoved` event the OS
  generates in response) read back the exact same coordinates both times,
  in both the plain and OpenGL drivers. `-lgame` added to Haiku's default
  gfx link libs (`fbc.bas`) — part of the base `haiku` package already, not
  a separate port, same as `-lbe`/`-lGL`.
- **ABI struct-passing/return conventions, empirically verified against
  real Haiku-gcc-compiled C code** (not just inferred from the closest BSD
  target anymore). Built a dedicated test battery (`abi_c.c` + `abi_test
  .bas`, not checked in — a one-off diagnostic, not part of the port):
  `Declare Function ... Cdecl Alias` calls from FB into real gcc-compiled C
  functions, and the reverse (FB `Function ... Cdecl Alias` functions
  called from C), covering:
  - Every SysV x86-64 eightbyte return/parameter classification for structs
    up to 16 bytes — integer-only (`S_R`/`S_RR`), float-only
    (`S_X`/`S_XX`), and both mixed orderings (`S_RX`/`S_XR`) — plus a
    20-byte struct forcing the memory-class (hidden-pointer) case.
  - A struct parameter preceded by 6 leading `Long` arguments (register-slot
    exhaustion interacting with a struct arg).
  - 6 interleaved `Double`/`Long` scalar arguments (3 of each) — the
    specific pattern where SysV's independent per-class register counters
    and a Windows x64-style shared positional counter would assign
    *different* registers if fbc used the wrong one.
  - 10 interleaved `Double`/`Long` scalar arguments (5 of each) — same idea
    but past the point (4 total args) where a Windows x64-style scheme
    would start spilling to the stack, while true SysV (caps of 6
    integer/8 float) would not; the sharpest test for exposing a
    register-vs-stack misclassification.
  All of the above passed byte-for-byte correct, in both call directions,
  on real hardware. See the "Known gaps" entry on `ctx.systemv` above for
  the one loose thread this raised (a theoretical divergence that didn't
  reproduce as an actual bug) — everything else about Haiku's `fb.bas`
  `targetinfo()` row (`FB_TARGETOPT_UNIX | CALLEEPOPSHIDDENPTR |
  RETURNINREGS | ELF`, no `RETURNINFLTS`/`STACKALIGN16`) is now considered
  verified rather than inferred. (`STACKALIGN16` was separately confirmed
  irrelevant to this port by reading the code, not by testing: it's gated
  to `FB_BACKEND_GAS`, the 32-bit x86 backend only, in
  `ast-node-call.bas` — this port only targets/tests 64-bit gas64.)
- **No `-dynamic-linker` override needed, confirmed (not just assumed):**
  `readelf -l` on a real fbc-built Haiku binary shows **no PT_INTERP segment
  at all** — Haiku's ELF loading has no equivalent of Linux's embedded
  interpreter path.
- **Building a `.so` (`FB_OUTTYPE_DYNAMICLIB`) works**, but needed its own
  fix: `start_dyn.o` (added for the executable case) provides `_start`,
  which references `main` — a real shared library has neither, so it's
  omitted for `DYNAMICLIB` output and `-e 0` is passed instead (confirmed by
  comparing against `gcc -shared -v` on the box). Verified end-to-end:
  compiled a `.so` exporting a `Cdecl` function, `DyLibLoad`/`DyLibSymbol`ed
  it from a separate program, called through the pointer, got the right
  answer.
- `tests/warnings/test.sh` and `tests/syntax/test.sh` (cross-target
  diagnostic-message tests, run *from* Haiku-hosted fbc, targeting
  dos/linux-x86/linux-x86_64/win32/win64) produce **zero diff** against the
  committed baseline — the Haiku-hosted compiler's cross-compilation
  diagnostics are byte-identical to upstream's.
- **The full fbcunit `unit-tests` suite passes**: 1,154,483 / 1,154,485
  assertions. The 2 failures (`optimizations/consteval.bas`,
  `acos(-1.0f)`) are Haiku's libm returning a value 1 ULP off from whatever
  fbc's own compile-time constant folder computes for the same input at
  that singular point (`acos(-1)` = exactly π) — a normal cross-platform
  libm precision difference, not a Haiku-port bug, and not something to
  chase further.
- **gfxlib2 works**, not just links: a real program using
  `ScreenRes`/`Cls`/`Line`/`Circle`/`PSet` renders correctly in an actual
  Haiku window (verified visually via screenshot, see above).

### Workflow notes

- Always sync changed files to the Haiku box and rebuild/test there before
  considering a change done — several real bugs in this port (missing
  `-L /boot/system/develop/lib`, the `crt1.o` assumption, `fb_config.h` not
  recognizing `__HAIKU__`) were only caught this way, not by local reasoning
  or by the Linux-side cross-emission dry run.
- A quick way to sync: `tar` up the relevant subtree, `scp` it over, extract
  over the existing `/boot/home/fbc-port/build` checkout there (that's the
  working directory used during the initial port — reuse it rather than
  re-cloning). After changing anything under `src/compiler/`, do a full
  self-hosted rebuild (`rm -rf src/compiler/obj && make compiler
  FBC="bin/fbc -i inc"`) before trusting the result, since stale `.bas`
  sources vs. freshly-generated `.c` bootstrap output have bitten this port
  once already.
- Regression-check the native Linux build after every change (`make
  compiler`, `make rtlib` with the system fbc) — the Haiku-specific edits
  all extend existing filter lists/case statements, so a Linux regression
  would mean an existing OS's behavior accidentally changed.
- **Multi-file `scp` (`scp file1 file2 user@host:dir/`) is not reliable to
  this box** — it silently dropped one of the two files, no error, more than
  once during this port, each time producing the exact same misleading
  false-positive: the self-hosted rebuild "succeeds" because the *linker*
  used for that build is the already-fixed *prior* binary (which doesn't
  need the new source to link correctly), so the staleness only shows up
  later when the resulting (regressed) compiler is used to build something
  that actually exercises the missing change. **Always `tar` files together
  and `scp`/extract the single archive instead of passing multiple paths to
  `scp` directly** — this was 100% reliable every time it was used in this
  port. Whichever method you use, `md5sum` (or `grep` for a distinctive
  string) the file **on the box** after syncing, before trusting any build
  that follows — this bit the port three separate times (once for a
  `makefile`+`fbc.bas` pair, twice more for `fbc.bas` alone) before the habit
  stuck. When changing `src/compiler/*.bas`, sync the **whole
  `src/compiler/` directory** (minus generated `.c`), not just the one file
  you think you changed — cheap insurance against the same class of bug.
- To visually verify gfxlib2 (or anything else that needs to actually be
  *seen*): launch the program fully detached over SSH (`nohup ./prog >
  out.log 2>&1 < /dev/null & disown` — a bare `&` without disown/nohup can
  hang the whole SSH command), then in a **separate** SSH call run
  `screenshot -s -d 1 /path/out.png` (the `-s`/`--silent` flag is required
  for non-interactive capture — without it, Haiku's Screenshot app opens its
  own GUI and blocks), then `scp` the PNG back and open it. Clean up stray
  background processes between attempts (`ps` on Haiku, then `kill -9
  <pid>`; there's no `pkill`).
- **A correct-looking screenshot is not proof a gfxlib2 program works
  correctly end-to-end** — the exit-deadlock bug above produced a window
  that rendered perfectly right up until the process hung forever on exit,
  invisible in any single screenshot. After a screenshot confirms the
  visuals, also check (via a separate `ps` call, after the program's
  expected runtime has elapsed) that the process actually terminated.
