# The plan: the kernel in the prefix, then disguised as arm64 Linux

The roadmap the frame in [`mapped-kernel.md`](mapped-kernel.md) orders. Two
phases: **first map the Android kernel into the tree** (every coordinate the
program can observe, by one law), **then disguise it as a stock arm64 Linux
kernel** (the values the kernel fixes and a mapping cannot carry). Status:
proposal.

## Where we are (the honest baseline)

- The mapping is **partial**: path in/out only, and the loader's own opens and
  the `LD_AUDIT` hook are still open (see below), but the law is one and the
  loader goes through it.
- The speed engines (the filter's tree, the shared cookie block, the site
  patches, the `LD_AUDIT` loader hook, the cancellable `svc`) work, but serve a
  mapping that is not yet complete.
- A few fakes exist (the id store, netlink, `execveat`) -- phase-2 residue, done
  early because the kernel refused them.
- Numbers to keep: the Lua build is 1662 stops; the bench is `scripts/bench.sh`.

## Phase 0 -- the foundation (one law, no seam)

The mapping must be *single* and *consistent* before it is widened.

1. **One law.** Done: the shim builds `policy.c` (`-DDN_LIBC`) and calls its law
   (`policy_path_nf`, `policy_path_at`, `policy_unpath`, `policy_resolve`,
   `policy_is_host`); its own resolver is gone, and the link cache (now
   `policy.c`'s) is a seqlock the shim's threads share. Two copies can disagree,
   and the 0.7 runtime died of that.
2. **Loader and gate uniform.** Done: the exec gate runs the program's own
   `PT_INTERP`, resolved in the tree and mapped to a host path (the kernel needs
   a file to exec), with `--argv0` and the shim/loader hook preloaded by their
   host paths (the runtime's own; the shim finds the root from it).  There is no
   `--library-path`: the loader finds the libraries through the tree's
   `ld.so.cache`, their paths the tree's, so the law maps them like any other.
   The `LD_AUDIT` hook maps the loader's own opens through that same law (E7).
3. **Binds are data.** Done for the table: one table in `policy.c` (the prefix
   root + the host rows: `/proc`, `/sys`, `/dev`, the shared dirs + the binds in
   `boot/binds`), read by every engine -- not re-derived in each; the walk
   resolves in guest space and maps through it; the mount point, `EXDEV` between
   rows, the `/proc` mount view (`mounts`, `mountinfo`, `/etc/mtab`) from the
   table and `ro` -> `EROFS` are all in. The step is done.

## Phase 1 -- complete the mapping (the coordinates)

Map every surface a program reads a coordinate from, one by one, and keep a
**checklist** so a hole is visible ([`coverage.md`](coverage.md)). The surfaces:

- **Path in** -- the rows the tracer already classifies (`gen_rows.py`).
- **Path out** -- `getcwd`, `readlink`, and every place the kernel *names* a
  path: `/proc/*/cwd`, `/proc/*/exe`, `/proc/*/fd/*`, `/proc/*/maps`,
  `/proc/*/mounts`, `/proc/*/mountinfo`, `statx`'s name, a directory listing.
- **fds** -- the target a descriptor names.
- **AF_UNIX paths** -- `bind`/`connect`/`sendto`/`sendmsg` and their readbacks
  (tmux, ssh-agent, gpg-agent, dbus). **Done**: the rows carry the `sockaddr_un`
  in (`SP_SOCK_IN`, mapped at entry) and back (`SP_SOCK_OUT`, at exit).
- **The mount view** -- the binds and the `/proc` mount files, generated from
  the table.
- **`/sys`, `/dev`, `/storage`** -- bound or synthesized.

**The coverage test:** a program that walks each surface and prints what it
sees; the expected output is the tree's view. It fails when a reader leaks the
host frame. This is the phase's proof.

## Phase 2 -- the disguise (arm64 Linux)

The values the kernel *fixes* and a mapping cannot carry.  The revised rule
([`phase2.md`](phase2.md)): **the kernel and the hardware are the truth and are
shown as they are** -- the phone is a real Linux on real arm64, so `uname`,
`/proc/version` and `/proc/cpuinfo` are not faked.  Only what Android *forces* is
changed -- the runtime's own traces and the files the app domain denies -- and
each such answer carries the Debian release the prefix reproduces:

- **`/proc/<pid>/status`** (done): `TracerPid`, `Seccomp`, `Name`, `State` -- the
  runtime's traces, not the kernel's.
- **ids** (fake root, done), **the SELinux label** (done), **netlink**.
- **the divergent class** (cgroup, rlimit, sched, capabilities, `/proc/sys/*`,
  the W^X behaviour): the real value where the tracer can read it, else a clear
  refusal -- never invented kernel numbers.

**The fingerprint test** (`scripts/fingerprint.sh`): it requires the kernel and
hardware to be the host's, the runtime's traces to be gone, and the Debian
identity to be the tree's.

## Cross-cutting

- **One model, two enforcement points** (the old spec): a model lives in one
  place and both the shim and the tracer read it; no model may be answered in
  only one of them.
- **Every reader covered:** a model is only as good as its least-covered
  reader.
- **Performance:** the engines are the mapping's fast paths; keep the bench and
  the stop counts as the measure, near-native as the target.
- **The boundary:** what the program may and may not see (isolation is *not* the
  goal -- one prefix, one boot -- but the host frame must not leak).

## Open questions (to settle before building)

- **Order within phase 1:** the checksum (answer readers first, then names)?
- **How deep the disguise:** a full `uname`/`cpuinfo` fiction, or the minimum a
  package probes? "Refuse, don't fake" sets the floor.
- **Where the law's data lives:** the prefix tree (the old `RT` store) or the
  app's files.
- **Performance budget:** how many stops a complete mapping may cost before an
  engine is required for a surface.
