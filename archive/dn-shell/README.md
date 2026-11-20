# dn-shell retirement

dn-shell was a standalone Android terminal APK. It owned a PTY and terminal
UI, managed sessions in a foreground service, and could unpack and enter
a Debian prefix bundled as an APK asset. A later design gave the app a
scriptable bionic host tree that could start before a guest was installed.

FOA supersedes its portable tree and host design. The host tree, deployment
sequence and service interface are documented in
[FOA's bionic tree](https://github.com/jronminh/fhs-on-android/blob/main/tree/bionic.md),
[host tree deployment](https://github.com/jronminh/fhs-on-android/blob/main/recipes/host-tree-deploy.md) and
[service verbs](https://github.com/jronminh/fhs-on-android/blob/main/recipes/service-verbs.md).

deb-native carries forward the [custom-app host contract](../../docs/spec/host.md)
and a separate [next-generation APK proposal](../../docs/spec/dn-ng.md).
See [provenance](../../PROVENANCE.md) for the source of this note.
