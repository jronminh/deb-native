# Netlink: a family policy, not a broker

Status: **proposal** (2026-10-19).  T2 service note, not in 0.9.0.  Netlink is
a kernel transport: there is no userspace object to bind and no `memfd` to
share.  The host allows some families, kills some, and hides others; the
runtime's job is to make the answer **per family** and consistent, not to
invent a routing table.

This page decides policy.  It does not add a broker; a family that needs a
synthetic view gets one only with a named workload and a test.

## Why a policy

The socket is created by `socket(AF_NETLINK, ...)`, which may be allowed while a
particular protocol is not.  A program then calls `bind`/`sendmsg`/`recvmsg` and
reads the kernel's answer.  The app domain's answer is the host's truth; where
it is killed or hidden, the runtime must refuse cleanly, not let a raw host
answer leak by accident.

Netlink is also the one place where "the kernel's truth" is accepted as a
**host leak**: a program that opens `NETLINK_ROUTE` sees the phone's interfaces
and routes.  That is the same choice as `uname` and `/proc/cpuinfo`
([`contract.md`](contract.md)); the boundary documents it rather than hiding it.

## Family policy

| family | number | policy | why |
| --- | --- | --- | --- |
| `NETLINK_ROUTE` | 0 | pass if the host allows; else `EPERM` | `ip`, `ss`, network tools; host truth |
| `NETLINK_USERSOCK` | 2 | pass | ordinary user netlink socket |
| `NETLINK_SOCK_DIAG` | 4 | pass if allowed; else `ENOENT` | `ss`; host truth |
| `NETLINK_XFRM` | 6 | `EPERM` | no `CAP_NET_ADMIN` |
| `NETLINK_SELINUX` | 7 | pass if allowed; else `EPERM` | libselinux probe |
| `NETLINK_AUDIT` | 9 | `EPROTONOSUPPORT` | libaudit tolerates it; no audit daemon |
| `NETLINK_NETFILTER` | 12 | `EPERM` | no `CAP_NET_ADMIN`; `iptables` is refused |
| `NETLINK_KOBJECT_UEVENT` | 15 | `EPERM`/`ENOENT` | no udev in the tree |
| `NETLINK_GENERIC` | 16 | per family: pass if the host does, else `EOPNOTSUPP` | ethtool and friends |
| `NETLINK_CRYPTO` | 21 | pass if allowed; else `ENOENT` | probe-only |
| anything else | | `EPROTONOSUPPORT` | do not guess |

The exact set is checked at runtime against the host; the table is the promise,
the probe decides whether a "pass" is possible on a given phone.  A pass that
fails on the host returns the host's error, not a fiction.

## What is not promised

- A synthetic routing table, interface list, neighbour table or conntrack view.
  If `NETLINK_ROUTE` is not allowed, `ip addr` fails honestly; it does not see a
  made-up `eth0`.
- A netlink broker for `NETLINK_GENERIC` families.  A named client that needs
  one gets a separate proposal.
- Netfilter.  The prefix has no `CAP_NET_ADMIN`; `iptables`/`nft` are refused
  ([`contract.md`](contract.md)).
- udev events.  `NETLINK_KOBJECT_UEVENT` is refused; a synthetic input device
  does not rely on udev in v1 ([`devices/input.md`](devices/input.md)).

## Consistency

- `socket(AF_NETLINK)` may succeed even when a protocol is refused; the error
  comes at `bind`/`sendmsg`, as on a real kernel.
- `getsockname`/`getpeername`/`SO_PEERCRED` return the kernel's values for a
  passed family; the ids are the app's residue, as with `AF_UNIX` peers.
- A refused family is a fixed error, not `ENOSYS` one time and `EPERM` the next.
- The policy is the same for every process in the prefix.

## Tests

- `ip addr`, `ip route`, `ss -lnt`, `ss -x` (with the host's answers or a clean
  refusal).
- `auditctl -s` (does not fail on `EPROTONOSUPPORT`).
- `ethtool` (generic family; pass or `EOPNOTSUPP`).
- A probe that opens each family, binds it, and records the exact error.

## Open questions

1. **Route family**: is the host's answer useful enough, or does its interface
   naming leak more than the boundary wants?  The current bias is pass.
2. **SOCK_DIAG**: `ss` is common; is the host's answer stable on both target
   kernels?
3. **SELinux family**: does libselinux need a real answer, or does `EPERM`
   suffice?
4. **Generic families**: a per-family table is data; where does the host
   declare it (a `boot/netlink` file, or a compiled table)?

## Sources

The kernel `netlink(7)` and rtnetlink documentation; the family numbers and
the host's allow/deny answers from
[`../reference/device-census.md`](../reference/device-census.md) and
[`contract.md`](contract.md).  `libaudit`'s tolerance of
`EPROTONOSUPPORT` is the audit-family precedent; `ip`, `ss`, and `ethtool` are
the clients.
