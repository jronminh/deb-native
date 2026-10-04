# The interpreter trampoline (`ld-dn`): execution phases

> Template: [`templates/docs.template.md`](../../templates/docs.template.md)
> (fix the relative path to match this file's depth). Read a doc's
> summary and table of contents below before its sections, and read its
> directory's own `README.md` first to confirm this is the right doc to
> open. Create a new doc, instead of extending an existing one, when the
> content is a distinct kind of writing -- a new spec topic, a new one-off
> investigation, or a new guide -- not just a long addition to what a doc
> already covers.

The runtime companion to [`ld-dn-config.md`](ld-dn-config.md): that doc
covers the *policy data*; this one covers what `native/ld-dn.c` (the
interpreter trampoline, slated for rename to `dn-interp`, `TODO.md`
"Runtime overhaul") actually *does*, as a phase model with line anchors --
for reviewing or re-reading the file, and for future reference when the
rename lands. Its static counterpart (how `PT_INTERP` is rewritten to point
here) is [`elf-interp-patch.md`](../reference/elf-interp-patch.md).

Living doc: tracks the shipped code, not a proposal.

## Contents

- [What it is](#what-it-is)
- [The phases](#the-phases)
- [Prepare vs commit](#prepare-vs-commit)
- [Why the loader's own base matters](#why-the-loaders-own-base-matters)
- [How to read the code](#how-to-read-the-code)
- [Limits and invariants](#limits-and-invariants)
- [Alternative: fuse into the loader](#alternative-fuse-into-the-loader)

## Related docs

- [`ld-dn-config.md`](ld-dn-config.md) -- the policy/config file this
  loader reads; the data side of the same component.
- [`elf-interp-patch.md`](../reference/elf-interp-patch.md) -- the static
  `PT_INTERP` patch that makes the kernel pick this file in the first
  place; the other half of the mechanism.
- [`path-shim.md`](../spec/shim/path-shim.md) -- the `LD_PRELOAD` shim this loader
  injects into the environment it builds.
- [`design.md`](../spec/design.md) -- scope and the three path mechanisms; this
  doc is the deepest look at the runtime one.
- [`dn-glibc-prefix.md`](../spec/dn-glibc-prefix.md) -- the next-gen prefix that
  replaces this trampoline with glibc's own prefix-built loader.
- `native/README.md` -- the other `native/` C programs.

## What it is

A freestanding (no libc, raw `svc` syscalls) static-PIE aarch64
executable. Every translated Debian ELF names it as `PT_INTERP`, so the
kernel runs it *first*, as the program's interpreter, however the program
was started. It does not re-exec and it does not link anything: it
transforms the current process in place, then jumps into glibc's real
dynamic loader with a stack it built itself. The header comment
(`native/ld-dn.c:1-33`) states this whole design in prose.

## The phases

The phases below are execution stages, not the file's layout. Line numbers
are for `native/ld-dn.c` as of the commit that added this doc.

**Fidelity is an assumption, not a proven fact.** The phase walkthrough
below describes `ld-dn` as reproducing the kernel's interpreter handoff
exactly. That is the design's central bet, and only parts of it have been
checked: the stack-rebuild math was sanity-checked against one large
`argv`/`envp` case (`TODO.md`, "Runtime component audit"), and the
end-to-end runs and tests exercise the common paths. Nobody has verified,
byte for byte, that the rebuilt stack/auxv and the manual loader mapping
match what the kernel produces across the full range of Debian binaries.
Whether glibc is fooled in every case is therefore the design's main
unproven risk -- and the reason this file, not the shim or the tracer, is
the one to re-audit the moment a new class of binary misbehaves.

**Phase A -- before it runs (kernel side).** The kernel `execve()`s the
original program, reads its `PT_INTERP` (the field `elf-interp-patch.md`
describes), loads *this* file as the interpreter, and transfers control to
it with the original program's initial stack. Crucially, `AT_PHDR` and
`AT_ENTRY` still describe the **original program**, not the interpreter.

**Phase B -- the entry contract** (`_start`, 596-609). Saves the
kernel-built `sp`, reserves `0x10000` bytes below it for a rebuilt stack,
calls `ld_dn_main(sp)`, and receives back two values: a new `sp` and a jump
target. Nothing has changed yet at this point.

**Phase C -- prepare** (`ld_dn_main`, 479-594), following the function's
own numbered comments:

- **C1 (479-493) parse inherited state.** Read `argc`/`argv`/`envp` from
  `sp`, walk `auxv` for `AT_PHDR`/`AT_PHNUM`/`AT_PAGESZ`.
- **C2 (495-505) derive the prefix.** From the *program's* `PT_INTERP`
  (via `AT_PHDR`), require the `/usr/lib/deb-native/ld-dn` suffix and strip
  it to get `$DN`. This is what makes the same binary prefix-agnostic; the
  `bias = phdr - PT_PHDR.p_vaddr` line turns a PIE `p_vaddr` into a runtime
  address.
- **C3 (507-523) policy.** Capture caller env, resolve `program_name()`,
  apply `policy_defaults()`, optionally read `ld-dn.conf`, then
  `build_env()` -- producing the `LD_PRELOAD` (shim), `DN_INSTDIR`,
  `LD_LIBRARY_PATH`, etc. See `ld-dn-config.md`.
- **C4 (525-545) rebuild the stack.** Lay out `[argc][argv][0][envp][0]
  [auxv]` below `sp`, dropping inherited vars per `skip_env()` and
  appending the new ones.
- **C5 (547-591) map the loader and fix auxv.** Open and validate
  `$DN/usr/lib/ld-linux-aarch64.so.1`, `mmap` each of its `PT_LOAD`
  segments (file part + anonymous zero-fill) into a reserved range, then
  copy `auxv` with **`AT_BASE` overwritten** to that mapping's base.

**Phase D -- commit** (`_start` tail, 599-609). `sp = x0` (new stack),
`x16 = x1` (loader entry), `x0 = 0`, `br x16`. These four instructions are
what the kernel itself does when starting an interpreter; `x0 = 0`
reproduces the register state `rtld` expects on entry.

**After D.** glibc's loader runs with the program as the main executable
and the shim preloaded, believing the kernel invoked it normally.
`/proc/self/exe` still points at the original program -- the no-re-exec
trick.

## Prepare vs commit

The "magic" is not one line. Split it:

- **Prepared** in Phase C, above all in **C5**: mapping glibc's real loader
  and rewriting `AT_BASE` is what constructs the kernel-shaped illusion.
- **Committed** in Phase D: the `br x16` into the loader is the
  irreversible moment, in the same process, with no `execve()`.

The single most load-bearing line for the illusion is the `AT_BASE`
overwrite (589-590); the single most load-bearing line for the no-re-exec
property is `br x16` (609).

## Why the loader's own base matters

A dynamic loader normally learns its own load address from `AT_BASE` (the
kernel sets it to where it loaded the interpreter). Because the kernel here
loaded `ld-dn` as the interpreter, the inherited `AT_BASE` points at
`ld-dn`, not at glibc's loader. If `ld-dn` handed that value through,
`rtld` would believe it lives at `ld-dn`'s mapping and miscompute its own
relocations and data addresses. C5 therefore recomputes and overwrites
`AT_BASE` with the base it just mapped the real loader at, so `rtld` sees
the value it would have seen had the kernel loaded it. Every other `auxv`
entry is copied through unchanged (`AT_PHDR`/`AT_ENTRY` must keep
describing the program).

## How to read the code

Read in this order, not top-to-bottom:

1. The header comment (1-33) -- the design in prose.
2. `_start` (596-609) -- the input/output contract, *before* `main`.
3. `ld_dn_main` (479-594) following its own 1-4 comment landmarks.
4. Policy as data: `policy_defaults()` (262-274), `build_env()` (418-461),
   `load_config()` -> `parse_config()` -> `dispatch()` (378-394,
   346-364, 315-341), `skip_env()` (251-260).
5. Re-read slowly the two fiddly spots: the stack rebuild (525-545) and
   the loader mapping (547-585, especially `base = area - lo`).

Skim on a first pass: the freestanding string helpers (108-188), the
config-parser edge cases (276-364), and `dump_policy()` (462-475).

Make it concrete: `tests/ld-dn-config/` exercises the config/override path;
`DN_REDIRECT_DEBUG=1` dumps the resolved policy at runtime (line 523).

## Limits and invariants

- **Prefix-agnostic by construction**: no hardcoded prefix; it is derived
  from the program's `PT_INTERP` suffix (C2). A file not installed as
  `$DN/usr/lib/deb-native/ld-dn` makes it `die()` (504).
- **Freestanding and bounded**: no allocation, fixed caps (`MAX_LIB`,
  `MAX_PRE`, `MAX_ENV`, `MAX_UNSET`), and fail-open on config overrun --
  it warns and keeps what fits, because it runs for *every* exec.
- **Stack budget**: the rebuilt vector must fit the `0x10000` reserve
  (531); it `die()`s rather than overflow.
- **Not the whole story**: it shapes only process startup (env, loader
  base). Runtime path calls are the shim's job; syscall-level paths are the
  tracer's; static and Bionic binaries never reach this file at all.

## Alternative: fuse into the loader

**Chosen**, being built out in [`dn-glibc-prefix.md`](../spec/dn-glibc-prefix.md)
(`TODO.md`, "Runtime overhaul"). Instead of a separate `ld-dn` the kernel
runs first, point `PT_INTERP` straight at glibc's own loader, built for the
prefix, which supplies the shim (`ld.so.preload`) and the library path
(`ld.so.cache`). Then the kernel does the loading again and `ld-dn`
disappears. The sketch and pros/cons below are kept as the record that led
to the decision; `dn-glibc-prefix.md` is the live design.

**Plan sketch.**

1. Build on the project's own-glibc fork (`third_party/glibc-android-patches/`,
   `scripts/bootstrap/dn-package-glibc.sh`); the loader already has to be the
   Android-seccomp-patched one, so this rides on that work.
2. Patch the loader's early init (`elf/rtld.c`, and the search-path/preload
   setup around `elf/dl-load.c`) to, before it maps any dependency:
   - derive the prefix the same way `ld-dn` does (C2): read the executed
     program's `PT_INTERP` and strip the `/usr/lib/.../ld-<name>` suffix;
   - append the prefix's `path-redirect.so` to its preload list and the
     prefix's library dirs to its search path, internally rather than via
     `LD_PRELOAD`/`LD_LIBRARY_PATH`.
3. `dn-translate-deb.sh` rewrites `PT_INTERP` to the patched loader instead
   of `ld-dn`.
4. Optionally keep the policy-as-data flexibility by having the loader read
   `$DN/etc/deb-native/ld-dn.conf` once the prefix is known.
5. Retire `native/ld-dn.c`; point `setup-runtime.sh` and the tests at the
   patched loader.

**Pros.**

- Deletes the highest-risk code in the tree: no stack rebuild, no manual
  `mmap` of `PT_LOAD`s, no `AT_BASE` surgery, no freestanding ELF.
- The kernel goes back to doing the load, so there is far less to get
  wrong or audit; `/proc/self/exe` stays the program with no special effort.
- Keeps the properties that matter: the loader only ever runs for glibc
  programs (so the shim stays glibc-only), and no env is needed (so
  env-clearing execs and direct-path launches still get the shim).
- One shipped artifact (the loader) instead of two; simpler mental model.

**Cons.**

- Version-coupled: a glibc patch to rebase on every glibc bump, in
  delicate, fast-moving internals (`rtld.c`/`dl-load.c`) -- versus a
  self-contained, version-independent 600-line binary audited once.
- The prefix-derivation logic is not removed, only relocated into the fork.
- The shim stays: the loader can only *deliver* it (preload), not replace
  the runtime path rewrites.
- Ties the runtime to the own-glibc effort; not available until that is
  solid, and the Debian-glibc stand-in path needs its own story.
- A bug now lives in the loader every program uses, and glibc internals are
  harder to instrument and test in isolation than a separate binary.

**Trigger to revisit**: once the own-glibc build (0.5.0) is the default and
stable, weigh this against the trampoline; until then the trampoline is the
lower-coupling choice.
