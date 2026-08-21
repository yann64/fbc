# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project purpose

This repository is a port of **fbc**, the FreeBASIC compiler
(https://github.com/freebasic/fbc), to **Haiku**. Work happens on the local
`haiku` branch (off `master`, which stays an unmodified upstream mirror).

**Current status: a working, self-hosted fbc exists and runs natively on real
Haiku hardware, with full console features, ThreadCall, and a native
graphics driver.** All 4 language-dialect log-test suites
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
  not code-reusably). Since BeAPI has no C bindings, this driver is **C++**
  (`gfx_driver_haiku.cpp`), the only `.cpp` anywhere in gfxlib2/rtlib — see
  "gfxlib2 driver" below for the build-system and design details. Scope:
  32bpp truecolor and 8bpp indexed/palette SCREEN modes, keyboard, mouse.

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
  `ALLCFLAGS` sets `-DDISABLE_X11 -DDISABLE_OPENGL` only (no X11 server, and
  the Haiku gfxlib2 driver doesn't do OpenGL). libffi and ncurses6
  (`termcap.h`/`curses.h`, `libncurses.so` → `libncursesw.so`, bundles the
  classic termcap API directly, no separate libtinfo split like on Linux)
  both work once `ncurses6_devel`/`libffi_devel` are installed — full
  console features (COLOR/LOCATE/INKEY$/WIDTH/etc.) and ThreadCall are both
  verified working. ncurses6 isn't part of the base OS install, unlike
  libffi — see the haikuporter recipe's `BUILD_PREREQUIRES`/`REQUIRES`.
  Also added `CXX`/`ALLCXXFLAGS` and parallel `LIBFBGFX_CXX` build rules (4
  variants: plain/PIC/MT/MTPIC) alongside the existing `_C`/`_S` ones — the
  only `.cpp` in the whole tree is the Haiku gfxlib2 driver (see below), so
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
- **`src/gfxlib2/haiku/`** — the native graphics driver, three files:
  `gfx_driver_haiku.cpp` (the actual `GFXDRIVER` implementation — see
  fb_gfx.h's doc comments for the interface fbc expects: gfxlib2's core does
  all software rasterization into `__fb_gfx->framebuffer`, a plain malloc'd
  buffer; the driver's whole job is get a window on screen, blit that
  buffer into it, and turn OS input events into `fb_hPostEvent()` calls),
  `gfx_haiku.c` (plain C, provides `__fb_gfx_drivers_list[]`/
  `fb_hScreenInfo()`, mirrors `gfx_unix.c`'s role), `fb_gfx_haiku.h` (the
  `extern "C"` bridge declaring the driver struct symbol for the C file).
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
- **Packaging**: `contrib/haiku/fbc-1.20.0.recipe`, a haikuporter recipe.
  `BUILD()` runs `make compiler rtlib gfxlib2`; `INSTALL()`'s file layout
  (`make install-compiler install-includes install-rtlib install-gfxlib2
  prefix=$prefix`) was verified end-to-end on the real box: a `hello.bas`
  compiled and ran using only the staged install tree, no build-tree paths.
  **Still needs**: a real `SOURCE_URI`/`CHECKSUM_SHA256` once this
  fork/branch has a fetchable tagged tarball — haikuporter can't build from
  an uncommitted local tree.

### Known gaps / deliberately out of scope

- **`SetMouse` can't reposition the system cursor** — only show/hide
  (`BApplication::ShowCursor()`/`HideCursor()`, both real) and clip-state
  tracking are implemented. Haiku's `set_mouse_position()` lives in
  `<WindowScreen.h>`, the fullscreen/game API, and wasn't confirmed safe to
  call from a plain windowed `BView`; left unimplemented rather than
  guessed at.
- No `fetch_modes`/`wait_vsync`/`set_window_pos`/OpenGL — all `NULL` in the
  `GFXDRIVER` struct; SCREEN always opens a fixed-size window at whatever
  size/position `BWindow::CenterOnScreen()` picks.
- The exact ABI flags in `fb.bas`'s `targetinfo()` row for Haiku
  (`FB_TARGETOPT_*` — struct-passing/return conventions) are still only
  inferred from the closest BSD-family target, not independently verified
  against Haiku's actual System V x86_64 ABI implementation.

### Confirmed since the initial port (no longer open questions)

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
