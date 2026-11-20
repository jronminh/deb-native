# Research notes

These edited notes preserve measurements and design questions that supplement
FOA and the implementation docs. The source snapshot is listed in
[PROVENANCE.md](../PROVENANCE.md). The remaining material stays in a private
archive.

| Note | Evidence or design question |
| --- | --- |
| [Android 16 syscall sweep](android-seccomp-syscall-map.md) | Per-number results, shell control and a discrepancy with the AOSP allowlist model |
| [Foreign libc in the app domain](android-vs-linux-foreign-libc.md) | Startup outcomes and coverage of an early shim/tracer combination |
| [glibc DNS on Android](glibc-dns.md) | A device-specific resolution result without a guest resolver configuration |
| [Translated package lifecycle](translated-package-lifecycle.md) | Ownership of derived files on upgrade, removal and purge |
| [Early shell-domain experiment](shell-domain-experiment.md) | FIFO refusal, service lifetime, executable storage and package-hook tool selection |
