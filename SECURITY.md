# Security policy

deb-native is **pre-alpha and not independently audited**. Treat it as unsafe
for anything you would not run in a throwaway Termux install.

## Reporting

Please do **not** open a public issue for a security problem. Use GitHub's
[private vulnerability reporting](https://github.com/jronminh/deb-native/security/advisories/new)
(Security → Report a vulnerability), or contact the maintainer
(<https://github.com/jronminh>). Include what you ran, the prefix's
`boot/version`, and the impact. Response is best-effort; there is no SLA.

## Scope

The runtime has **no isolation by design**: a prefix's programs are ordinary
processes of the app that runs them, and any of them can reach the app's files
(the threat model is accidental leaks, [`docs/architecture.md`](docs/architecture.md)).
So the interesting issues are narrower:

- In scope: the tracer or the shim doing something a program did not ask for —
  a path mapped to the wrong file, a write landing outside the prefix when the
  program named a path inside it, `build-prefix.sh` touching anything outside
  the directory it was given.
- Out of scope: the lack of isolation itself, anything that needs root, and
  anything that requires an already-compromised Termux.
