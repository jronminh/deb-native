/* Probe for statically-bound libc NSS reads (syscall-boundary.md case 2).
 * The LD_PRELOAD shim cannot see getpwnam()'s internal opens; the syscall
 * tracer can. Run under dn-run to check the NSS route. */
#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pwd.h>

int main(void)
{
  char buf[4096];
  ssize_t total = 0, r;
  struct passwd *pw;
  int fd = open("/etc/passwd", O_RDONLY);

  if (fd >= 0) {
    while (total < (ssize_t)sizeof buf - 1 &&
           (r = read(fd, buf + total, sizeof buf - 1 - total)) > 0)
      total += r;
    buf[total] = '\0';
    close(fd);
  }
  fprintf(stderr, "open_dnshim=%d\n",
          strstr(buf, "dnshim") ? 1 : 0);

  pw = getpwnam("dnshim");
  fprintf(stderr, "getpwnam_dnshim=%s\n", pw ? pw->pw_name : "NOTFOUND");
  return pw ? 0 : 1;
}
