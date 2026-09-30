# Per-package fixes

`custom/<package>.sh` is run by `scripts/install/dn-translate-deb.sh` with the
extracted package tree and the prefix path, for a Debian package that needs
a prefix-specific change before dpkg sees it (the naibed branch's
`fusion-custom/`). None are needed yet.
