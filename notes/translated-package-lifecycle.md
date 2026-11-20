# Translated package lifecycle

**Design note from the package-translation work.** A translated archive
must already satisfy the assumptions that dpkg and maintainer scripts
read before and during unpack. FOA describes the original
[package assumptions](https://github.com/jronminh/fhs-on-android/blob/main/reference/problem.md#what-a-package-assumes)
and [translation procedure](https://github.com/jronminh/fhs-on-android/blob/main/recipes/translate-debs.md).

## Ownership on upgrade and removal

dpkg can remove paths recorded in its database. The prefix still owns
any state the translation generates outside that list.

| Derived state | Lifecycle concern |
| --- | --- |
| Per-program launchers | Regenerate them from the installed package set when their origin can be derived |
| ELF files edited after installation | On-disk bytes may diverge from dpkg's recorded checksums. Repack before indexing when distributing a translated feed |
| Relative symlink rewrites and alternatives | Reconcile rewritten targets as packages change. The alternatives database owns its links separately |
| Loader cache | Rebuild it from the installed tree |
| Adopted binaries and locally generated configuration | Record their origin if a package purge should remove them |

If all derived state is reconstructible, an idempotent post-transaction
step can make dpkg the source of truth and rebuild the remaining files.
When provenance cannot be derived, a per-package manifest can make
teardown precise and adds a second database to keep consistent. Rebuilding
the entire prefix trades tracking for more work. Lazy cleanup can leave
stale launchers and links.

Open design questions include conffile prompts on upgrade, how to match
old and new translated archives, the scope of purge, and which metadata
a repack must preserve (modes, owner, set-id bits and link relationships).
The source records these as design alternatives. The
[historical feed proposal](../apt-feed/README.md) supplies the distribution context.
