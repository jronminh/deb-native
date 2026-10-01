# Per-package fixes

> Template: [`templates/readme.template.md`](../templates/readme.template.md). Read this
> file before touching anything in this directory or guessing a
> file's purpose from its name alone. Add a `README.md` like this one
> whenever a new directory holds more than a couple of files that
> aren't self-explanatory from their names alone.

`custom/<package>.sh` is run by `scripts/install/dn-translate-deb.sh` with the
extracted package tree and the prefix path, for a Debian package that needs
a prefix-specific change before dpkg sees it (the naibed branch's
`fusion-custom/`). None are needed yet.
