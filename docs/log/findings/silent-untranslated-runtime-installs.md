# Findings: apt installs after bootstrap were silently untranslated (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** A runtime `apt install` (htop) produced a binary that
pointed at the real `/usr`/loader and crashed, while bootstrap-installed
packages worked. The whole "install and run" premise degraded silently for
anything installed after the base.

## Contents

- [Symptom](#symptom)
- [Root cause](#root-cause)
- [Why it stayed invisible](#why-it-stayed-invisible)
- [Fix](#fix)
- [Still open](#still-open)

## Symptom

`htop` installed via the prefix's apt could not run: its ELF interpreter was
still `/lib/ld-linux-aarch64.so.1` (absent on Android), so its launcher fell
through to `dn-run` -> `dn-trace`, which killed it (`terminated with signal
7`, SIGBUS). `bash`, `ls`, `apt`, `dpkg` -- installed at **bootstrap** -- were
correctly repointed to the prefix's fused loader. `patchelf` (also a runtime
install) was untranslated too, and `dn-run` failed on it outright
(`execv: No such file or directory`).

## Root cause

The translation step rewrites an ELF's interpreter only when it can read it:

```sh
interp=$(patchelf --print-interpreter "$f" 2>&1) || interp=""
case "$interp" in
  */ld-linux-aarch64.so.1) [ "$interp" = "$LD" ] || patchelf --set-interpreter "$LD" "$f" ;;
esac
```

`|| interp=""` swallows any `patchelf` failure, so if `patchelf` is missing or
unrunnable the `case` never matches, the ELF is repacked untouched, and the
script still exits 0 -- the package installs "successfully" untranslated.

And `patchelf` **was** unrunnable inside the prefix: it is a glibc binary that
must itself be translated, but it is installed by the same batch it is meant to
help translate. Bootstrap got away with it because `dn-translate-deb.sh` runs
in Termux's environment there and uses the **host** patchelf -- which nothing
ever checked was present. Once the base is installed, the prefix's PATH is
prefix-only, `patchelf` resolves to the (untranslated) prefix copy, and every
runtime install silently no-ops. The chicken-and-egg is never closed.

## Why it stayed invisible

Two swallows agree: `dn-translate-deb.sh` returns 0 without translating, and
the apt post-hook ends `exit 0` ("never fails the apt run"). So dpkg reports
`ii`, the install looks clean, and the failure only shows when the program is
run.

## Fix

- `scripts/install/dn-translate-deb.sh`: a preflight that aborts (non-zero) if
  `patchelf` is absent or does not run, before the loop. A missing translator
  now fails the bootstrap or the apt hook loudly instead of shipping broken
  binaries.
- `scripts/bootstrap/setup-apt-prefix.sh`: require a working host `patchelf`
  up front (`pkg install patchelf`), so the base's own patchelf is translated
  and the runtime path has a runnable translator.

Verified on the live prefix: after making `patchelf` runnable,
`apt-get install --reinstall htop` repointed htop to the fused loader and
`htop` runs.

## Still open

- `dn-run.c` and the shim's `do_exec` still have no branch for a glibc binary
  whose interpreter does not exist -- they exec it anyway (ENOENT) or hand it
  to the tracer (SIGBUS). A launch-time fallback to the prefix loader would
  make such a binary run even if a translation was ever skipped again
  (defense in depth).
- The apt hook path baked into `$DN/etc/apt.conf` is an absolute path into the
  checkout (`setup-apt-prefix.sh`); if the checkout moves, runtime translation
  silently stops. Regenerating it on refresh -- or shipping the hooks inside
  the prefix -- is deferred.
