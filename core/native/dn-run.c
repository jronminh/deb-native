/* dn-run -- runtime launch dispatcher for a deb-native prefix program.
 *
 * The per-program wrapper (the post hook (core/install/dn-hook-post.sh)) calls:
 *   dn-run REAL [args...]
 * and this decides, at launch, which mechanism REAL needs:
 *
 *   glibc      -> LD_PRELOAD the dn-shim shim (+ DN_INSTDIR, PATH), exec
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
#include <sys/wait.h>
#include <sys/syscall.h>
#include <errno.h>
#include "dn-child.h"

static char instdir[4096];

/* Does PATH exist as an executable on the device itself? dn-run is a glibc
 * program, so the prefix's shim is loaded into it (ld.so.preload) and
 * interposes access()/faccessat(): a guest-shaped path such as
 * /lib/ld-linux-aarch64.so.1 would be rewritten into the prefix, where the
 * loader does exist, and a missing interpreter would look present. The raw
 * syscall is not interposed, so it answers for the real path (the same trap
 * the shim avoids with real_access_ok()). */
static int real_access_x(const char *path) {
  return syscall(SYS_faccessat, AT_FDCWD, path, X_OK, 0) == 0;
}
static int g_glibc;   /* the target is a glibc ELF (classify) */

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

static int classify(const char *path, int *nss, char *interp, size_t isz) {
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
    if (interp && isz) snprintf(interp, isz, "%s", in);
    /* Every binary this project translates has PT_INTERP rewritten to the
     * prefix's own fused glibc loader (dn-translate-deb.sh,
     * patchelf --set-interpreter), an "ld-linux" path -- caught by the
     * ld-linux match below. A glibc binary that slipped through to
     * C_DYNOTHER would get a bare execv() with no NSS check, silently
     * skipping the tracer routing this function exists for. Same
     * classification as dn-shim.c's target_is_glibc() (runtime
     * component audit, 2026-09-30). */
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
  char path[8192];
  dn_build_path(instdir, path, sizeof path);
  setenv("PATH", path, 1);
}

static void die(const char *what) {
  fprintf(stderr, "dn-run: %s: %s\n", what, strerror(errno));
  _exit(127);
}

/* Lazy adopt for a glibc binary whose PT_INTERP is a loader that is not on
 * this device (/lib/ld-linux-aarch64.so.1): rewrite the interpreter once to
 * the prefix's fused loader, so the kernel can start it and /proc/self/exe
 * stays the program (Bun/Node SEA safe). The prefix's own dn-elf does the
 * rewrite, in place or by growing a PT_LOAD to map a longer path (the same
 * editor the translator uses), so no patchelf is needed inside the prefix.
 * Returns 0 on success, -1 if dn-elf is missing or the rewrite failed
 * (read-only file, a layout dn-elf refuses) -- the caller then falls back to
 * the tracer. */
static int try_adopt(const char *path) {
  char elf[4096], ld[4096];
  snprintf(elf, sizeof elf, "%s/usr/lib/deb-native/dn-elf", instdir);
  if (access(elf, X_OK) != 0) return -1;
  snprintf(ld, sizeof ld,
           "%s/usr/lib/aarch64-linux-gnu/ld-linux-aarch64.so.1", instdir);
  set_path();
  setenv("DN_INSTDIR", instdir, 1);
  pid_t pid = fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    execl(elf, elf, "set-interp", path, ld, (char *)NULL);
    _exit(127);
  }
  int st = 0;
  if (waitpid(pid, &st, 0) < 0) return -1;
  return (WIFEXITED(st) && WEXITSTATUS(st) == 0) ? 0 : -1;
}

static void launch_glibc(char **args) {
  /* Fused loader (dn-glibc): the dn-shim shim is delivered by
   * <prefix>/etc/ld.so.preload and the loader ignores LD_PRELOAD
   * (docs/spec/dn-glibc-prefix.md), so do not inject it here -- just drop any
   * inherited preload. */
  unsetenv("LD_PRELOAD");
  setenv("DN_INSTDIR", instdir, 1);
  set_path();
  execv(args[0], args);
  die("execv");
}

/* Route #2: syscall-level rewrite. deb-native's tracer, `dn-trace` (tracer/:
 * a ptrace tracer grown out of PRoot's core, cut down to what the prefix
 * needs), maps the guest /usr,/etc,... onto the prefix for the whole traced
 * tree, which is why it reaches static binaries, raw syscalls, and the
 * libc-internal NSS reads the libc shim cannot. There is no fallback to
 * Termux's proot: the prefix's own loader, the shim and dn-trace cover the
 * prefix. Only dirs that exist are bound.
 *
 * The prefix's own fused glibc self-derives its sysconfdir, so its NSS
 * reads (/etc/passwd, /etc/hosts, ...) already resolve inside the prefix
 * through the /etc bind below -- no Termux glibc sysconfdir bind is needed
 * (0.7.0 removed it). */
static void launch_trace(char **args, int nss) {
  char tracer[4096];
  const char *e = getenv("DN_TRACE");
  struct stat st;
  (void)nss;   /* the prefix's own glibc self-derives NSS; no extra bind */

  if (e && *e)
    snprintf(tracer, sizeof tracer, "%s", e);
  else
    snprintf(tracer, sizeof tracer, "%s/usr/lib/deb-native/dn-trace", instdir);
  if (stat(tracer, &st) != 0) {
    /* No dn-trace (built at install when make and libtalloc are present).
     * A glibc program still takes the shim route (clean preload, prefix
     * PATH) -- raw, it would inherit termux-exec's Bionic preload and fail
     * to load; only its NSS reads or raw syscalls stay unredirected. Any
     * other program runs untranslated, and says so. */
    if (g_glibc) launch_glibc(args);
    fprintf(stderr, "dn-run: %s needs dn-trace, which is not built "
            "(pkg install make libtalloc, then re-run the installer); "
            "running it untranslated; it may fail\n", args[0]);
    execv(args[0], args);
    die("execv");
  }

  set_path();
  setenv("DN_INSTDIR", instdir, 1);
  /* The tracer builds its glue rootfs under its own temp directory (P_tmpdir
   * is /tmp, which is not writable on Android). Point it at the prefix's own
   * tmp, which the binds below also map to the tracee's /tmp. */
  {
    char tmpd[4096];
    snprintf(tmpd, sizeof tmpd, "%s/tmp", instdir);
    mkdir(tmpd, 0777);
    setenv("PROOT_TMP_DIR", tmpd, 1);
    setenv("TMPDIR", tmpd, 1);
  }
  /* No tracee inherits termux-exec or our shim: the tracer rewrites at the
   * syscall layer. termux-exec would rewrite a Bionic child's
   * execve("/usr/...") to $PREFIX/... before the tracer sees it, and for a
   * glibc tracee a Bionic preload is the wrong libc. */
  unsetenv("LD_PRELOAD");
  unsetenv("DN_BIONIC_PRELOAD");

  static char *pargv[4096];
  static char binds[10][8192];
  const char *dirs[] = { "usr", "etc", "var", "opt", "bin", "sbin", "tmp", "run", NULL };
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
  else if (dn_derive_instdir(instdir, sizeof instdir) != 0) {
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
  char in[512] = {0};
  int cls = classify(args[0], &nss, in, sizeof in);
  g_glibc = (cls == C_GLIBC);

  if (force_trace)
    launch_trace(args, nss);

  /* Lazy adopt: a glibc binary whose interpreter is not on this device was
   * installed outside apt (a vendor installer, a tarball, ...) and never
   * translated. Rewrite it once to the prefix's fused loader and run it
   * natively; if that is impossible (no dn-elf, read-only), fall through
   * to the tracer route. Foreign binaries become case N of the same door. */
  if (cls == C_GLIBC && in[0] && !real_access_x(in)) {
    if (try_adopt(args[0]) == 0) {
      fprintf(stderr,
              "dn-run: adopted %s (interpreter %s -> prefix loader)\n",
              args[0], in);
    } else {
      fprintf(stderr,
              "dn-run: %s: interpreter %s missing and adoption failed; "
              "using the tracer\n", args[0], in);
      launch_trace(args, nss);
    }
  }

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
