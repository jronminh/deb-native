/* dn-run -- runtime launch dispatcher for a deb-native prefix program.
 *
 * The per-program wrapper (scripts/make-launchers.sh) calls:
 *   dn-run REAL [args...]
 * and this decides, at launch, which mechanism REAL needs:
 *
 *   glibc      -> LD_PRELOAD the path-redirect shim (+ DN_INSTDIR, PATH), exec
 *   glibc + NSS -> syscall tracer: statically-bound libc NSS reads are
 *             invisible to the shim, and Termux glibc's sysconfdir is a host
 *             path, so bind $INSTDIR/etc over it
 *   bionic     -> exec untouched (Termux's own libc; keep termux-exec preload)
 *   static     -> syscall tracer `-b <host>:<guest>` -- the only layer that
 *             sees a static binary or a raw syscall() (route #2)
 *   other      -> exec untouched
 *
 * Classification is done in-process by reading REAL's ELF PT_INTERP, so a
 * launch costs no extra fork beyond this dispatcher itself. INSTDIR comes
 * from DN_INSTDIR, else is derived from this binary's own path
 * ($INSTDIR/usr/lib/deb-native/dn-run -> up three dirs).
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <elf.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>

static char instdir[4096];

static const char *termux_prefix(void) {
  const char *p = getenv("DN_TERMUX_PREFIX");
  if (!p || !*p) p = getenv("PREFIX");
  if (!p || !*p) p = "/data/data/com.termux/files/usr";
  return p;
}

/* .../usr/lib/deb-native/dn-run -> ... (strip filename + 3 dirs) */
static int derive_instdir(char *out, size_t sz) {
  char self[4096];
  ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
  if (n <= 0) return -1;
  self[n] = '\0';
  char *s = strrchr(self, '/');
  if (!s || s == self) return -1;
  *s = '\0';
  for (int i = 0; i < 3; i++) {
    s = strrchr(self, '/');
    if (!s || s == self) return -1;
    *s = '\0';
  }
  snprintf(out, sz, "%s", self);
  return 0;
}

enum { C_NOTELF, C_GLIBC, C_BIONIC, C_DYNOTHER, C_STATIC };

/* Does the ELF import an NSS entry point (getpwnam, getaddrinfo, ...)?  Those
 * lookups go through statically-bound libc symbols the LD_PRELOAD shim cannot
 * reach, so such a binary must run under the syscall tracer.  The scan is a
 * conservative raw string search: a false positive only costs tracer overhead,
 * never correctness. */
static int has_nss_import(int fd) {
  static const char *names[] = {
    "getpwnam", "getpwuid", "getgrnam", "getgrgid", "getspnam", "getspent",
    "getaddrinfo", "gethostbyname", "gethostbyaddr", "getservbyname",
    "getservbyport", "getnetbyname", "getprotobyname", "initgroups",
    "getaliasbyname", "gethostent", NULL
  };
  struct stat st;
  char *buf;
  ssize_t off = 0, r;
  int i, found = 0;

  if (fstat(fd, &st) != 0 || st.st_size <= 0 || st.st_size > 64 * 1024 * 1024)
    return 0;
  buf = malloc((size_t)st.st_size);
  if (!buf)
    return 0;
  while (off < st.st_size &&
         (r = pread(fd, buf + off, (size_t)(st.st_size - off), off)) > 0)
    off += r;
  if (off == st.st_size) {
    for (i = 0; names[i]; i++) {
      if (memmem(buf, (size_t)st.st_size, names[i], strlen(names[i])) != NULL) {
        found = 1;
        break;
      }
    }
  }
  free(buf);
  return found;
}

static int classify(const char *path, int *nss) {
  int fd = open(path, O_RDONLY | O_CLOEXEC);
  if (fd < 0) return C_NOTELF;
  Elf64_Ehdr eh;
  if (pread(fd, &eh, sizeof eh, 0) != (ssize_t)sizeof eh ||
      eh.e_ident[0] != 0x7f || eh.e_ident[1] != 'E' ||
      eh.e_ident[2] != 'L' || eh.e_ident[3] != 'F' ||
      eh.e_phentsize != sizeof(Elf64_Phdr)) {
    close(fd);
    return C_NOTELF;
  }
  int phnum = eh.e_phnum > 128 ? 128 : eh.e_phnum;
  for (int i = 0; i < phnum; i++) {
    Elf64_Phdr ph;
    if (pread(fd, &ph, sizeof ph, eh.e_phoff + (off_t)i * sizeof ph) != (ssize_t)sizeof ph)
      continue;
    if (ph.p_type != PT_INTERP || ph.p_filesz == 0 || ph.p_filesz >= 511) continue;
    char in[512];
    if (pread(fd, in, ph.p_filesz, ph.p_offset) != (ssize_t)ph.p_filesz) continue;
    in[ph.p_filesz] = '\0';
    if (strstr(in, "ld-linux")) {
      *nss = has_nss_import(fd);
      close(fd);
      return C_GLIBC;
    }
    if (strstr(in, "linker")) {
      close(fd);
      return C_BIONIC;
    }
    close(fd);
    return C_DYNOTHER;
  }
  close(fd);
  return C_STATIC;
}

static void set_path(void) {
  const char *p = termux_prefix();
  char path[8192];
  snprintf(path, sizeof path,
           "%s/usr/sbin:%s/usr/bin:%s/sbin:%s/bin:%s/usr/games:"
           "%s/usr/lib/deb-native/bin:%s/glibc/bin:%s/bin",
           instdir, instdir, instdir, instdir, instdir, instdir, p, p);
  setenv("PATH", path, 1);
}

static void die(const char *what) {
  fprintf(stderr, "dn-run: %s: %s\n", what, strerror(errno));
  _exit(127);
}

static void launch_glibc(char **args) {
  /* Preserve whatever preload we inherited (on Termux, termux-exec) so the
   * shim can hand it back to a Bionic child it execs; never stash our own
   * shim. Capture before overwriting LD_PRELOAD. */
  const char *inh = getenv("LD_PRELOAD");
  if (inh && *inh && !strstr(inh, "path-redirect.so"))
    setenv("DN_BIONIC_PRELOAD", inh, 1);
  char shim[4096];
  snprintf(shim, sizeof shim, "%s/usr/lib/deb-native/path-redirect.so", instdir);
  setenv("LD_PRELOAD", shim, 1);
  setenv("DN_INSTDIR", instdir, 1);
  set_path();
  execv(args[0], args);
  die("execv");
}

/* Route #2: syscall-level rewrite. A tracer (fork-lite `dn-trace`, else
 * Termux `proot`) maps the guest /usr,/etc,... onto the prefix for the whole
 * traced tree, which is why it reaches static binaries, raw syscalls, and the
 * libc-internal NSS reads the libc shim cannot. Only dirs that exist are bound
 * -- proot errors on a missing host path.
 *
 * nss=1 adds one more bind: Termux's glibc reads its sysconfdir at
 * $PREFIX/glibc/etc (a host path outside the prefix), so NSS reads
 * (/etc/passwd, /etc/hosts, ...) never hit the guest /etc. Bind the prefix's
 * /etc over it so those lookups resolve in the prefix. */
static void launch_trace(char **args, int nss) {
  char tracer[4096];
  const char *e = getenv("DN_TRACE");
  struct stat st;

  if (e && *e)
    snprintf(tracer, sizeof tracer, "%s", e);
  else
    snprintf(tracer, sizeof tracer, "%s/usr/lib/deb-native/dn-trace", instdir);
  if (stat(tracer, &st) != 0) {
    snprintf(tracer, sizeof tracer, "%s/bin/proot", termux_prefix());
    if (stat(tracer, &st) != 0) {
      fprintf(stderr, "dn-run: no tracer (dn-trace/proot); running unredirected\n");
      execv(args[0], args);
      die("execv");
    }
  }

  set_path();
  setenv("DN_INSTDIR", instdir, 1);
  /* A glibc tracee must not inherit termux-exec/our shim: the tracer rewrites
   * at the syscall layer; a Bionic preload would be the wrong libc. */
  if (nss)
    unsetenv("LD_PRELOAD");

  static char *pargv[4096];
  static char binds[10][8192];
  const char *dirs[] = { "usr", "etc", "var", "opt", "bin", "sbin", NULL };
  int n = 0;
  pargv[n++] = tracer;
  for (int i = 0; dirs[i] && n < 4080; i++) {
    char host[4096];
    snprintf(host, sizeof host, "%s/%s", instdir, dirs[i]);
    if (stat(host, &st) != 0) continue;
    snprintf(binds[i], sizeof binds[i], "%s/%s:/%s", instdir, dirs[i], dirs[i]);
    pargv[n++] = (char *)"-b";
    pargv[n++] = binds[i];
  }
  if (nss && n < 4078) {
    static char getc_bind[8192];
    char host_etc[4096];
    snprintf(host_etc, sizeof host_etc, "%s/etc", instdir);
    if (stat(host_etc, &st) == 0) {
      snprintf(getc_bind, sizeof getc_bind, "%s/etc:%s/glibc/etc",
               instdir, termux_prefix());
      pargv[n++] = (char *)"-b";
      pargv[n++] = getc_bind;
    }
  }
  int ac = 0;
  while (args[ac]) ac++;
  for (int i = 0; i < ac && n < 4090; i++) pargv[n++] = args[i];
  pargv[n] = NULL;
  execv(tracer, pargv);
  die("execv tracer");
}

int main(int argc, char **argv) {
  if (argc < 2) {
    fprintf(stderr, "usage: dn-run [--trace] REAL [args...]\n");
    return 2;
  }
  const char *e = getenv("DN_INSTDIR");
  if (e && *e) snprintf(instdir, sizeof instdir, "%s", e);
  else if (derive_instdir(instdir, sizeof instdir) != 0) {
    fprintf(stderr, "dn-run: cannot derive INSTDIR (set DN_INSTDIR)\n");
    return 127;
  }

  /* --trace: forced by the launcher for a binary that issues its own syscalls
   * (inline `svc` or a `syscall()` import), which the shim cannot see. */
  int argi = 1;
  int force_trace = 0;
  if (strcmp(argv[1], "--trace") == 0) {
    force_trace = 1;
    argi = 2;
    if (argc < 3) {
      fprintf(stderr, "usage: dn-run [--trace] REAL [args...]\n");
      return 2;
    }
  }

  char **args = &argv[argi];
  int nss = 0;
  int cls = classify(args[0], &nss);

  if (force_trace)
    launch_trace(args, nss);

  switch (cls) {
    case C_GLIBC:
      if (nss) launch_trace(args, 1);
      else     launch_glibc(args);
      break;
    case C_STATIC: launch_trace(args, 0); break;
    default:       execv(args[0], args); die("execv"); break;
  }
  return 127;
}
