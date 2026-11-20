# The boot manager: a flat launcher for trees

Android supplies the firmware and the sandbox; a prefix is self-contained
(`boot/boot.sh`, [`../building.md`](../building.md)); and the host's job is the
session ([`host.md`](host.md)). So the "boot loader" here is a thin **session
launcher over the trees** -- not GRUB. GRUB's model is the reference, trimmed to
what a prefix on Android needs.

## The split it keeps

- **The host is the manager** -- it lists the trees, holds a default, and
  invokes one; it passes the parameters (the command, `DN_TINY_FLAGS`, the
  binds). This is GRUB's role.
- **The prefix is the "OS"** -- it *owns* `boot/boot.sh`/`init.sh` and boots
  itself. The manager invokes it; it never owns or reimplements it, so a tree
  stays portable (Termux, an app, another device).
- The manager is the app screen in dn-shell and a CLI (`dn-boot`) in Termux,
  both over the one contract: **a prefix is a directory with `boot/boot.sh`**
  (format 2, not the old `.dn/contract`).

## Why it is thin

- Android *is* the firmware and the security boundary: no disk or boot sector to
  touch, no kernel/initrd to load (the kernel is the phone's), and no per-entry
  password -- the sandbox is the boundary, not the menu.
- "Boot" is "open a session"; there is no reboot.
- A user has a few trees, not ten OSes: a **flat list**.

## The feature set (lean)

| # | feature | note |
| --- | --- | --- |
| 1 | **A flat list** | the registry is the directory: `prefixes/` in the app, `~/work/*` in Termux; a prefix is one with `boot/boot.sh` |
| 2 | **Enter** | `prefix enter NAME` / a tap opens a session running `boot.sh` with the command and the environment |
| 3 | **Default + timeout** | enter the default after a few seconds |
| 4 | **Safe boot (per entry)** | boot with the shim off (`DN_LANE_PATCH=0`, or `--no-shim`): the one recovery that matters, since the `svc` patch is the fragile part (`mapped-kernel.md`, rule 7) |
| 5 | **Fallback** | a session that dies in under `failSeconds` offers Safe boot or the previous entry (dn-shell already times this) |
| 6 | **A banner** | one line an entry: `Debian trixie - kernel <v> - pre-alpha, not a sandbox` |
| 7 | **Import / export** | a tree from a file or URL into `prefixes/`, and out to a tarball (the app's `DocumentsProvider` over `prefixes/`) |

**Policy is not a menu item.** Which Android capability, socket or bind a prefix
gets is the *app's* decision, hidden from the user.

## What it drops from GRUB, and why

- **groups / submenus, scripting, conditionals** -- too much for a handful of
  trees.
- **passwords / a super-user** -- pointless: the Android sandbox is the boundary.
- **`grubenv`, a one-time entry, serial console, chainloading, network boot** --
  there is no "next boot", no second console, and no nesting ("one prefix, one
  boot", [`../architecture.md`](../architecture.md)).
- **loading a kernel or an initrd, touching a disk** -- the kernel is Android's.

## The minimal version

1 + 2 + 6 (list, enter, banner) is a working first version; 3 + 4 (a default and
Safe boot) make it whole. 5 and 7 come later.

Not "a small GRUB" but **a small `systemd-boot` for trees**: a flat menu, a
default, a safe boot, a banner.

## See also

- [`host.md`](host.md) -- what the host provides a prefix (the session it opens).
- [`variants.md`](variants.md) -- the trees a menu chooses among.
- dn-shell's spec, "§8 Prefixes" and stage 5 -- the same manager inside the app;
  it still scans for `.dn/contract`, which this contract replaces with `boot/`.
