# Historical implementation evidence

These frozen scripts preserve the historical versions cited by FOA. Keep
the archived copies unchanged.

| Archived file | Original path | Original commit | Commit date | Evidence |
| --- | --- | --- | --- | --- |
| [dn-fix-glibc.sh](dn-fix-glibc.sh) | `core/install/dn-fix-glibc.sh` | `f0331fe64b26ab1468c64dec713fb9b09308f45e` | 2026-10-05 | Historical glibc patching evidence cited by FOA's [patched-libc method](https://github.com/jronminh/fhs-on-android/blob/main/methods/patched-libc.md). |
| [dn-translate-deb.sh](dn-translate-deb.sh) | `scripts/prefix/dn-translate-deb.sh` | `039f45b93ab4df45bc2f582354e5d1605b035765` | 2026-10-06 | The archived bytes also match the earlier `core/install/dn-translate-deb.sh` at `50eaa1ef7456faf67c0146c037543b195dc5491d` (2026-10-06), cited by FOA's [translation recipe](https://github.com/jronminh/fhs-on-android/blob/main/recipes/translate-debs.md). |

## Retired fusion launcher

On 2026-09-26, the fusion build could resolve the `figlet` alternative
after correcting double-prefixed and dangling links. Launching through
Termux's `grun` still stripped `LD_PRELOAD`, so the path shim never loaded
and `figlet` could not find a font that was installed.

The fusion fix patched glibc ELF interpreters at install time using only
files listed by `dpkg -L` for that package. A self-link at `usr` let the
existing launcher keep its nested path assumptions. The launcher set
`LD_PRELOAD` and executed the patched binary directly. The same day's
follow-up recorded `figlet` rendering through the alternative. Today's
[exec gate and `dn-exec`](../../docs/architecture.md#dn-exec-birth-time-patching-for-the-loader-and-static-programs)
start unmodified Debian programs inside the prefix.
