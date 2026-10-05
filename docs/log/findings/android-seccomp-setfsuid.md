# Findings: Android seccomp kills `setfsuid` -- interactive shell fixed (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** A new Termux session entered the deb-native prefix,
printed the welcome, then the shell died with no prompt (what looked like
being "kicked"). Root-caused to an Android seccomp trap and fixed in the shim.

## Contents

- [Symptom](#symptom)
- [Diagnosis](#diagnosis)
- [Fix](#fix)
- [Login layer, deferred](#login-layer-deferred)

## Symptom

`~/.termux/shell -> ~/.dn-login`, so the app's `login` runs the motd welcome
then `exec $SHELL -l` -> `.dn-login` -> the prefix's `dn-shell -l`. In a pty
the welcome printed, then nothing: no prompt, and the process was a zombie
(`State: Z`). Running `dn-shell -l` directly under a pty reproduced it;
`dn-shell -c '...'` worked.

## Diagnosis

`strace` (Termux) showed the trailing syscall:

```
faccessat(AT_FDCWD, ".../usr/share/terminfo/x/xterm", R_OK) = 0
setfsuid(0)                                   = 0
--- SIGSYS {si_code=SYS_SECCOMP, si_syscall=__NR_setfsuid, si_arch=AUDIT_ARCH_AARCH64} ---
+++ killed by SIGSYS +++
```

So the killer is **signal 31 = SIGSYS**: Android's seccomp filter traps
`setfsuid` for untrusted apps. It is not the shim and not fake-root -- the trap
is independent of both. It is reached by **libtinfo/ncurses**, whose terminfo
lookup calls `setfsuid`; `tput colors`/`infocmp` die the same way ("Bad system
call"). Readline is built on libtinfo, so *every interactive bash* (and any
ncurses TUI) died; `--noediting` and `-c` (no readline) were unaffected.

## Fix

`core/native/path-redirect.c` now overrides `setfsuid`/`setfsgid` as silent no-ops
returning the current uid/gid. They are deliberately **not** in the
`FAKE_SETID` helper, which falls through to `real()` when fake-root is off --
the seccomp trap fires either way, so the fallthrough would still SIGSYS. A
rebuilt shim installed into the prefix makes `tput colors` (8), `infocmp`,
interactive `dn-shell -l`, and the full login path all prompt again.

## Login layer, deferred

Separately, the multi-userland selector `~/.dn-login` itself has a bug: it
execs the prefix `dn-shell -l` yet that process exits 0 with no prompt, while
the same `dn-shell -l` run directly works. Called out here, not fixed now. To
unblock the app immediately, `~/.termux/shell` points straight at the
prefix's `dn-shell` (no chooser); the multi-userland login returns later.
`make-shell-interface.sh` still generates `.dn-login`; it is simply unused.
