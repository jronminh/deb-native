# third_party/adbwire/

<!-- template: templates/readme.template.md -->

Vendored source for **`adbwire`**, the minimal Wireless-Debugging ADB client
from [`termux-adb-bridge`](https://github.com/jronminh/termux-adb-bridge)
(GPL-3.0), built into the prefix by `scripts/install/setup-runtime.sh` so
`dn-adbwire` works with no separate checkout. It opens one connection per
command and runs it at Android's `shell` UID, with no daemon. The client
carries no secret: it pairs with the device key at `~/.android/adbkey`.

- `adbwire.c` — the ADB client: mDNS discovery, SPAKE2 pairing, TLS 1.3,
  `shell,v2` with real exit codes, and stdin streaming.
- `spake2.c`, `spake2.h` — SPAKE2-over-Ed25519, to match Android's pairing.
- `ed25519/` — the ref10 Ed25519 implementation from
  [`orlp/ed25519`](https://github.com/orlp/ed25519) (zlib), vendored
  unmodified; `ed25519/NOTICE` carries its license and must be kept.
