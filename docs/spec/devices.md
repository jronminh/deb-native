# The device table: synthetic devices for a prefix

Status: **proposal** (2026-10-19).  Not in 0.9.0 and not part of T1.  This is
the frame for a new milestone ("0.10 devices"): the prefix keeps `/dev` as the
host's truth by default, and a boot may add **synthetic devices** whose
semantics the runtime owns and whose data plane a host backend serves.

The path law is [`mapped-kernel.md`](mapped-kernel.md) and
[`../architecture.md`](../architecture.md), "The law".  What a program may rely
on is [`contract.md`](contract.md); this page adds a new kind of row to that
contract.  The host side is [`host.md`](host.md).

## Why 0.9.0 cannot do it

Today `/dev` is a **host row**: `binds_defaults()` adds `/proc`, `/sys` and
`/dev` as identity rows (`src/dn-tiny/policy.c`), so guest `/dev/X` is the
phone's `/dev/X`.  The contract says a host `/dev` node is the host's and its
`ioctl` is passed through (`contract.md`, "ioctl / device").  `B_SYNTH`
synthesizes **file contents** (the mount view), not a stateful device.

A fake device needs what the path law deliberately does not have:

- a stateful **fd** with a device instance behind it;
- `ioctl`, `mmap`, `read`, `poll` routed by fd, not by path;
- `stat`/`fstat` that agree with the node (char device, `st_rdev`);
- a data plane (frames, events) between the runtime and a host backend;
- hotplug and clean failure when the backend is gone.

That is a **device ABI**, a new tier.  It is not a bind and not a patch to
0.9.x; with the feature off, behaviour must stay byte-identical.

## Contents

- [The model](#the-model)
- [The device table](#the-device-table)
- [The device fd](#the-device-fd)
- [The provider protocol](#the-provider-protocol)
- [Policy: data and authority](#policy-data-and-authority)
- [Consistency](#consistency)
- [Lifecycle and failure](#lifecycle-and-failure)
- [Security and limits](#security-and-limits)
- [Conformance](#conformance)
- [Milestones](#milestones)
- [Non-goals](#non-goals)
- [Open questions](#open-questions)
- [Prior art](#prior-art)

## The model

```
guest program
   │  open/ioctl/mmap/read/poll
runtime (device frontend: node, fd tag, ioctl table)
   │  DN_DEV/1 provider protocol (host-only socket, fds over SCM_RIGHTS)
provider (device model: fbdev, evdev, ...)
   │  optional backend transport
backend (app: Surface/input; Termux: file viewer; ADB: none)
```

- The **runtime** owns the node's face: paths, fd tags, `stat`, ioctl
  validation, errors.  It is host-agnostic.
- A **provider** owns the device model: structs, commands, events, state.  It
  may be a process next to the runtime, inside the app, or on the other end of a
  socket.  A complex model does not grow `dn-tiny`.
- A **backend** owns the physical side when the provider needs one (pixels,
  input, audio).  Provider and backend may be the same process; the protocol
  does not require it.

The device table does **not** replace `boot/binds`.  A bind is a path; a device
is a stateful fd.  A `pass` device uses the existing host row (or a bind); a
`broker` device has no host node at all.

## The device table

`ROOT/boot/dev` is a data file read once at boot, like `boot/binds`.  One device
per line:

```
# guest path        kind   strategy  provider   options
/dev/fb0            char   broker    display
/dev/input/event9   char   broker    input
/dev/kgsl-3d0       char   pass      -
/dev/kvm            char   refuse    -
```

- `guest path` — must be under `/dev/` in v1.  It is a **proper row**: the
  longest guest row wins, as in the path law.
- `kind` — `char` in v1; `block` parses but is refused.
- `strategy`:
  - `pass` — use the host node (the 0.9.0 rule), now declared on purpose.
  - `broker` — provider-served, no host node.
  - `refuse` — present in the table, absent to programs (`ENOENT`), and named
    in the boot log.  An explicit refusal is better than a silent host leak.
- `provider` — a name the **host** offers; `boot/dev` cannot invent one (see
  [Policy](#policy-data-and-authority)).
- `options` — `replace` (allow shadowing a name the host already has),
  `mode=NNNN`, `ro` (reads only).  Unknown options are an error for that line.
  Options apply to `broker` rows; a `pass` row keeps the host node's own mode.

Rules:

- No `boot/dev` file = 0.9.0 behaviour: `/dev` stays a plain host row.
- A line that does not parse is logged and skipped; it never fails the boot.
- `broker` without a live provider: the node exists, `open` returns `ENODEV`.
  That is a truthful "the device is unplugged", not a fake success.
- `pass` for a name the host does not have is a boot-log warning and `ENOENT`.
- `replace` is required for a `broker` row to shadow an existing host node;
  without it the line is skipped.  `refuse` always shadows -- hiding the name is
  its purpose.  `replace` is allowed for data devices only; authority devices
  may never shadow a host node.

The table lives in the tree, so a guest can edit it for the next boot.  It is
therefore **not** an authority grant.  The host decides which providers exist
and which of them may carry authority; the table only selects among them.

## The device fd

`open("/dev/fb0")` on a `broker` row does not reach the host path.  The runtime:

1. validates the row and asks the provider to open an instance;
2. creates a local fd the kernel can hold — a `memfd`, `eventfd`, or
   `socketpair`, depending on the device — and maps the provider's data fd onto
   it (fd passing, see below);
3. records a **tag** for `(pid, fd)` and returns the fd.

Semantics the tag must preserve:

| operation | behaviour |
| --- | --- |
| `read`/`write` | routed to the provider; device-defined |
| `ioctl` | checked against the provider's ioctl table; unknown `ENOTTY` |
| `mmap` | provider supplies a memfd/dma-buf; runtime maps it; offsets device-defined |
| `poll`/`select`/`epoll` | an `eventfd` the provider rings; `POLLHUP` on provider death |
| `fstat`/`stat` | synthetic char device: `S_IFCHR`, stable `st_rdev`, mode, `st_size = 0` |
| `dup`/`dup2`/`F_DUPFD` | tag copied, instance refcounted |
| `close` | refcounted; final close releases the instance |
| `fork` | child inherits the fd and its tag |
| `exec` | `FD_CLOEXEC` closes; other fds and tags survive |
| `SCM_RIGHTS` | **refused** in v1 (`EOPNOTSUPP`) when a guest passes a synthetic fd |

`/proc/self/fd/N` and `readlink` already report the guest path through the path
law; the tag must not change that.

Implementation may put the table in the tracer (for raw syscalls and static
programs) and in the shim (for dynamically linked programs), but the two must
agree.  The existing shared-generation/seqlock pattern is the precedent; the
tag table is per process and must survive `vfork` exactly as fds do.

`stat` must not lie about the type: a client that checks `S_ISCHR` and
`major()/minor()` must see a plausible device.  Use the Linux majors/minors of
the modelled device (fb `29:0`, input `13:*`, DRM `226:*`) or a documented
allocation; do not reuse a number already in the host `/dev`.

## The provider protocol

Transport: a UNIX socket the **host** creates outside the tree (the guest must
not reach it), `SOCK_SEQPACKET` preferred, length-prefixed frames, `SCM_RIGHTS`
for fds.  The runtime learns the provider directory through its own environment
(`DN_DEV_DIR=<host path>`) or an inherited fd; the host path is never placed in
`boot/dev` and never bound into the tree.  A `provider` name resolves to
`<DN_DEV_DIR>/<name>.sock`; a name with no socket is a provider the host does
not offer (`ENODEV`).

Version 1:

| message | direction | fields |
| --- | --- | --- |
| `HELLO` | provider → runtime | protocol version, provider name, device kinds |
| `WELCOME` | runtime → provider | accepted version, limits |
| `OPEN` | runtime → provider | guest path, flags, mode |
| `OPENED` | provider → runtime | instance id, optional data fd |
| `CLOSE` | runtime → provider | instance id |
| `IOCTL` | both | instance, cmd, direction, size, bytes |
| `MMAP` | runtime → provider | instance, offset, length, prot |
| `MAP` | provider → runtime | fd, map offset |
| `READ`/`WRITE` | both | instance, bytes |
| `POLL` | runtime → provider | instance, event mask, token |
| `EVENT` | provider → runtime | instance, mask, token |
| `HOTPLUG` | provider → runtime | add/remove, guest path |
| `BYE` | either | reason |

`ioctl` is never forwarded blindly.  Each provider publishes an **ioctl table**
per device kind:

```
cmd, size, direction (in/out/inout), name
```

The runtime copies exactly that many bytes to/from the tracee and rejects
anything not in the table with `ENOTTY`.  v1 supports fixed-size arguments only;
variable-size or pointer-bearing structs are modelled as a fixed header and a
provider-specific secondary call, or refused.  A provider never receives a
tracee pointer.

`mmap` returns a `memfd` (or, later, a dma-buf).  The runtime maps it and keeps
the device offset rules in the model.  A provider may share one memfd across
instances; refcounts are the runtime's.

Limits: max protocol frame (1 MiB in v1, configurable), max ioctl argument,
max mmap, max instances per device, and a reply timeout.  A provider that exceeds them is
disconnected, not trusted.

## Policy: data and authority

Two **trust tiers**, decided by the **host**, not by `boot/dev`:

- **data** — display, input, audio, log, sensors.  A synthetic device can only
  change what a program sees; it grants no Android capability.  Allowed when the
  host offers the provider.
- **authority** — `/dev/mem`, `/dev/kvm`, block devices, raw network, `binder`,
  anything that would imply privilege over the host or the kernel.  Refused by
  default.  A host may grant one for a named workload; the grant lives outside
  the tree (host config or environment) and is not editable by the guest.

The host's provider registry is the real limit: `boot/dev` can select a
provider, but only one the host put on the socket directory.  A guest cannot
invent a provider, cannot point one at a host path, and cannot make a data
provider into an authority provider.

`pass` devices are host nodes: they keep the 0.9.0 rule (the node's truth).
For `/dev`, the host identity row already makes them reachable (SELinux may
still refuse the node); a bind is not required.  Declaring one in `boot/dev`
does not create a bind; it documents intent and, in a future version, may add
policy checks.  `ioctl` on a passed node stays the kernel's.

## Consistency

A program may probe.  The model must answer the same story on every door:

- `stat`/`fstat` — char device, stable `st_rdev`, mode from the table;
- `open`/`access` — `ENOENT` for `refuse`, `ENODEV` for an unplugged broker;
- `/proc/self/fd/N` — the guest path;
- `readlink` — the guest path;
- `mmap`/`ioctl` — same size/format the `stat` implies;
- hotplug — a removed device makes `poll` report `POLLHUP` and new opens fail.

v1 does **not** synthesize `/sys/class/*`, `/proc/devices` or a udev database.
Programs that enumerate by sysfs will not find synthetic devices; programs that
open the node by name will.  This is a documented limitation, not a leak.  It is
also why a model should be reachable by its documented name first (Qt
`linuxfb`, `fbi`, `evtest` can be given a path).

## Lifecycle and failure

- The host creates providers before the runtime starts; the runtime connects
  lazily on the first `open` of a brokered node.  If the provider is missing,
  the open fails cleanly and the next open retries.
- Provider death: existing fds get `EIO` on the next operation, `poll` reports
  `POLLHUP`, the instance is released.  There is no automatic restart in v1;
  the host may relaunch the provider and the next `open` succeeds.
- Provider restart: new instances only; old fds stay dead.  A device model may
  define a reattach, but the runtime does not guess.
- Boot with no provider: the node is absent (`ENOENT`) or unplugged
  (`ENODEV`) according to the row; the prefix still boots.
- A prefix is one boot: no state survives it; the provider and the backend own
  any state they need.

## Security and limits

- Provider sockets live outside the guest-visible tree, mode `0600`, owned by
  the host identity.  The guest cannot connect to them.
- The guest is untrusted: every ioctl is size-checked, every mmap offset is
  validated, every frame is bounded.
- The provider is trusted but not privileged: it cannot ask the runtime to open
  a host path, bind a directory, or change identity.  It can only serve fds and
  bytes.
- Synthetic devices are data, not authority.  Authority requires a host grant
  outside the tree.
- Failure is local: a bad provider must never panic the runtime, leak a host
  fd, or make an unrelated path answer differently.

The general bypass classes and the rules they imply are in
[`../reference/syscall-security.md`](../reference/syscall-security.md); a
synthetic device is a parser for guest arguments, not a trusted caller.

## Conformance

The infrastructure ships a **fake provider** (an echo device) and a test that
covers:

- `open`/`stat`/`fstat`, `S_ISCHR`, stable `st_rdev`;
- ioctl table: valid, wrong size, unknown, in/out copy;
- mmap: bounds, offset, shared/private, unmap;
- `read`/`poll`/`epoll`, nonblocking, `POLLHUP`;
- `dup`, `close`, refcount, `fork`, `exec`, `FD_CLOEXEC`;
- `SCM_RIGHTS` refusal;
- provider death and restart;
- parse errors in `boot/dev`, `replace`, `refuse`, `pass` without a node;
- leak check: no host fds or paths reachable from the guest.

Each device model adds its own client test: `fbi` and Qt `linuxfb` for the
reference fbdev; `evtest`/`libinput` for evdev; and so on.  A model without a
test is a proposal, not a promise.

## Milestones

| | what | proves |
| --- | --- | --- |
| **D0** | device table + fd tags + protocol + fake provider | the mechanism; no real device |
| **D1** | fbdev reference provider (see [`devices/fbdev.md`](devices/fbdev.md)) | one full client on a Surface, software |
| **D2** | evdev provider ([`devices/input.md`](devices/input.md)) | input without `/dev/uinput`; `evtest`/compositor |
| **D3** | app backend (Surface + input) | the Android host, still host-agnostic runtime |
| **D4** | a named second device: [audio](devices/audio.md), [TUN](devices/tun.md), or [uinput](devices/input.md) | the table is general |
| **D5** | [DRM study](devices/drm.md) (not a promise) | only when a compositor is the target |

D0-D2 need no shared-memory broker (memfd is enough) and no GPU.  They do not
depend on T2 or T3.

## Non-goals

- A general "load a kernel module" facility.
- Authority devices by default.
- A complete `/sys`/udev universe for synthetic nodes.
- Byte-for-byte emulation of every ioctl of a real driver.  A model promises a
  table, not the universe.
- GPU zero-copy in v1.
- Replacing `boot/binds`; paths remain paths.

## Open questions

1. **Tag storage**: tracer table, shim table, or one shared side table?  How is
   it keyed across `fork`/`exec` and what happens to a tag when the shim and
   tracer disagree?
2. **`SCM_RIGHTS`**: refusing is safe but some clients pass fds; a later
   version needs a provider-assisted transfer.
3. **Provider location**: in-process (fast, grows the runtime) or out-of-process
   (isolated, extra hop).  The protocol should allow both; the first
   implementation should pick one.
4. **Sysfs/udev**: how much to synthesize for the first target, and which
   programs actually need it.
5. **Authority grants**: the exact host-side form (env, a file outside the
   tree, a per-boot capability) and how they are audited.
6. **boot/dev trust**: today it is guest-writable data.  Is selecting a
   host-offered provider harmless by construction, or does the host need a
   per-prefix allowlist?
7. **Device numbers**: a fixed allocation table, or follow the Linux majors
   and risk colliding with a future host node.
8. **Performance**: when to move a model in-process; what the ioctl-per-message
   cost does to a compositor.

## Prior art

- **gVisor** — device semantics at the syscall boundary (pty, tun, vfs); the
  closest model.
- **CUSE/FUSE** — a char device in userspace; needs kernel support the app does
  not have, but the ioctl-size forwarding is the same idea.
- **vhost-user / virtio** — a device split across processes with shared rings;
  a transport reference, not a bus for a prefix without a guest kernel.
- **QEMU device models** — a provider's complexity independent of the VMM.
- **Mesa `drm-shim`** — a narrow DRM model, enough for a test stack.
- **WSL1** — syscall translation with a very small device surface; the lesson
  is not to emulate the whole kernel.
- **Host-integration platforms** — Crostini/Sommelier, WSLg, and Android's
  AVF/Terminal app show a host that owns the screen and a guest that owns its
  compositor; libhybris and xf86-video-hwcomposer show Android HALs from glibc;
  libudev-zero/seatd are the input/seat pieces; vfio-user/vhost-user are the
  device-backend split.  Details in [`../reference/prior-art.md`](../reference/prior-art.md),
  "Other hosts, devices, and ABI".

## References

In the repository: [`contract.md`](contract.md), [`host.md`](host.md),
[`../architecture.md`](../architecture.md), [`../reference/prior-art.md`](../reference/prior-art.md),
[`devices/fbdev.md`](devices/fbdev.md).
