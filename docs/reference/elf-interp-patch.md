# ELF PT_INTERP patch: the one on-disk edit deb-native makes

<!-- template: templates/docs.template.md -->

deb-native's static "binary surgery" on a `.deb`'s ELF files is exactly
one field: the `PT_INTERP` program header's pathname, rewritten from
`/lib/ld-linux-aarch64.so.1` to the prefix's own fused glibc loader
`$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1`
(`core/install/dn-translate-deb.sh`, via `dn-elf`). This doc
documents precisely which bytes that touches, why the kernel reads that
pathname through `p_offset`/`p_filesz` while glibc's own loader names
itself from the address `p_vaddr` maps to (so the string must land
there too), why the rewrite usually has to relocate the string rather
than overwrite it in place, and the invariants that keep the patch
safe. `dn-elf` does the edit; it grows one `PT_LOAD` the way
`patchelf`'s `rewriteSectionsLibrary` does for `.interp`, and nothing
more.

## Contents

- [What gets patched, and when](#what-gets-patched-and-when)
- [ELF layout: finding the program header table](#elf-layout-finding-the-program-header-table)
- [PT_INTERP: the field the kernel actually reads](#ptinterp-the-field-the-kernel-actually-reads)
- [Why the string usually has to move](#why-the-string-usually-has-to-move)
- [What patchelf does](#what-patchelf-does)
- [Invariants and safety](#invariants-and-safety)
- [The runtime side, briefly](#the-runtime-side-briefly)
- [What deb-native does NOT touch](#what-deb-native-does-not-touch)
- [A self-brewed replacement: dn-elf](#a-self-brewed-replacement-dn-elf)

## Related docs

- [`package-lifecycle.md`](../spec/package-lifecycle.md) — where the translate
  step (which does this patch) sits in a package's overall lifecycle.
- `ld-dn-config.md` — `ld-dn`'s policy and config
  file; the runtime side this patch hands off to.
- `../log/findings/patchelf-et-exec-runpath.md`
  — the related `patchelf` bug (`--set-rpath` on `ET_EXEC`, not
  `--set-interpreter`) that led to dropping per-file `RUNPATH` rewrites
  entirely; background for "why not patch more than PT_INTERP" below.
- [`design.md`](../spec/design.md) — overall scope and the three path
  mechanisms (maintainer scripts, the libc shim, the tracer); this doc
  is the fourth, much smaller mechanism: a one-field static ELF edit.

## What gets patched, and when

`core/install/dn-translate-deb.sh` runs once per `.deb`, before
`dpkg` ever installs it (so maintainer scripts never run against an
unpatched binary). For every regular file whose first 4 bytes are the
ELF magic (`7f 45 4c 46`):

```sh
interp=$("$ELF" get-interp "$f" 2>/dev/null) || interp=""
case "$interp" in
  */ld-linux-aarch64.so.1) [ "$interp" = "$LD" ] || "$ELF" set-interp "$f" "$LD" ;;
esac
```

(`core/install/dn-translate-deb.sh`). `$LD` is
`$DN/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1` — the absolute,
prefix-rooted path to the loader. `dn-elf get-interp` fails (captured,
not treated as an error) for static binaries and libraries, which have
no `PT_INTERP` segment at all; those are left untouched. A file whose
interpreter is already `$LD` (re-running the translator, or a package
translated twice) is also left untouched — the step is idempotent.

Nothing else in the pipeline (`patch-scripts-tree.sh`, the `#!`
line rewrite loop) touches ELF files; both of those operate on
plain-text scripts.

## ELF layout: finding the program header table

Before `PT_INTERP` can be located, the ELF header (`Elf64_Ehdr`) has to
be read to find the program header table. Three fields locate it
(`elf(5)`, `Elf64_Ehdr`):

- `e_phoff` — file offset of the program header table.
- `e_phentsize` — size in bytes of one entry (fixed per file).
- `e_phnum` — number of entries.

Reading the table is `e_phnum` consecutive `e_phentsize`-byte structs
starting at `e_phoff`. `core/native/ld-dn.c` does exactly this twice: once
implicitly (the kernel already built `AT_PHDR`/`AT_PHNUM` for the
*translated program itself* by the time `ld-dn` runs — see "The runtime
side" below) and once explicitly for the real loader it maps
(`core/native/ld-dn.c:555`, `pread64` of `eh.e_phnum * sizeof(Phdr)` bytes
starting at `eh.e_phoff`).

**Extended phnum (`PN_XNUM`).** If a file has 0xffff (65535) or more
program headers, `e_phnum` cannot hold the count (it is a 16-bit
field), so the ELF spec reserves `e_phnum == 0xffff` as a sentinel: the
real count is instead stored in the section header table's first
entry's `sh_info` (`elf(5)`, `PN_XNUM`). No binary in this project's
scope comes remotely close to 65535 program headers — loader stubs and
Debian programs both have a handful — so this case is unverified in
practice here and not handled by `core/native/ld-dn.c`'s own phdr reader
(which takes `e_phnum` at face value and bounds it to a 32-entry local
array, `core/native/ld-dn.c:205`, `g_ph[32]`, dying if a mapped loader
exceeds that). Worth flagging if `dn-elf` (below) is ever pointed at
an unusual binary, but not a real-world risk for glibc loaders or
Debian arm64 programs.

## PT_INTERP: the field the kernel actually reads

A `PT_INTERP` program header entry (`Elf64_Phdr`) has the same shape as
every other entry: `p_type, p_flags, p_offset, p_vaddr, p_paddr,
p_filesz, p_memsz, p_align`. For `PT_INTERP` specifically, `elf(5)`
says the entry "specifies the location and size of a null-terminated
pathname to invoke as an interpreter," and that it must precede any
`PT_LOAD` entry.

The kernel's own source confirms *which* two fields that "location and
size" means. In `fs/binfmt_elf.c`'s `load_elf_binary()` (Linux kernel,
current mainline, `fs/binfmt_elf.c`), the `PT_INTERP` handling is:

```c
if (elf_ppnt->p_type != PT_INTERP)
    continue;

retval = -ENOEXEC;
if (elf_ppnt->p_filesz > PATH_MAX || elf_ppnt->p_filesz < 2)
    goto out_free_ph;

retval = -ENOMEM;
elf_interpreter = kmalloc(elf_ppnt->p_filesz, GFP_KERNEL);
if (!elf_interpreter)
    goto out_free_ph;

retval = elf_read(bprm->file, elf_interpreter, elf_ppnt->p_filesz,
    elf_ppnt->p_offset);
```

So the kernel allocates `p_filesz` bytes and reads them from the file
at byte offset `p_offset` — a plain file read, independent of any
`PT_LOAD` mapping. **`p_vaddr` and `p_memsz` are not consulted at all**
for this: they describe where the segment would land in memory if it
were loaded, which is irrelevant since `PT_INTERP`'s only job is to
name a path, not to be mapped. (Verified from kernel source, fetched
2026-10-03 from `android.googlesource.com`'s mirror of
`fs/binfmt_elf.c`, upstream-current at the time; the exact variable
names may drift between kernel versions, but `p_offset`/`p_filesz`
being the only fields read for `PT_INTERP` is architectural, not
version-specific — it follows from the field semantics in `elf(5)`
too.)

This is exactly the pair `patchelf --set-interpreter` and `ld-dn.c`'s
own `PT_INTERP` scan (`core/native/ld-dn.c:500`, `interp = (const char
*)(bias + pp[i].p_vaddr)`) both have to keep consistent — `ld-dn.c`
reads via `p_vaddr` because by the time it runs the *kernel has
already mapped the program*, so the string is sitting in memory at its
load address; `p_vaddr` there is just "where in the already-mapped
image," not "how the interpreter string was located" (that happened at
`execve()`, from `p_offset`/`p_filesz`, before `ld-dn` ever got control).
Both reads must agree on `p_filesz` being authoritative for the
string's length, including the terminating NUL (`elf(5)`: "size of a
**null-terminated** pathname" — `p_filesz` includes it).

## Why the string usually has to move

The old interpreter string is the bare `/lib/ld-linux-aarch64.so.1`
(26 bytes) that upstream Debian arm64 binaries ship, or, on a re-run,
already `$LD`. The new string is `$DN/usr/lib/deb-native/ld-dn` — the
prefix-rooted path, which is routinely *longer* than the bare upstream
value and varies in length by installation.

An ELF file has no spare bytes sitting after a string "just in case" —
the file is packed to its declared sizes, and whatever comes right
after the old `PT_INTERP` string's bytes is either more file content or
the next page boundary. When the new string does not fit in the old
`p_filesz`, the string data has to move somewhere with room — typically
appended near the end of the file, in a new location picked by whatever
tool is doing the edit — and **the existing `PT_INTERP` program header
entry's `p_offset` (and `p_filesz`, for the new length) are updated to
point at the new location.** This is the one and only program header
entry that changes. No new `PT_LOAD` or other segment is added for this
to work, because the interpreter string is not itself a segment that
needs to be mapped — it is bytes in the file read once, directly via
`p_offset`/`p_filesz`, as shown above. That is the difference from
`patchelf`'s general-purpose machinery (below), which does add/grow
`PT_LOAD` segments when a *section* genuinely needs new mapped space —
`PT_INTERP`'s payload never does.

## What patchelf does

`patchelf`'s `setInterpreter` (NixOS/patchelf, `src/patchelf.cc`):

```cpp
void ElfFile<ElfFileParamNames>::setInterpreter(const std::string & newInterpreter)
{
    if (getInterpreter() == newInterpreter) {
        debug("given interpreter is already set\n");
        return;
    }

    std::string & section = replaceSection(".interp", newInterpreter.size() + 1);
    setSubstr(section, 0, newInterpreter + '\0');
    changed = true;
    this->rewriteSections();
}
```

(fetched 2026-10-03 from `raw.githubusercontent.com/NixOS/patchelf`,
`master`.) It works through the `.interp` *section* (ELF section
headers are a separate, linker/debugger-oriented table alongside the
program headers; `.interp`'s section and `PT_INTERP`'s segment are
required to describe the same bytes), not by hand-editing the program
header directly. `replaceSection` marks `.interp` for replacement at
the new size; `rewriteSections()` is the general machinery that decides
*where* every replaced/grown section ends up and fixes up every section
and program header that refers to it — for a library, that can mean
placing content in a new `PT_LOAD` near the end of the file; for an
executable with no layout slack, it instead has to shift existing
content and renumber/resize the program header table itself.

That generality is also the risk this project hit in practice, just
not through `--set-interpreter`: `docs/log/findings/patchelf-et-exec-runpath.md`
found `patchelf --set-rpath` on a tightly-packed `ET_EXEC` binary
(`cc1`, no slack, no existing `RPATH` to grow into) miscomputing the new
segment boundary and producing two overlapping `PT_LOAD`s — a corrupt
binary that crashed at loader startup with no syscall in flight.
`--set-interpreter` alone was tested in isolation during that
investigation and came out clean (13 program headers, sane `MemSiz`);
the bug was specific to inserting a *new* `DT_RPATH`/`DT_RUNPATH`
dynamic-section entry on a binary with none. That finding is *why*
deb-native patches `PT_INTERP` only — the `RUNPATH` rewrite was dropped
entirely, the run-time search paths being the loader's own job.
`PT_INTERP`'s
payload is a single flat byte string with a well-defined grow path
(relocate one segment's offset/size, same as described above); it does
not carry the same structural risk that inserting a new dynamic-section
entry did.

## Invariants and safety

- **Temp copy.** `dn-translate-deb.sh` unpacks the `.deb` into a
  `mktemp -d` work directory (`trap 'rm -rf "$WORK"' EXIT`) and only
  overwrites the original `$DEB` file at the very end, after a
  successful `dpkg-deb -Znone -b` repack. A `patchelf` failure mid-loop
  leaves the original `.deb` untouched.
- **Idempotency.** The `case` guard (`[ "$interp" = "$LD" ] || patchelf
  ...`) means re-running the translator on an already-translated binary
  is a no-op for that file, not a second rewrite.
- **Extended phnum.** Not handled (see above) — unverified in practice,
  believed irrelevant at this project's binary sizes.
- **64-bit LE aarch64 only.** Every struct offset and field width in
  this doc, and in `core/native/ld-dn.c`'s own hand-rolled `Ehdr`/`Phdr`
  structs (`core/native/ld-dn.c:39-50`), assumes `ELFCLASS64` +
  `ELFDATA2LSB` (`Elf64_*`, not `Elf32_*`) and `EM_AARCH64` (machine
  value 183, checked at `core/native/ld-dn.c:552`). deb-native only ever
  targets Debian `arm64`; nothing here generalizes to 32-bit ELF or a
  different byte order without separate struct layouts.
- **Only ever one `PT_INTERP` entry.** `elf(5)`: "it may not occur more
  than once in a file." The patch rewrites that single entry in place
  (by field, possibly relocating its payload); it never adds a second
  `PT_INTERP`.

## The runtime side, briefly

The static patch only changes *what path the kernel will read as the
interpreter*; the interesting work happens at `execve()` time inside
`core/native/ld-dn.c`, which this doc does not re-describe in full (see
`design.md` and `ld-dn-config.md`). Briefly, so the connection to the
field above is concrete:

1. The kernel reads the (now-rewritten) `PT_INTERP` path exactly as
   described above, opens *that* file as the interpreter, and transfers
   control to it with the original program's initial stack (`AT_PHDR`
   and `AT_ENTRY` still describe the original program, not the
   interpreter).
2. `core/native/ld-dn.c` is that file. It re-derives `$DN` by scanning the
   *original program's* already-mapped program headers (the kernel's
   `AT_PHDR` points at the executable, not the interpreter) for
   `PT_INTERP` again (`core/native/ld-dn.c:500`) and stripping the known
   `/usr/lib/deb-native/ld-dn` suffix. That string is exactly the field
   this doc patches, read back out of memory.
3. It then opens glibc's real loader (`ld-linux-aarch64.so.1`, never
   itself the kernel-facing `PT_INTERP` after translation), reads
   *its* `Ehdr`/`PT_LOAD` headers, maps those segments manually with
   `mmap`, and rewrites the `AT_BASE` auxv entry to point at that
   mapping before jumping to the real loader's entry point
   (`core/native/ld-dn.c:547-593`). `AT_BASE` is how a dynamic loader
   normally learns its own load address; `ld-dn` fakes that handoff
   so glibc's loader thinks it was invoked the normal way.

No second on-disk ELF edit happens here — this is all in-memory, once
per process launch.

## What deb-native does NOT touch

- **`DT_RPATH`/`DT_RUNPATH`.** Tried, then removed
  (`docs/log/findings/patchelf-et-exec-runpath.md`): a per-file rewrite
  doesn't help transitively anyway, and `ld-dn` setting
  `LD_LIBRARY_PATH` once per launch covers the whole load graph for
  free. No `.deb`'s dynamic section is edited today.
- **Code or instructions.** Nothing in the `.text` segment, or any
  other segment's bytes besides the relocated `PT_INTERP` string
  itself, is modified. The ELF's actual program logic is bit-for-bit
  what Debian shipped.
- **Section headers**, beyond what `patchelf`'s own `.interp`/program
  header sync requires. deb-native's own runtime code
  (`core/native/ld-dn.c`) never reads section headers at all — only program
  headers, which is all `execve()` itself needs too.
- **Libraries and static binaries.** `patchelf --print-interpreter`
  failing (no `PT_INTERP` segment) is the signal to skip a file
  entirely; nothing is patched on it.

## A self-brewed replacement: dn-elf

`TODO.md`'s "Runtime overhaul" section records the decision to replace
`patchelf` in this pipeline with `dn-elf`. The kernel reads `PT_INTERP`
from the file (`p_offset`), but glibc's loader names itself from the
address `p_vaddr` maps to (`_dl_rtld_libname.name = l_addr + p_vaddr`,
`elf/rtld.c`), and the run-time prefix is cut from that name
(`__dn_prefix_init`). The new string must land where `p_vaddr` points,
not merely at a new `p_offset`.

- `dn-elf get-interp FILE` — parse `Ehdr` (`e_phoff`/`e_phentsize`/
  `e_phnum`), scan program headers for `PT_INTERP`, print the string
  read from `p_offset`/`p_filesz`. Exit non-zero if there is no
  `PT_INTERP` entry (static binary or library).
- `dn-elf set-interp FILE NEWPATH` —
  1. Locate the existing `PT_INTERP` entry (fail if none: this tool is
     for rewriting an existing interpreter, not for adding one to a
     static binary, which is out of scope — `dn-translate-deb.sh`
     already skips those).
  2. If `NEWPATH` (plus NUL) fits within the old `p_filesz` **and** the
     entry is consistent — `p_offset` is the file offset `p_vaddr` maps
     to, via the `PT_LOAD` holding it — overwrite in place, padded with
     NUL to the old `p_filesz`. When the string already matches but
     `p_offset` does not map to `p_vaddr` (a file written by the older,
     buggy append), this is not treated as "done": it falls to step 3
     and is repaired.
  3. Otherwise: append the new NUL-terminated string at the end of the
     file, grow the `PT_LOAD` with the highest virtual end
     (`p_vaddr + p_memsz`) so those bytes are mapped, and set the
     `PT_INTERP` entry's `p_offset`, `p_vaddr` and `p_paddr` to where
     the string landed. This is the one job `patchelf`'s
     `rewriteSectionsLibrary` does for `.interp`, kept minimal: no new
     program header, no `e_phnum` change, no section added — only the
     highest existing `PT_LOAD`'s `p_filesz`/`p_memsz` grow, so nothing
     is overlapped (`p_memsz` is raised to `p_filesz` if the grow passes
     it).
  4. The `PT_LOAD`'s `p_align` still holds: it keeps `p_vaddr -
     p_offset` unchanged, so the appended offset maps to the address
     `p_vaddr` was set to.
- Scope: `ET_EXEC`/`ET_DYN`, `ELFCLASS64`, `ELFDATA2LSB`,
  `EM_AARCH64` only, matching every other assumption in this project's
  own native code (see "Invariants and safety" above) — reject
  anything else outright rather than silently mishandling it.
- `PN_XNUM` (`e_phnum == 0xffff`): reject outright with a clear error
  rather than silently truncating, since it is unverified territory
  (see above) and a Debian/glibc binary hitting it would be
  unprecedented in this project's experience so far.

This removes the third-party dependency from the translate step: `dn-elf`
does the one edit this project needs — rewrite `PT_INTERP`, growing one
`PT_LOAD` when the path is longer — instead of `patchelf`'s
general-purpose section machinery (and its `ET_EXEC`/`RUNPATH` failure
mode, `patchelf-et-exec-runpath.md`).
