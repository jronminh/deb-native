# Security policy

deb-native is **pre-alpha and not independently audited** — see the warning
in the [README](README.md). Treat it as unsafe for anything you would not run
in a throwaway Termux install.

## Reporting

Please do **not** open a public issue for a security problem. Use GitHub's
[private vulnerability reporting](https://github.com/jronminh/deb-native/security/advisories/new)
(Security → Report a vulnerability), or contact the maintainer
(<https://github.com/jronminh>). Include what you ran, the prefix state
(`termux-dn-doctor`), the version, and the impact. Response is best-effort;
there is no SLA.

## Scope

This project runs unprivileged and has **no isolation by design** — the
userland is ordinary processes sharing Termux's tree. So the interesting
issues are narrower:

- In scope: a userland program or a host-side command here acting **beyond
  its job** — the shim/tracer mishandling a path, the prefix reaching a file
  it should not, or `install.sh` / a generated wrapper doing something
  unexpected in the host prefix (Termux's own tree).
- Out of scope: the lack of isolation itself, anything that already needs
  root, and anything that requires an already-compromised Termux.
