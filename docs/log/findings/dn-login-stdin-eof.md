# Findings: dn-login "welcome then kicked" -- `enter` ran inside a redirected loop (2026-10-04)

<!-- template: templates/docs.template.md -->

**Impact: Repo change.** The app showed the prefix welcome and then the session
exited ("kicked"). The shim's `setfsuid` fix
([`android-seccomp-setfsuid.md`](android-seccomp-setfsuid.md)) was separate;
this is the second bug behind the same symptom.

## Contents

- [Symptom](#symptom)
- [Cause](#cause)
- [Fix](#fix)

## Symptom

With `~/.termux/shell -> ~/.dn-login`, a new session printed the motd welcome
and then closed. `~/.dn-login -l` exited 0 with no prompt, while running
`dn-shell -l` directly gave a prompt and stayed alive.

## Cause

The generator called `enter` -- which `exec`s `dn-shell` -- from **inside** the
`while ... read ... done < "$LISTF"` loops:

```sh
while IFS=... read -r n p; do
  [ "$p" = "$want" ] && enter "$p" "$@"     # exec happens here
done < "$LISTF"
```

`exec` replaces the process image but keeps the open file descriptors, so the
new shell inherited **fd 0 = `$LISTF`**, already read to EOF. An interactive
bash reads EOF on stdin and exits immediately -- the welcome prints (login's
motd runs first), then the shell is gone.

## Fix

Select the target prefix into a variable inside the loop and call `enter`
**after** it, in both the bare-start and `--choose` branches, so fd 0 is the
tty again:

```sh
sel=""
for want in "$EXPLICIT" "$LAST" "$DEFAULT"; do
  [ -n "$sel" ] && break
  while IFS=... read -r n p; do
    [ "$p" = "$want" ] && { sel=$p; break; }
  done < "$LISTF"
done
[ -n "$sel" ] || sel="$first"
enter "$sel" "$@"
```

Verified: `~/.dn-login -l` now prompts (`~ # `) and stays alive. The generator
also repoints `~/.termux/shell` from the stopgap direct `dn-shell` back to
`~/.dn-login`.
