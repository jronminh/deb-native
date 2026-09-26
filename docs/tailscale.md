# Goal: run Tailscale natively

Install Tailscale's Debian `arm64` package into a deb-native prefix and run
`tailscaled` + `tailscale` unprivileged. It is the cleanest worked example of
the package class that lands on the **tracer boundary**: a static, path- and
network-sensitive daemon.

Not started — this is the plan, measured from the package itself.

## What the package is (`tailscale 1.102.4`)

Checked from `https://pkgs.tailscale.com/stable/debian` (it is **not** in
`deb.debian.org`):

- `usr/sbin/tailscaled` and `usr/bin/tailscale`: **statically linked Go**
  binaries (no `PT_INTERP`, no `NEEDED`). **The libc shim cannot redirect
  them** — this is a `tracer/` (fork-lite) case, not a shim case.
- control scripts: **no `preinst`**; `postinst` only calls
  `deb-systemd-helper`/`systemctl`, every call `|| true` and gated on
  `/run/systemd/system`, so it **degrades gracefully without systemd**.
- `Depends: iptables` (Recommends `iproute2`), systemd units, and
  `/etc/default/tailscaled`.
- Size ~35 MB; installs to ~69 MB.

## Why it is hard here

| need | reality on this device | way through |
|---|---|---|
| kernel TUN + `CAP_NET_ADMIN` | `/dev/net/tun` exists but is root-only | **userspace networking** (`--tun=userspace-networking`) — no TUN, no root |
| systemd service | Termux has none | run `tailscaled` **by hand**, no unit |
| `iptables` | Debian dep; needs netfilter/root | userspace mode does not use it (installs as a no-op dep) |
| path redirection | the binary is static → it reads the **real** Android `/` | the tracer, or explicit `--state`/`--socket` |

## The identity problem

Two identities are in play, which is why an installer "cannot tell what type
of Linux":

- the **prefix** already says Debian trixie — `base-files` ships
  `/etc/os-release` → `usr/lib/os-release` (`ID=debian`, trixie) and
  `/etc/debian_version`;
- but a **static Go binary does not load our shim**, so it reads the *real*
  root — Android's `/etc`, with no `os-release`, `systemctl` or `lsb_release`.

So anything inside `tailscaled` that probes the distribution sees Android, not
the fake Debian. That is exactly the gap the tracer closes, and the concrete
reason this goal is worth doing: **a real static, path-sensitive daemon.**

### Observed: Tailscale's own installer fails in raw Termux

`curl -fsSL https://tailscale.com/install.sh | sh` run directly in Termux
(not through the prefix) prints:

```
Couldn't determine what kind of Linux is running.
...
OS=other-linux
UNAME=Linux localhost 5.10.240-android12-9-… aarch64 Android
No /etc/os-release
```

Expected, and not our pipeline: the script runs under Android's `/bin/sh` on
the real root, where there is no `/etc/os-release` and `uname` reports
`Android`, so it cannot classify the system. Our prefix *does* carry
`ID=debian` (from `base-files`), but only a process launched **under the shim**
sees it — and their installer never was.

So do **not** run Tailscale's installer. Install the `.deb` into the prefix
(add their repo to the prefix's apt sources, or `dpkg -i` the file), or run
their installer through `dn-shell` so `/etc/os-release` resolves to the prefix.
Either way the shipped binaries remain static → runtime still needs the tracer.

## Plan

1. **Tracer first.** Make fork-lite redirect a static binary's syscalls
   (`openat`/`stat`/`socket`/…), so `tailscaled` sees the prefix. Alternative
   stopgap before the tracer: pass every path explicitly (below).
2. Add Tailscale's repo to the prefix's apt sources
   (`deb [trusted=yes] https://pkgs.tailscale.com/stable/debian trixie main`),
   or just `dpkg -i` the downloaded `.deb`.
3. Install into a test prefix; expect the systemd-only `postinst` to warn and
   continue.
4. Run `tailscaled` by hand in userspace mode, then `tailscale up`:

   ```sh
   R=$DN_PREFIX/root
   tailscaled --tun=userspace-networking \
       --state=$R/var/lib/tailscale/tailscaled.state \
       --socket=$R/run/tailscale/tailscaled.sock \
       --socks5-server=127.0.0.1:1055 \
       --outbound-http-proxy-listen=127.0.0.1:1056 &
   tailscale --socket=$R/run/tailscale/tailscaled.sock up --accept-dns=false
   ```

   `--accept-dns=false` because there is no `systemd-resolved`; the SOCKS5 /
   HTTP proxy is how the rest of the system then reaches the tailnet.

5. There is no service manager: start `tailscaled` manually (later: a
   `termux-services`/runit `run` script, see [`design.md`](design.md)).

## Open questions

- Does `tailscaled` start at all under userspace mode in the app uid, and what
  paths/syscalls does it actually hit? (Answer with `strace`/the tracer.)
- What is the minimum syscall set fork-lite must cover for it (`openat`,
  `newfstatat`, `socket`, `connect`, `unix` sockets, netlink)?
- DNS in userspace mode without `systemd-resolved`.
- Does the `postinst` run clean under our glibc shell (missing
  `deb-systemd-helper` is `|| true`, but confirm).

## Status

Not started. First dependency is **fork-lite running static binaries** — see
[`direct-usage.md`](direct-usage.md) and [`../tracer/README.md`](../tracer/README.md).
