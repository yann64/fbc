# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project purpose

This repository is a port of **fbc**, the FreeBASIC compiler
(https://github.com/freebasic/fbc), to **Haiku**. Work happens on the local
`haiku` branch (off `master`, which stays an unmodified upstream mirror).

**Current status: a working, self-hosted, console-mode fbc exists and runs
natively on real Haiku hardware.** All 4 language-dialect log-test suites
(`fb`/`fblite`/`qb`/`deprecated`) pass with zero failures. See "Port status"
below for exactly what's done, what's known-missing, and where.

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
- **`src/gfxlib2/`** — the graphics library (`libfbgfx`). **Not ported to
  Haiku, deliberately, and not currently planned**: the only Unix-family
  gfxlib2 backend is X11-based, and Haiku has no X11 server by default. A
  native driver would need to target Haiku's BeAPI (`BWindow`/
  `BDirectWindow`) from scratch — no existing fbc gfxlib2 backend is a
  usable template. `SCREEN`/graphics BAS programs correctly get a clean
  "gfxlib has not been ported to this target" compile error on Haiku
  (mirrors how Android is handled) rather than a broken build.

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
- `make` / `make all` — build compiler + rtlib + gfxlib2 (do **not** use
  plain `make`/`make install` on Haiku — it pulls in gfxlib2, which isn't
  ported; use `make compiler rtlib` / `make install-compiler install-includes
  install-rtlib` instead)
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

On Haiku specifically: `unit-tests` links **every** test `.bas` file into one
binary, including the handful of `tests/gfx/*.bas` files — since gfxlib2
isn't ported, that final link always fails with `cannot find -lfbgfxmt`. This
is expected, not a bug; all ~470 test files still compile cleanly first. The
`log-tests` suite doesn't have this problem and is the primary regression
signal — it passes 100% across all 4 dialects on Haiku.

## Working on the Haiku port

### Port status (what's done)

- **makefile**: `haiku` OS detection (both `TARGET=` triplet and `uname`
  paths) and all recurring Unix-family filter lists updated. Haiku
  `ALLCFLAGS` block sets `-DDISABLE_X11 -DDISABLE_NCURSES` (Haiku has no X11
  server and doesn't ship ncurses/terminfo; `-DDISABLE_FFI` was tried
  initially but **removed** — libffi.so + ffi.h are present on stock Haiku,
  so ThreadCall works).
- **`src/compiler/fb.bi`/`fb.bas`/`fbc.bas`**: `FB_COMPTARGET_HAIKU` added
  throughout. The important, Haiku-specific (not just "add to the Unix-family
  case list") logic lives in `hLinkFiles()` in `fbc.bas`:
  - **Haiku links every binary, including plain executables, as
    `-shared -no-undefined` ET_DYN** — confirmed via `gcc -v` on real
    hardware. There's a dedicated `case FB_COMPTARGET_HAIKU` for this (not
    shared with the Linux/BSD case), and `hTargetNeedsPIC()` forces PIC
    unconditionally for Haiku regardless of CPU family.
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
  `_mbstate_t.h`). `inc/crt/stdio.bi` still falls back to the FreeBSD
  binding under the existing generic `__FB_UNIX__` warning path — untouched,
  works well enough (FILE* is used opaquely).
- **Packaging**: `contrib/haiku/fbc-1.20.0.recipe`, a haikuporter recipe.
  `BUILD()` runs `make compiler rtlib` (never plain `make`/`gfxlib2`).
  `INSTALL()`'s file layout (`make install-compiler install-includes
  install-rtlib prefix=$prefix`) was verified end-to-end on the real box: a
  `hello.bas` compiled and ran using only the staged install tree, no
  build-tree paths. **Still needs**: a real `SOURCE_URI`/`CHECKSUM_SHA256`
  once this fork/branch has a fetchable tagged tarball — haikuporter can't
  build from an uncommitted local tree.

### Known gaps / deliberately out of scope

- **gfxlib2 / `SCREEN` graphics** — not ported, not currently planned (see
  architecture section above). Correctly produces a compile error rather
  than broken output.
- **ncurses/terminfo console features** — disabled (`DISABLE_NCURSES`).
  Haiku doesn't ship ncurses by default (HaikuPorts has `ncurses6` available
  if this is revisited); rtlib's `hinit_enable_vt100_escapes.c` fallback
  path is what's actually active.
- **`inc/crt/stdio.bi`** on Haiku uses the FreeBSD `FILE` struct layout via
  the pre-existing generic-Unix fallback (with a build-time warning) rather
  than a real Haiku-verified one. Fine for opaque `FILE*` passthrough (the
  overwhelmingly common case); would matter only for code that directly
  dereferences `FILE` internals.
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
