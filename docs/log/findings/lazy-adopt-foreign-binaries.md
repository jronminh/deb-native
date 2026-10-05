# Findings: lazy adopt of foreign binaries at launch (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** Closes the non-dpkg gap from
[`non-dpkg-install-paths.md`](non-dpkg-install-paths.md): a glibc binary
installed outside apt (a vendor installer, a tarball) is now **adopted on
first launch** instead of failing, with no session-wide tracing.

## Contents

- [Trigger](#trigger)
- [Action](#action)
- [The device-level trap](#the-device-level-trap)
- [Verified](#verified)

## Trigger

The shim's `do_exec` (`core/native/path-redirect.c`) already intercepts every
`execve` a prefix process makes. It now also reads the target's `PT_INTERP`
and, when that loader **does not exist on the device** (`/lib/ld-linux-aarch64.so.1`),
hands the exec to `dn-run` instead of letting the kernel fail with ENOENT.
That is the "foreign package" trigger: any untranslated glibc binary run from a
shimmed process -- the prefix shell, any program -- is caught, no launcher and
no global tracer needed.

## Action

`dn-run` (`core/native/dn-run.c`) calls `try_adopt`: run the prefix's own
`patchelf --set-interpreter .../ld-linux-aarch64.so.1 <binary>`, so the kernel
can start it and `/proc/self/exe` stays the program (Bun/Node SEA safe). If the
rewrite succeeds, the binary is exec'd natively; on the next run its interpreter
is already the fused loader, so the trigger is skipped. If adoption fails (no
patchelf, read-only, a layout patchelf refuses), it falls back to the tracer
route, per binary. Adopting a compiled binary once is cheaper than tracing it
every run, and confined to non-apt software.

## The device-level trap

Inside the shim, a bare `access(interp, X_OK)` does **not** answer "does this
loader exist": `access` is one of the shim's own interposed functions, so the
call rewrites `/lib` to `$DN/lib` and a missing loader looks present -- the
trigger never fired. `do_exec` now uses `real_access_ok()`
(`dlsym(RTLD_NEXT, "access")`) for the check. (`dn-run` is a separate binary,
so its `access` is untouched, which is why running it directly worked while the
shim path did not.)

## Verified

A copy of `fastfetch` with its interpreter reset to `/lib/ld-linux-aarch64.so.1`,
run from the prefix shell, produced:

```
dn-run: adopted /data/.../ff-foreign (interpreter /lib/ld-linux-aarch64.so.1 -> prefix loader)
fastfetch 2.40.4-debug (aarch64)
```

and its interpreter afterwards is the prefix's fused loader.
