# ELF PT_INTERP patch: the one on-disk edit deb-native makes

<!-- template: templates/docs.template.md -->

deb-native's static "binary surgery" on a `.deb`'s ELF files is exactly one
field: the `PT_INTERP` program header's pathname, rewritten from
`/lib/ld-linux-aarch64.so.1` to the prefix's own fused glibc loader
(`scripts/prefix/dn-translate-deb.sh`, via `dn-elf`). This doc documents
precisely which bytes that touches, why the kernel reads it through
`p_offset` while the loader names itself from `p_vaddr`, why the string
usually has to move, and the invariants that keep the patch safe.

## Contents

- [What gets patched, and when](#what-gets-patched-and-when)
- [ELF layout: the program header table](#elf-layout-the-program-header-table)
- [PT_INTERP: the field the kernel reads](#ptinterp-the-field-the-kernel-reads)
- [Why the string usually has to move](#why-the-string-usually-has-to-move)
- [The dn-elf editor](#the-dn-elf-editor)
- [Invariants and safety](#invariants-and-safety)

## What gets patched, and when

`scripts/prefix/dn-translate-deb.sh` runs once per `.deb`, before `dpkg` ever
installs it (so maintainer scripts never run against an unpatched binary). For
every regular file whose first 4 bytes are the ELF magic (`7f 45 4c 46`):

```sh
interp=$("$ELF" get-interp "$f" 2>/dev/null) || interp=""
case "$interp" in
  */ld-linux-aarch64.so.1) [ "$interp" = "$LD" ] || "$ELF" set-interp "$f" "$LD" ;;
esac
```

`$LD` is `<prefix>/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`. `dn-elf
get-interp` fails (captured, not an error) for static binaries and libraries,
which have no `PT_INTERP`; those are left alone. A file already at `$LD` is
left alone — the step is idempotent.

## ELF layout: the program header table

The kernel finds the table at `e_phoff`, with `e_phnum` entries of
`e_phentsize` bytes. `PT_INTERP` is one entry: a pathname, given by
`p_offset` (where in the file), `p_filesz` (how many bytes), read as a
NUL-terminated string.

## PT_INTERP: the field the kernel reads

At `execve()`, the kernel reads the interpreter pathname **from the file**,
through `PT_INTERP`'s `p_offset`/`p_filesz`, and **never maps it**. It
requires the last byte of the range to be NUL and opens the path up to the
first NUL; anything after is ignored.

But the **loader names itself** from memory: glibc sets
`_dl_rtld_libname.name = main_map->l_addr + p_vaddr` (`elf/rtld.c`), and this
project's loader cuts the run-time prefix from that name. So the new string
must land where `p_vaddr` points, not merely at a new `p_offset`.

## Why the string usually has to move

Debian's own interpreter (`/lib/ld-linux-aarch64.so.1`) is shorter than the
prefix's loader path, so the new string does not fit the old `p_filesz`; it
has to be written somewhere with room — while keeping `p_vaddr` pointing at
it.

## The dn-elf editor

`dn-elf set-interp FILE NEWPATH [CAPACITY]`:

- Locate `PT_INTERP` (fail if none: this rewrites an existing interpreter, it
  does not add one to a static binary).
- If `NEWPATH` fits the old `p_filesz` **and** the entry is consistent
  (`p_offset` is the file offset `p_vaddr` maps to), overwrite in place,
  padded with NUL.
- Otherwise append the NUL-terminated string at the end of the file, grow the
  `PT_LOAD` with the highest virtual end so those bytes are mapped, and set
  `PT_INTERP`'s `p_offset`/`p_vaddr`/`p_paddr` to where it landed. This is the
  one job `patchelf`'s `rewriteSectionsLibrary` does for `.interp`, kept
  minimal: no new program header, no `e_phnum` change, no section. The optional
  `CAPACITY` pads `p_filesz` up to it in the same write, so the build can
  reserve the 256-byte room a relocation needs without a second grow (two
  grows would leave two copies of the path).
- `get-interp FILE` reads the current string; exit non-zero if there is no
  `PT_INTERP`.
- Scope: `ELFCLASS64`, `ELFDATA2LSB`, `EM_AARCH64`, `ET_EXEC`/`ET_DYN`; anything
  else is refused.

## Invariants and safety

- **The kernel needs only a terminated string.** No segment, section or other
  header changes; relocation is a byte write at a recorded offset.
- **No other binary byte names the prefix.** Measured on the artifact: no ELF
  carries the build path anywhere but in `PT_INTERP` (the glibc and the shim
  derive the prefix at run time), so the recorded offsets are the whole job;
  the build fails if a binary names it elsewhere.
- **Every relocation write is checked first.** `.dn/baked-paths` records each
  `PT_INTERP` offset and capacity; the relocation script refuses if the bytes
  there are not the old loader path.
- **Idempotency.** `dn-elf` treats an already-correct interpreter as done.
