# bootstrap/

<!-- template: templates/readme.template.md -->

Build the glibc bundle (B1, MODULARIZE.md "Build vs Ship") on a build host:
this project's own Android-patched glibc, packaged as the `libc6`/`libc-bin`
`.deb`s a prefix is assembled from. It never runs on a target; the target only
ships the artifact (`core/runtime/ship-prefix.sh`).

- `dn-apply-glibc-patch.sh` — apply
  [`../third_party/glibc-android-patches/`](../third_party/glibc-android-patches/README.md)
  to a glibc source tree.
- `dn-package-glibc.sh` — package this project's own-built glibc as a real
  `libc6` `.deb` (Debian's own package as the template, our payload).
- `dn-package-libc-bin.sh` — package this build's own glibc *programs*
  (`ldconfig`, `ldd`, `getconf`, `locale`, ...) as a real `libc-bin` `.deb`,
  needed because `ldconfig` is path-sensitive.
