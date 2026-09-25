# Direction 2: static per-binary wrappers (replaces the "view")

Status: **design, not implemented.**

## Problem this replaces

sudo-less's "view" (`docs/view.md` in sudo-less) makes a package's
hardcoded absolute paths (`/etc/foo.conf`, `/usr/share/foo/templates`)
resolve correctly at run time by overlaying the prefix onto the real `/usr
/etc /var /opt` inside a private mount namespace, live, for the duration of
the call. That needs `unshare(CLONE_NEWUSER)` + unprivileged overlayfs,
both assumed blocked under Termux's SELinux domain (see
[`prior-art.md`](prior-art.md)).

## Approach

Do the same job **ahead of time, per binary, with no namespace**: at
install time, for each program `prefix-wrap`'s heuristics flag as needing
path help, generate a fixed script (or patch the binary directly) that
resolves its paths against the prefix explicitly, instead of relying on
`/etc/foo.conf` transparently meaning the prefix's copy.

Concretely, by failure mode (same table as sudo-less's `view.md`):

| why the binary needs help | static fix |
|---|---|
| interpreter shebang not on host (`#!/usr/bin/ruby`, host only has Termux's `ruby`) | rewrite the shebang at install time to the actual interpreter path in the prefix, or wrap with `exec $PREFIX/usr/bin/ruby "$0" "$@"` |
| interpreter only searches compiled-in module paths (Python/Perl/Node/...) | wrapper sets the interpreter's own search-path env var (`PYTHONPATH`, `PERL5LIB`, `NODE_PATH`, ...) to the prefix's copy before `exec`ing — this is exactly the case sudo-less's own docs call out as **not** solvable by env vars *for the view's other cases*, but it's the right tool specifically for module search paths |
| `ldd` can't find a library the package ships | wrapper sets `LD_LIBRARY_PATH` to the prefix's lib dir before `exec` — same caveat as above: fine for this one case, not a general substitute for the view |
| ELF binary is a glibc build, needs glibc-runner | wrapper (or the binary's patched ELF interpreter directly, per the existing manual glibc-runner method) invokes it against the glibc side-install |
| binary/script has a hardcoded absolute path to its own data (`/usr/share/figlet`, `/etc/redis/redis.conf`) it reads directly, not through a library call the above env vars cover | **solved** — see [`design-manual-overlay.md`](design-manual-overlay.md): an `LD_PRELOAD` shim (`native/path-redirect.c`) intercepts `open`/`openat`/`fopen`/`stat`/`fstatat` and rewrites the path, verified against `figlet`'s real `/usr/share/figlet` lookup |

## What this used to not solve (now closed)

The view's whole point is Debian packages assume `/` is real. A static
wrapper only helps for the *specific, enumerable* ways a program looks
things up (interpreter search paths, dynamic linker search paths,
shebangs). A binary that does `open("/etc/foo.conf")` directly in its own
C code, with no env var and no CLI flag to redirect it, used to have no
static fix short of binary-patching the literal path string (only works if
the replacement is the same length or shorter) — **until
`design-manual-overlay.md`'s `LD_PRELOAD` shim, now built and verified**.
Binary-patching the string remains the fallback for a statically-linked
binary (no dynamic libc calls to intercept), which the shim genuinely
cannot reach.

Per sudo-less's own survey (`survey-2026-09.md`, referenced from
`view.md`): ~73% of packages need **no** path help at all and just run from
the prefix's `bin/` directly. Of the remaining ~27%, an unmeasured fraction
falls into the "hardcoded path, no env var" bucket this direction can't
reach. **Needs its own survey against real glibc arm64 `.deb`s before
claiming a coverage number** — do not assume sudo-less's 73%/27% split
transfers; it was measured on Debian's package set with Debian's own
`/usr` layout assumptions, not against Termux's prefix.

See [`design-hooks.md`](design-hooks.md) for how this detection/generation
step gets triggered automatically after an install (apt's
`DPkg::Post-Invoke` plus a `dpkg` wrapper script, not a patch).

## Open work

- [ ] Run `prefix-wrap`'s detection heuristic (or a reimplementation of it)
      against a real sample of glibc arm64 `.deb`s to get an actual
      coverage number for "wrapper suffices" vs. "needs a path-virtualization
      layer neither direction handles yet".
- [x] ~~Decide whether the `LD_PRELOAD` shim is worth building~~ — built,
      see `design-manual-overlay.md`.
- [ ] Wire the shim's env vars (`TDB_REDIRECT_FROM`/`_TO`) into the
      wrapper-script generation this doc describes, instead of setting
      them by hand as done for the `figlet` test.
- [ ] Decide whether the remainder (statically-linked binaries, unreachable
      by any dynamic-call interception) is small enough to just exclude
      (same as sudo-less excludes root-only-maintainer-script packages).
- [ ] Reuse or reimplement `prefix-wrap`'s wrapper-script generation
      (`$PREFIX/bin/<name>` script recorded per-package so it's removed
      with the package) — this part has no namespace dependency and can
      likely be ported near-verbatim.
