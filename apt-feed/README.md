# Translated apt feed

**Historical proposal for deb-native 0.2.0-prealpha.** The proposed feed would
translate Debian `arm64` packages before publication for a fixed Termux
prefix. Applicability to the current deb-native runtime is unknown. This
scaffold contains seed package lists and placeholders for the builder and
package-specific fixes.

Package translation is a level-1 method in FOA. Its
[method description](https://github.com/jronminh/fhs-on-android/blob/main/methods/patch-package.md)
and [translation recipe](https://github.com/jronminh/fhs-on-android/blob/main/recipes/translate-debs.md)
cover the portable mechanism. This proposal adds a signed distribution step.

## Proposed build

1. Fetch the historical seed lists in [packages/](packages/) and their
   dependency closure from a Debian mirror.
2. Translate ELF interpreter paths, `RUNPATH`, and maintainer-script
   shebangs for the chosen prefix. Apply package-specific fixes from
   [custom/](custom/) and provide stand-ins where the prefix already
   supplies a dependency such as glibc.
3. Repack before indexing so apt verifies the bytes it installs. Generate
   `Packages` and `Release` metadata and sign the feed. Keep signing
   material outside the source tree.

The proposal considered relabelling both `all` and `arm64` packages to
Termux dpkg's `aarch64` label. Prefix layout, architecture policy, hosting
and fallback for packages outside the feed remained open. The runtime
would still provide launch and path handling on the device.

[builder/](builder/) holds the builder placeholder. The [package lifecycle
note](../notes/translated-package-lifecycle.md) records open questions about
upgrades and purge.

The scaffold is GPL-3.0-or-later. See [LICENSE](LICENSE). Packages served
from any eventual feed retain their own licenses.
