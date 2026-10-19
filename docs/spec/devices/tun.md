# TUN: a host-granted network device

Status: **proposal** (2026-10-19).  Device model for the
[device table](../devices.md), deliberately later than display/input.  A TUN
device is not a display-style data device: it moves IP packets and it touches
Android's VPN boundary, so the host must grant it.

Named workload: a VPN client (WireGuard/OpenVPN) or a program that needs a
private interface.

## The node

| | |
| --- | --- |
| guest path | `/dev/net/tun` |
| kind | `char` |
| `st_rdev` | `10:200` |
| model | one TUN interface per instance, IP only |
| access | read/write packets; `TUNSETIFF` and the common ioctls |

TAP (Ethernet) is not promised; a program that asks for `IFF_TAP` is refused.

## The host side

Android's network stack is the framework's:

- A VPN app calls `VpnService.prepare`/`establish`, receives a `tun` fd, sets
  addresses, routes and DNS, and owns the interface.
- The prefix cannot open the host tun, cannot create one, and must not add
  routes behind the app's back.

So the model is a **granted tunnel**, not a general TUN factory:

- The app establishes a VPN and passes the tun fd (or a packet channel backed
  by it) to the provider, together with the interface name, MTU, addresses and
  the fact that routing is the app's.
- The provider serves `/dev/net/tun` in the prefix.  `TUNSETIFF` selects the
  one interface the host granted; a second name is `EBUSY`/`EINVAL`.
- `read`/`write` move one IP packet each (no packet-info header in v1;
  `IFF_NO_PI` is assumed).  `poll` reports readability; a dead app fd is
  `EIO`/`POLLHUP`.
- `TUNGETIFF` reports the granted name and flags.  `TUNSETOWNER`/`TUNSETGROUP`
  are accepted only for the fake ids (no host effect); `TUNSETPERSIST` is
  refused (the tunnel ends with the session); offload/vnet-hdr are `EINVAL` or
  `ENOTSUP`.

## Routing and addresses

The app's `VpnService` owns routes and DNS.  The guest sees an interface but
**does not control routing**:

- `ip addr`/`ip route` inside the prefix show the host's view (netlink policy,
  [`netlink.md`](../netlink.md)); they do not add routes to the VPN.
- A program that needs the tunnel as its default route relies on the app's
  configuration, not on its own `ip route add`.
- DNS through the tunnel is the app's resolver configuration, not a guest
  `/etc/resolv.conf` rewrite.

This is a deliberate limit: letting a guest change Android's routing is
authority, and the app holds it.

## Security

- The app must hold and grant the VPN; the provider cannot establish one.
- The guest cannot open a raw socket or bypass the tunnel; those are refused by
  the contract.
- The provider never exposes the app's other interfaces or the host's routing
  table beyond the netlink policy.
- One tunnel per prefix in v1.

## Tests

- A C program opens `/dev/net/tun`, `TUNSETIFF` with the granted name, reads and
  writes an ICMP echo through the app's tunnel.
- `ping` through the interface; `ip addr` shows the granted address; a route
  added by the guest does not affect Android.
- App revokes the VPN or the fd dies: reads fail, `poll` reports `POLLHUP`, the
  node stays but `open` is `ENODEV`.
- A second interface name is refused; `IFF_TAP` is refused.

## Milestones

| | scope |
| --- | --- |
| **T1** | read/write + `TUNSETIFF`/`TUNGETIFF` on a host-granted fd |
| **T2** | owner/group/MTU/offload answers; clean revoke |
| **T3** | a real VPN client end to end |

## Non-goals

- TAP/Ethernet, bridges, raw sockets, `CAP_NET_ADMIN`.
- Guest-controlled Android routing or DNS.
- A general TUN factory for arbitrary interfaces.
- Packet-info headers, multi-queue, offload.

## Open questions

1. **fd vs channel**: pass the app's tun fd through, or copy packets over a
   socket?  The fd is zero-copy but its ioctls belong to the app; a channel is
   safer but adds a copy.
2. **Interface name**: does the app choose it, or does the guest's requested
   name map to it?
3. **Addresses**: should the guest see the VPN address via netlink, or only via
   `TUNGETIFF`/a config file?
4. **Permission UI**: the app must ask the user for VPN consent; what happens to
   the prefix while the dialog is open?
5. **IPv6**: same path; test it, do not assume it.

## Sources

The Linux TUN/TAP documentation (`tun(4)`, `TUNSETIFF`); Android's
`VpnService` documentation for the host grant; [`../netlink.md`](../netlink.md) for the route
policy; `ping` and a VPN client as the workloads.
