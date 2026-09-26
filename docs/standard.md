# The deb-native standard (spec v1)

What `deb-native` supports, and how a claim about a package is written down
and proved. The mechanism is in [`design.md`](design.md); the function-level
coverage is measured in [`shim-coverage.md`](shim-coverage.md). The model is
`sudo-less`'s `docs/standard.md`, adapted to Android.

## Triage, not universal support

`deb-native` does not promise every `.deb`. It **classifies** each package,
handles the ones in scope with the cheapest mechanism that works, and says
plainly why the others are not. A `never` or "the admin's" verdict is a
decision with a reason, not a gap. Universal coverage is explicitly not the
goal: prefer a clear verdict over a fragile hack.

Two questions, asked of every package:

1. **Scope:** is it the user's, the admin's, or out of reach?
2. **Mechanism:** how does each program run from the prefix — directly with
   the shim, or through the (unbuilt) syscall tracer?

## Scope

| scope | meaning | the user sees |
|---|---|---|
| `user` | installs to `$INSTDIR` and runs by name, unprivileged | `deb-native install PKG`, then `PKG` |
| `admin` | needs root to install or to work (a service, a device, a system user) | a refusal naming what the admin must do |
| `never` | no mechanism fits the rules (setuid, a kernel module, a foreign architecture) | a refusal with the reason |

### By Debian section

A package's scope starts from its Debian `Section`. The split mirrors
`sudo-less`'s:

| scope | sections |
|---|---|
| `user` | libraries/development: `libs`, `libdevel`, `devel`, `debug`, `introspection`, `vcs`; languages: `python`, `perl`, `ruby`, `rust`, `golang`, `haskell`, `javascript`, `java`, `php`, `ocaml`, `lisp`, `gnu-r`, `interpreters`; tools/content: `utils`, `text`, `editors`, `shells`, `doc`, `fonts`, `localization`, `tex`; applications: `science`, `math`, `graphics`, `sound`, `video`, `games`, `electronics`, `hamradio`, `education`, `embedded`; desktop: `x11`, `gnome`, `kde`, `xfce`; mixed client/server: `web`, `comm` |
| `admin` | `admin`, `kernel`, `net`, `mail`, `database`, `httpd`, `tasks`, `metapackages`, and the `required`/`important`/`standard` packages of a base install |
| undecided | `cli-mono`, `gnustep`, `misc`, `news`, `oldlibs`, `otherosfs`, `zope` |

### Signals override the section

Whatever its section, a package is the **admin's** if it:

- depends on `adduser` or creates a system user;
- ships a system service (depends on `init-system-helpers`, installs units or
  init scripts) — Termux has no systemd, and the runit translation is unbuilt;
- needs a setuid/setgid file or a file capability to work (Android will not
  honour them, and they are unsafe).

A client in an admin section (`curl`, `mtr` in `net`) can be brought into
scope as an exception.

### Android-only signals

- **Architecture:** `arm64` only. The Debian archive name (`arm64`) differs
  from the platform's `aarch64`; the current `--force-architecture` workaround
  is flagged unsafe (`findings.md`) and still open.
- **`gui` is an attribute, not a refusal.** On Termux the display is
  Termux:X11/VNC; a package can be `user` and still need a display to do
  anything.
- **glibc-dynamic is an assumption, not a promise.** A program that is
  statically linked, or that reaches the filesystem with a raw `syscall()`,
  is not path-redirected by the shim. That is the syscall tracer's job, and
  until it exists those programs are best-effort — see
  [`shim-coverage.md`](shim-coverage.md#the-real-boundary).

## "Supported" is proved, not predicted

A package is claimed to work only once it has been **installed to `ii` and its
program run from the prefix, by name**, not by the system's own copy on
`PATH`. This is `sudo-less`'s rule, kept verbatim.

## Versioning

**Spec v1** (2026-09). Scope follows the Debian section; exceptions, where the
classifier is wrong, are written down with the case and the reason. Adding a
scope or mechanism is a spec change.
