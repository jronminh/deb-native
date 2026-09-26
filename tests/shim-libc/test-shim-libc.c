#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <sys/statvfs.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/xattr.h>
#include <utime.h>
#include <spawn.h>
#include <sys/inotify.h>
#include <sys/wait.h>
#include <errno.h>
#include <limits.h>

extern char **environ;

static void note(const char *n) { fprintf(stderr, "TEST %s\n", n); }

int main(void) {
  char rb[PATH_MAX];
  char *r;
  int fd;
  FILE *f;

  note("creat");      fd = creat("/etc/zz_creat", 0644); if (fd >= 0) close(fd);
  note("creat64");    fd = creat64("/etc/zz_creat64", 0644); if (fd >= 0) close(fd);
  note("freopen");    f = fopen("/etc/zz_freopen", "w"); if (f) { freopen("/etc/zz_freopen", "w", f); fclose(f); }
  note("chown");      chown("/etc/zz_chown", getuid(), getgid());
  note("lchown");     lchown("/etc/zz_lchown", getuid(), getgid());
  note("fchownat");   fchownat(AT_FDCWD, "/etc/zz_fchownat", getuid(), getgid(), 0);
  note("utime");      utime("/etc/zz_utime", NULL);
  note("setxattr");   setxattr("/etc/zz_setxattr", "user.a", "1", 1, 0);
  note("lsetxattr");  lsetxattr("/etc/zz_lsetxattr", "user.a", "1", 1, 0);
  note("getxattr");   getxattr("/etc/zz_getxattr", "user.a", rb, 4);
  note("lgetxattr");  lgetxattr("/etc/zz_lgetxattr", "user.a", rb, 4);
  note("listxattr");  listxattr("/etc/zz_listxattr", rb, 4);
  note("llistxattr"); llistxattr("/etc/zz_llistxattr", rb, 4);
  note("removexattr"); removexattr("/etc/zz_removexattr", "user.a");
  note("lremovexattr"); lremovexattr("/etc/zz_lremovexattr", "user.a");
  note("mkfifo");     mkfifo("/etc/zz_mkfifo", 0644);
  note("mkfifoat");   mkfifoat(AT_FDCWD, "/etc/zz_mkfifoat", 0644);
  note("mknod");      mknod("/etc/zz_mknod", S_IFIFO | 0644, 0);
  note("mknodat");    mknodat(AT_FDCWD, "/etc/zz_mknodat", S_IFIFO | 0644, 0);
  note("statfs64");   { struct statfs64 s; statfs64("/etc/zz_statfs64", &s); }
  note("statvfs64");  { struct statvfs64 s; statvfs64("/etc/zz_statvfs64", &s); }
  note("realpath");   r = realpath("/etc/zz_realpath", rb); (void)r;
  note("canon");      r = canonicalize_file_name("/etc/zz_canon"); free(r);
  note("inotify");    { int wd = inotify_init1(IN_CLOEXEC); if (wd >= 0) { inotify_add_watch(wd, "/etc/zz_inotify", IN_ALL_EVENTS); close(wd); } }
  note("sendto");     { int sd = socket(AF_UNIX, SOCK_DGRAM, 0); struct sockaddr_un u; memset(&u, 0, sizeof u); u.sun_family = AF_UNIX; strcpy(u.sun_path, "/etc/zz_sendto"); if (sd >= 0) { sendto(sd, "x", 1, 0, (struct sockaddr *)&u, sizeof u); close(sd); } }

  note("mkstemp");    { char t[] = "/etc/zz_mkstempXXXXXX"; fd = mkstemp(t); if (fd >= 0) { fprintf(stderr, "MKSTEMP_TMPL=%s\n", t); close(fd); } else fprintf(stderr, "MKSTEMP_ERR\n"); }
  note("mkostemp");   { char t[] = "/etc/zz_mkostempXXXXXX"; fd = mkostemp(t, O_CLOEXEC); if (fd >= 0) { fprintf(stderr, "MKOSTEMP_TMPL=%s\n", t); close(fd); } else fprintf(stderr, "MKOSTEMP_ERR\n"); }
  note("mkdtemp");    { char t[] = "/etc/zz_mkdtempXXXXXX"; r = mkdtemp(t); if (r) fprintf(stderr, "MKDTEMP_TMPL=%s\n", t); else fprintf(stderr, "MKDTEMP_ERR\n"); }

  note("sentinel_open"); { struct stat st; stat("/etc/real.txt", &st); }

  note("posix_spawn");  { pid_t p; char *av[] = {"/etc/zz_posix_spawn", NULL}; char *ev[] = {NULL}; int rc = posix_spawn(&p, "/etc/zz_posix_spawn", NULL, NULL, av, ev); fprintf(stderr, "POSIX_SPAWN_RC=%d\n", rc); }
  note("posix_spawnp"); { pid_t p; char *av[] = {"zz_nope", NULL}; char *ev[] = {NULL}; int rc = posix_spawnp(&p, "zz_nope", NULL, NULL, av, ev); fprintf(stderr, "POSIX_SPAWNP_RC=%d\n", rc); }
  note("posix_spawn_real"); {
    pid_t p; int st;
    char *av[] = {"zz_true", NULL};
    char *ev[] = {"FOO=1", NULL};
    int rc = posix_spawn(&p, "/usr/bin/zz_true", NULL, NULL, av, ev);
    if (rc == 0) { waitpid(p, &st, 0); fprintf(stderr, "POSIX_SPAWN_REAL_EXIT=%d\n", WIFEXITED(st) ? WEXITSTATUS(st) : -1); }
    else fprintf(stderr, "POSIX_SPAWN_REAL_RC=%d\n", rc);
  }

  fprintf(stderr, "DONE\n");
  return 0;
}
