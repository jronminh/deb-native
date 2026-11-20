# Early shell-domain experiment

**Measured 2026-10-08** on one Android 16 user build, kernel 5.10, with
the shell UID. The experiment used an older translated prefix with patched
glibc. These observations predate the current ADB runtime. FOA covers the
[host domains](https://github.com/jronminh/fhs-on-android/blob/main/reference/domains.md) and
[recorded ADB recipe](https://github.com/jronminh/fhs-on-android/blob/main/recipes/no-root-glibc.md).

## Supervisor and session lifetime

- `runsvdir` started, but `runsv` failed on `supervise/control`. Both
  `mkfifo` and `mknod p` returned `EACCES` at all nine probed locations.
  The experiment did not identify the exact policy rule. The
  [ADB host measurements](../docs/spec/adb-host.md) show that FIFO creation
  varies across devices.
- A prototype using shell loops and no FIFO restarted a killed service in
  about a second. Killing its per-service watcher left the old worker
  running and the next scan started a second copy. Killing the top-level
  watcher left workers running without a supervisor.
- Double-forking and starting a new session changed the helper's parent to
  init while it remained in the ADB daemon's cgroup. The shell UID lacked
  write access to the cgroup membership file. This limits what process
  detachment can do for service lifetime. [FOA supervisor recipe](https://github.com/jronminh/fhs-on-android/blob/main/recipes/supervisor.md)

## Executable storage and package hooks

- The shell could write and execute from `/data/local/tmp`. It could
  write to `/data/local/traces`, but executing the probe there returned
  `Permission denied`. The shared-storage probe was also non-executable.
  [FOA storage durability](https://github.com/jronminh/fhs-on-android/blob/main/tree/durability.md)
- A translated package's pre-install hook failed with
  `xargs: Unknown option 'I{}'`. The hook was running in the guest shell
  with a system directory ahead of the prefix in `PATH`, so it picked the
  system toolbox's `xargs`. Removing that directory from the hook's
  `PATH` restored the install. The experiment exercised a small set of
  maintainer scripts.
