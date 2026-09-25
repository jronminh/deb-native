# Findings: path-redirect shim performance (2026-09-25)

Goal: make the `LD_PRELOAD` shim not a measurable drag on installed
programs. `rewrite()` sits on the hot path of every intercepted
`open`/`stat`/`exec`/`mkdir`/... call, so it is the only place worth
optimizing.

## What changed

Before, every intercepted call did:

```
getenv("DN_INSTDIR")            # linear scan of the environment
for each of /usr /etc /var /opt: strlen(prefix); strncmp(path, prefix, len)
snprintf(buf, bufsz, "%s%s", root, path)   # on a match
getenv("DN_REDIRECT_DEBUG")     # on a match
```

Now (`native/path-redirect.c`):

- **Cached at load.** An `__attribute__((constructor))` reads
  `DN_INSTDIR`, its length, `DN_REDIRECT_DEBUG` and `DN_BIONIC_PRELOAD`
  once. The environment is fixed before `exec` by the launchers, so
  caching is safe; no `getenv()` runs per call.
- **Branch dispatch on `path[1]`.** `/usr`, `/etc`, `/var`, `/opt` are
  all four bytes long, so a `switch (path[1])` picks the candidate prefix
  in one compare; a path that is not one of the four (the common case:
  `$INSTDIR/...`, relative paths, `/dev`, `/proc`, `/system`) returns
  after that single compare instead of four `strlen`+`strncmp` rounds.
- **`memcpy` instead of `snprintf`** to build the rewritten path.
- `scripts/build-path-redirect.sh` now compiles with **`-O2`**; clang's
  default is `-O0`, so the shim was previously built unoptimized.

## What the numbers actually say

On-device microbenchmarks are dominated by things that are not the shim:
Android `stat` on a missing path and Termux `fork`/`exec` cost tens of
microseconds to milliseconds, while `rewrite()`'s contribution is tens to
hundreds of nanoseconds. Repeated runs of the same tight loop
(`for …; do [ -e /usr/share/x ]; done`) disagreed with each other by
large margins (an early interleaved run showed the old shim ~35 % slower
than no shim and the new one ~3–6 % better; a later run was within noise;
a 2 M-iteration run took 37 s with no shim at all, i.e. the loop, not the
shim, is the cost).

So no stable end-to-end percentage is claimed. What is solid and does not
depend on measurement:

- the fast-reject path is now one compare, where it used to be four
  `strlen`+`strncmp` rounds;
- a rewrite no longer calls `getenv`/`strlen`/`snprintf` per call;
- real programs mostly open `$INSTDIR/...` (fast reject) or relative
  paths (no rewrite); the shim only does extra work for a literal
  `/usr`, `/etc`, `/var`, `/opt` path, which is its whole purpose.

## Separately: `grun --configure` is free at run time

`grun --configure` rewrites a `.deb`'s ELF `PT_INTERP` to
`$PREFIX/glibc/lib/ld-linux-aarch64.so.1` and adds a `RUNPATH` of
`$PREFIX/glibc/lib`. Both are one-time static edits — no wrapper, no
`ptrace`, no trampoline, no emulation, so the patch adds nothing at run
time. The visible cost of running an installed program is Termux's
`fork`/`exec` latency, not the patch or the shim.
