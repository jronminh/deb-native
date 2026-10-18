# tests/bench

The lane test, the regression set and the Lua build wall (TODO, "Speed"), moved
out of a work prefix's `tmp/lane/`.  One script prints the rows of
[`../../docs/performance.md`](../../docs/performance.md), "In an app session",
and runs the checks a runtime change must keep:

```sh
tests/bench/run.sh PREFIX bench
tests/bench/run.sh PREFIX check
tests/bench/run.sh PREFIX            # both
```

**bench** — the regression set (`reg.sh`), the file-tree set, `find`, `ls`, and
the Lua build when `LUA_DIR` names its source (a guest path;
`LUA_DIR=/home/bench/lua-5.4.7`).  Each row is the wall and the tracer's
entry/exit stops; the tracer polls 500 us as `boot.sh` does.  Interleave the runs
before and after a change: the phone's timings vary 10-20% between runs.
`reg.sh` writes `/tmp/reg-run` (host: `PREFIX/tmp/reg-run`); the digest printed
after its row is one number to compare between builds.

**check** — `fr.sh` (ids, stat, `setresuid`, vfork/fork), `mt` (threads and a
vfork child that `chdir`s and `setuid`s), `xm` (`execmod`/`execmem`, the two
checks Android 16 must pass before the shim or the loader hook ships),
`syscalls` (the Android 16 denials that have no row: each must return `ENOSYS`,
not kill the process), `owner` (`chown` kept in `user.dn.owner` and answered by
`stat`, with `-R`, reset to 0:0, and a hard-link copy), and `t.sh` (`git init`
and `apt-get clean`, the two calls the shim's opendir bug broke).  `xm` fails on
a domain that refuses `mprotect` RX of libc's text; that is a finding, not a
broken test.

The kit is copied into `PREFIX/tmp/dn-bench` on each run, so a prefix needs
`gcc` for the checks.  `reg.sh`, `fr.sh`, `lua.sh`, `t.sh` and `exec200.sh` run
under the prefix's `sh`.

The runner seeds its own `dn-tiny` runs as **root** (`--initial root`): the
checks (`chown`, `setresuid`) and the rows in `docs/performance.md` are root's.
Set `DN_INITIAL=user` to measure the real-user session instead; a prefix's
`boot.sh` seeds the real user by default (see `docs/spec/identity.md`).
