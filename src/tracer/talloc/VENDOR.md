# Vendored: talloc 2.4.3

`talloc.c` and `talloc.h` are the upstream Samba talloc 2.4.3 sources
(https://download.samba.org/pub/talloc/talloc-2.4.3.tar.gz), vendored so
`dn-trace` can be built **self-contained** (static, no external libtalloc.so,
so a poor host can start it with a bare exec).

Copyright (C) 2004-2008 Andrew Tridgell and others; licensed under the GNU
Lesser General Public License, version 3 or later (see the header of
`talloc.c` and the upstream `LICENSE`).

`replace.h` is NOT upstream: it is a minimal stand-in for Samba's libreplace
header, providing only the few macros talloc.c needs (`_PUBLIC_`, `MIN`, the
`TALLOC_BUILD_VERSION_*` numbers) plus the standard headers the upstream
`replace.h` would pull in. Keep it in sync with talloc.c's needs.
