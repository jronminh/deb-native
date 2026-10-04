# Known issues

Confirmed breakages in the current tree: reproduced, not merely suspected.
This is a catalog of the current state, like the other specs — not a
roadmap. Deeper tracking and future work live in [`TODO.md`](../../TODO.md);
each entry here says how to reproduce the problem, what actually happens,
and the known root cause.

## Contents

- [Maintainer scripts with a raw interpreter shebang](#maintainer-scripts-with-a-raw-interpreter-shebang)
- [figlet's alternatives symlink points into Termux's tree](#figlets-alternatives-symlink-points-into-termuxs-tree)

## Related docs

- `design.md` — the path shim and the maintainer-script mechanism these
  issues hit.
- `install-flow.md` — the bootstrap where maintainer-script shebangs are
  rewritten.
- `host-userland.md` — the userland/host split; the shim is userland
  session state.

## Maintainer scripts with a raw interpreter shebang

**Symptom.** `dpkg --configure ca-certificates` (or an install/reinstall of
it) fails and leaves the package `iF`:

```
sed: can't read /etc/ca-certificates.conf: No such file or directory
dpkg: error processing package ca-certificates:arm64 (--configure):
 installed ca-certificates:arm64 package post-installation script subprocess returned error exit status 2
```

**Root cause.** Most maintainer scripts have their shebang rewritten to
`#!$INSTDIR/usr/bin/dn-shell` — the launcher, which sets `LD_PRELOAD` to the
path shim before the interpreter runs. A few are missed by that rewrite
(`patch-scripts-tree.sh`) and keep a raw `#!$INSTDIR/usr/bin/dash`. On this
prefix they are `ca-certificates`, `cpp`, `figlet`, `gcc` and
`libcrypt-dev` (5 of 22 `postinst`s).

When the kernel executes such a script, its interpreter runs **without** the
shim, so the script's `/etc`, `/usr`, ... reads hit Android's real root
instead of the prefix. Running the interpreter explicitly instead of via the
shebang does not have this problem. Minimal repro, in the userland:

```sh
cat > ~/t.sh <<'EOF'
#!/data/data/com.termux/files/home/.dn/usr/bin/dash
sed -n 1p /etc/ca-certificates.conf
EOF
chmod 755 ~/t.sh
~/t.sh            # sed: can't read /etc/ca-certificates.conf  (rc=2)
dash ~/t.sh       # prints the file                            (rc=0)
```

**Workaround.** Invoke the missed script through the interpreter with the
shim in the environment (re-export `DN_INSTDIR` and `LD_PRELOAD` first). The
durable fix is to rewrite every maintainer-script shebang, not a subset.

## figlet's alternatives symlink points into Termux's tree

**Symptom.** After installing `figlet`, the command is not found: its
alternatives link resolves into Termux's tree, where nothing exists.

```
$ ls -l $DN/usr/bin/figlet
/.../usr/bin/figlet -> /data/data/com.termux/files/usr/etc/alternatives/figlet
```

**Root cause.** `figlet`'s postinst is one of the missed raw-shebang scripts
above. Its `update-alternatives` call then resolves to Termux's real one,
which writes the link against Termux's own `/etc/alternatives` rather than
the prefix's (`setup-runtime.sh`'s `priv/update-alternatives` wrapper, which
forces `--altdir`, is not on dpkg's maintainer PATH).
