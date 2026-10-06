/* dn-elf -- read or rewrite the ELF interpreter (PT_INTERP) of a program.
 *
 *   dn-elf get-interp FILE
 *   dn-elf set-interp FILE NEWPATH
 *
 * The kernel reads PT_INTERP from the file (p_offset, p_filesz) at execve(),
 * and never maps it. So changing the string changes no other byte of the ELF:
 * a path that fits the old string is written in place (padded with NULs to the
 * old p_filesz); a longer one is appended at the end of the file, and only the
 * PT_INTERP program header entry is pointed at it (p_offset, p_filesz, p_memsz).
 * No segment, no section and no other header changes -- which is why this is
 * not a general ELF editor and never grows a PT_LOAD. See
 * docs/reference/elf-interp-patch.md.
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

static int cmd_set(const char *file, const char *newpath) {
  size_t len = strlen(newpath);
  if (len == 0 || newpath[0] != '/') return die("the new interpreter must be an absolute path", file);
  if (len + 1 > 4096) return die("the new interpreter is too long", file);

  FILE *f = fopen(file, "r+b");
  if (!f) return die(strerror(errno), file);
  Elf64_Ehdr e;
  Elf64_Phdr ph;
  char cur[4096];
  long idx;
  int rc = read_ehdr(f, &e, file) || find_interp(f, &e, &ph, &idx, file) ||
           read_string(f, &ph, cur, sizeof cur, file);
  if (rc) { fclose(f); return rc; }
  if (strcmp(cur, newpath) == 0) { fclose(f); return 0; }   /* already done */

  Elf64_Off ph_at = e.e_phoff + (Elf64_Off)idx * sizeof ph;
  if (len + 1 <= ph.p_filesz) {
    /* Fits: overwrite in place, NUL-padded to the old size. */
    char *pad = calloc(1, ph.p_filesz);
    if (!pad) { fclose(f); return die("out of memory", file); }
    memcpy(pad, newpath, len + 1);
    rc = (fseek(f, (long)ph.p_offset, SEEK_SET) != 0 ||
          fwrite(pad, ph.p_filesz, 1, f) != 1) ? die("cannot write PT_INTERP", file) : 0;
    free(pad);
  } else {
    /* Longer: append the string at the end and point PT_INTERP at it. */
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return die("cannot seek to the end", file); }
    long end = ftell(f);
    if (end < 0) { fclose(f); return die("cannot size the file", file); }
    if (fwrite(newpath, len + 1, 1, f) != 1) { fclose(f); return die("cannot append", file); }
    ph.p_offset = (Elf64_Off)end;
    ph.p_filesz = len + 1;
    ph.p_memsz = len + 1;
    rc = (fseek(f, (long)ph_at, SEEK_SET) != 0 ||
          fwrite(&ph, sizeof ph, 1, f) != 1) ? die("cannot update the PT_INTERP header", file) : 0;
  }
  if (fclose(f) != 0 && rc == 0) rc = die("cannot close the file", file);
  return rc;
}

int main(int argc, char **argv) {
  if (argc == 3 && strcmp(argv[1], "get-interp") == 0) return cmd_get(argv[2]);
  if (argc == 4 && strcmp(argv[1], "set-interp") == 0) return cmd_set(argv[2], argv[3]);
  fprintf(stderr, "usage: dn-elf get-interp FILE\n"
                  "       dn-elf set-interp FILE NEWPATH\n");
  return 1;
}
