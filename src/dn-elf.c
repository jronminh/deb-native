/* dn-elf -- read or rewrite the ELF interpreter (PT_INTERP) of a program.
 *
 *   dn-elf get-interp FILE
 *   dn-elf set-interp FILE NEWPATH [CAPACITY]
 *
 * The kernel reads PT_INTERP from the file (p_offset, p_filesz) at execve(),
 * but glibc's own loader names itself from the memory PT_INTERP maps to:
 * _dl_rtld_libname.name = main_map->l_addr + ph->p_vaddr (elf/rtld.c), and the
 * run-time prefix is cut from that name (__dn_prefix_init). So a new string is
 * written either in place, NUL-padded within the old p_filesz, or -- when it is
 * longer -- into room made for it: the string is appended and the PT_LOAD with
 * the highest virtual end is grown to map it, PT_INTERP's p_vaddr set to where
 * it landed. This is the same job patchelf's rewriteSectionsLibrary does for
 * .interp (see docs/reference/elf-interp-patch.md); an append that moves only
 * p_offset, leaving p_vaddr at the stale bytes, is never done -- the loader
 * would read those and lose the prefix.
 *
 * Scope: ELF64, little-endian, EM_AARCH64, ET_EXEC or ET_DYN; anything else is
 * refused. Exit 0 on success (set-interp is idempotent), 1 on any error.
 */
#include <elf.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int die(const char *what, const char *file) {
  fprintf(stderr, "dn-elf: %s: %s\n", what, file);
  return 1;
}

/* Read the ELF header and check it is a file this tool may edit. */
static int read_ehdr(FILE *f, Elf64_Ehdr *e, const char *file) {
  if (fseek(f, 0, SEEK_SET) != 0 || fread(e, sizeof *e, 1, f) != 1)
    return die("not an ELF file (too short)", file);
  if (memcmp(e->e_ident, ELFMAG, SELFMAG) != 0) return die("not an ELF file", file);
  if (e->e_ident[EI_CLASS] != ELFCLASS64) return die("not ELF64", file);
  if (e->e_ident[EI_DATA] != ELFDATA2LSB) return die("not little-endian", file);
  if (e->e_machine != EM_AARCH64) return die("not aarch64", file);
  if (e->e_type != ET_EXEC && e->e_type != ET_DYN) return die("not an executable or shared object", file);
  if (e->e_phentsize != sizeof(Elf64_Phdr)) return die("unexpected program header size", file);
  return 0;
}

/* Find the PT_INTERP entry: its index in the table and its contents. */
static int find_interp(FILE *f, const Elf64_Ehdr *e, Elf64_Phdr *ph, long *idx, const char *file) {
  for (long i = 0; i < e->e_phnum; i++) {
    if (fseek(f, (long)(e->e_phoff + i * sizeof *ph), SEEK_SET) != 0 ||
        fread(ph, sizeof *ph, 1, f) != 1)
      return die("cannot read program headers", file);
    if (ph->p_type == PT_INTERP) {
      *idx = i;
      return 0;
    }
  }
  return die("no PT_INTERP (static or not a program)", file);
}

/* File offset the given virtual address maps to, from the PT_LOAD holding it.
   PT_INTERP's string is read by the loader at p_vaddr, so "in place" is only
   safe when p_offset is that address's file offset. */
static int vaddr_to_offset(FILE *f, const Elf64_Ehdr *e, Elf64_Addr vaddr, Elf64_Off *off, const char *file) {
  Elf64_Phdr ph;
  for (long i = 0; i < e->e_phnum; i++) {
    if (fseek(f, (long)(e->e_phoff + i * sizeof ph), SEEK_SET) != 0 ||
        fread(&ph, sizeof ph, 1, f) != 1)
      return die("cannot read program headers", file);
    if (ph.p_type == PT_LOAD && vaddr >= ph.p_vaddr && vaddr < ph.p_vaddr + ph.p_memsz) {
      *off = ph.p_offset + (vaddr - ph.p_vaddr);
      return 0;
    }
  }
  return -1;
}

/* Read the current interpreter string into buf (NUL-terminated). */
static int read_string(FILE *f, const Elf64_Phdr *ph, char *buf, size_t bufsz, const char *file) {
  if (ph->p_filesz == 0 || ph->p_filesz > bufsz) return die("PT_INTERP has an unusable size", file);
  if (fseek(f, (long)ph->p_offset, SEEK_SET) != 0 || fread(buf, ph->p_filesz, 1, f) != 1)
    return die("cannot read PT_INTERP", file);
  if (memchr(buf, '\0', ph->p_filesz) == NULL) return die("PT_INTERP is not NUL-terminated", file);
  return 0;
}

static int cmd_get(const char *file) {
  FILE *f = fopen(file, "rb");
  if (!f) return die(strerror(errno), file);
  Elf64_Ehdr e;
  Elf64_Phdr ph;
  char buf[4096];
  long idx;
  int rc = read_ehdr(f, &e, file) || find_interp(f, &e, &ph, &idx, file) ||
           read_string(f, &ph, buf, sizeof buf, file);
  fclose(f);
  if (rc) return rc;
  printf("%s\n", buf);
  return 0;
}

static int cmd_set(const char *file, const char *newpath, long capacity) {
  size_t len = strlen(newpath);
  if (len == 0 || newpath[0] != '/') return die("the new interpreter must be an absolute path", file);
  if (len + 1 > 4096) return die("the new interpreter is too long", file);
  /* WANT is the size PT_INTERP ends up with. CAPACITY reserves room for a
     later, longer path (the build's 256-byte invariant) in the same write --
     a separate reserve would grow twice and leave two copies of the build
     path in the file. */
  size_t want = len + 1;
  if (capacity > 0 && (size_t)capacity > want) want = (size_t)capacity;

  FILE *f = fopen(file, "r+b");
  if (!f) return die(strerror(errno), file);
  Elf64_Ehdr e;
  Elf64_Phdr ph;
  char cur[4096];
  long idx;
  int rc = read_ehdr(f, &e, file) || find_interp(f, &e, &ph, &idx, file) ||
           read_string(f, &ph, cur, sizeof cur, file);
  if (rc) { fclose(f); return rc; }

  Elf64_Off mapped;
  int consistent = (vaddr_to_offset(f, &e, ph.p_vaddr, &mapped, file) == 0 &&
                    mapped == ph.p_offset);
  if (consistent && strcmp(cur, newpath) == 0 && ph.p_filesz >= want) {
    fclose(f); return 0;   /* already done */
  }

  if (consistent && want <= ph.p_filesz) {
    /* Fits: overwrite in place, NUL-padded to the old size. */
    char *pad = calloc(1, ph.p_filesz);
    if (!pad) { fclose(f); return die("out of memory", file); }
    memcpy(pad, newpath, len + 1);
    rc = (fseek(f, (long)ph.p_offset, SEEK_SET) != 0 ||
          fwrite(pad, ph.p_filesz, 1, f) != 1) ? die("cannot write PT_INTERP", file) : 0;
    free(pad);
  } else {
    /* Make room by adding a new PT_LOAD that carries the string (and a
       relocated program-header table) above every existing segment. Growing
       an existing segment is unsafe: a segment with a BSS (p_memsz >
       p_filesz) would turn its zero-fill into file bytes, and the loader
       reads its own name from the memory PT_INTERP maps (elf/rtld.c), so the
       string must be mapped. */
    long n = e.e_phnum;
    Elf64_Phdr phs[128];
    if (n + 1 > (long)(sizeof phs / sizeof phs[0]))
      { fclose(f); return die("too many program headers", file); }
    for (long i = 0; i < n; i++)
      if (fseek(f, (long)(e.e_phoff + i * sizeof(Elf64_Phdr)), SEEK_SET) != 0 ||
          fread(&phs[i], sizeof(Elf64_Phdr), 1, f) != 1)
        { fclose(f); return die("cannot read program headers", file); }
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return die("cannot seek to the end", file); }
    long end = ftell(f);
    if (end < 0) { fclose(f); return die("cannot size the file", file); }

    /* A free vaddr for the new segment: page-aligned above every segment. */
    Elf64_Addr newV = 0;
    for (long i = 0; i < n; i++)
      if (phs[i].p_type == PT_LOAD) {
        Elf64_Addr segend = phs[i].p_vaddr + phs[i].p_memsz;
        if (segend > newV) newV = segend;
      }
    newV = (newV + 0xfff) & ~(Elf64_Addr)0xfff;

    /* The new segment must be page-congruent: p_offset and p_vaddr share the
       same page remainder (glibc: "address/offset not page-aligned"). */
    Elf64_Off string_off = ((Elf64_Off)end + 0xfff) & ~(Elf64_Off)0xfff;
    long new_n = n + 1;
    size_t phsz = sizeof(Elf64_Phdr);
    Elf64_Off phdr_off = string_off + (Elf64_Off)want;
    Elf64_Off seg_sz = (Elf64_Off)want + (Elf64_Off)new_n * (Elf64_Off)phsz;

    /* The new read-only LOAD covers the string and the relocated table. */
    Elf64_Phdr nl;
    memset(&nl, 0, sizeof nl);
    nl.p_type = PT_LOAD;
    nl.p_flags = PF_R;
    nl.p_offset = string_off;
    nl.p_vaddr = nl.p_paddr = newV;
    nl.p_filesz = nl.p_memsz = seg_sz;
    nl.p_align = 0x1000;

    /* PT_PHDR must describe the relocated table (the kernel reads AT_PHDR
       from it; without PT_PHDR the kernel finds it in the new LOAD). */
    for (long i = 0; i < n; i++)
      if (phs[i].p_type == PT_PHDR) {
        phs[i].p_offset = phdr_off;
        phs[i].p_vaddr = phs[i].p_paddr = newV + (Elf64_Addr)want;
        phs[i].p_filesz = phs[i].p_memsz =
          (Elf64_Xword)new_n * (Elf64_Xword)phsz;
        phs[i].p_align = 0x1000;
      }
    ph.p_offset = string_off;
    ph.p_vaddr = ph.p_paddr = newV;
    ph.p_filesz = ph.p_memsz = want;
    phs[idx] = ph;
    phs[n] = nl;

    size_t pad = (size_t)(string_off - (Elf64_Off)end);
    char *buf = calloc(1, pad + (size_t)seg_sz);
    if (!buf) { fclose(f); return die("out of memory", file); }
    memcpy(buf + pad, newpath, len + 1);
    memcpy(buf + pad + want, phs, (size_t)new_n * phsz);
    rc = (fseek(f, (long)end, SEEK_SET) != 0 ||
          fwrite(buf, pad + (size_t)seg_sz, 1, f) != 1)
         ? die("cannot write the new segment", file) : 0;
    free(buf);
    if (rc == 0) {
      e.e_phoff = phdr_off;
      e.e_phnum = (Elf64_Half)new_n;
      rc = (fseek(f, 0, SEEK_SET) != 0 || fwrite(&e, sizeof e, 1, f) != 1)
           ? die("cannot update the ELF header", file) : 0;
    }
  }
  if (fclose(f) != 0 && rc == 0) rc = die("cannot close the file", file);
  return rc;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "get-interp") == 0) return cmd_get(argv[2]);
  if ((argc == 4 || argc == 5) && strcmp(argv[1], "set-interp") == 0)
    return cmd_set(argv[2], argv[3], argc == 5 ? atol(argv[4]) : 0);
  fprintf(stderr, "usage: dn-elf get-interp FILE\n"
                  "       dn-elf set-interp FILE NEWPATH [CAPACITY]\n");
  return 1;
}
