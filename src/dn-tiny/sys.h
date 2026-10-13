/* sys.h -- the whole of the "libc" this program needs: raw aarch64 syscalls,
 * the entry point, and a few string helpers.  No glibc, so nothing runs before
 * main() and nothing here calls a syscall the app domain would kill.
 * Only the kernel's own uapi headers are used. */
#ifndef SYS_H
#define SYS_H
#include <stddef.h>
#include <stdint.h>
#include <asm/ptrace.h>          /* struct user_pt_regs */
#include <linux/uio.h>           /* struct iovec */

typedef long ssize_t;
typedef int pid_t;
typedef unsigned long u64;
typedef long i64;

/* a raw syscall: the result, or -errno */
static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
	register i64 x8 __asm__("x8") = n;
	register i64 x0 __asm__("x0") = a;
	register i64 x1 __asm__("x1") = b;
	register i64 x2 __asm__("x2") = c;
	register i64 x3 __asm__("x3") = d;
	register i64 x4 __asm__("x4") = e;
	register i64 x5 __asm__("x5") = f;

	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory", "cc");
	return x0;
}

/* arm64 syscall numbers */
enum { N_openat = 56, N_close = 57, N_read = 63, N_write = 64, N_exit_group = 94, N_kill = 129,
       N_rt_sigaction = 134, N_sched_setaffinity = 122, N_clock_gettime = 113, N_ptrace = 117,
       N_prctl = 167, N_getpid = 172, N_clone = 220, N_execve = 221, N_wait4 = 260,
       N_process_vm_readv = 270, N_process_vm_writev = 271 };

#define EINTR 4

static inline ssize_t sys_write(int fd, const void *b, size_t n) { return sc(N_write, fd, (i64) b, (i64) n, 0, 0, 0); }
static inline ssize_t sys_read(int fd, void *b, size_t n) { return sc(N_read, fd, (i64) b, (i64) n, 0, 0, 0); }
static inline int sys_open(const char *p, int flags) { return (int) sc(N_openat, -100, (i64) p, flags, 0, 0, 0); }
static inline int sys_close(int fd) { return (int) sc(N_close, fd, 0, 0, 0, 0, 0); }
static inline ssize_t sys_readlink(const char *p, char *buf, size_t n) { return sc(78, -100, (i64) p, (i64) buf, (i64) n, 0, 0); }
static inline i64 sys_ptrace(i64 req, i64 pid, i64 addr, i64 data) { return sc(N_ptrace, req, pid, addr, data, 0, 0); }
static inline pid_t sys_wait4(pid_t pid, int *st, int opt) { return (pid_t) sc(N_wait4, pid, (i64) st, opt, 0, 0, 0); }
static inline pid_t sys_fork(void) { return (pid_t) sc(N_clone, 17 /* SIGCHLD */, 0, 0, 0, 0, 0); }
static inline pid_t sys_getpid(void) { return (pid_t) sc(N_getpid, 0, 0, 0, 0, 0, 0); }
static inline int sys_kill(pid_t p, int sig) { return (int) sc(N_kill, p, sig, 0, 0, 0, 0); }
static inline int sys_prctl(i64 a, i64 b, i64 c) { return (int) sc(N_prctl, a, b, c, 0, 0, 0); }
static inline ssize_t sys_vm_read(pid_t p, const struct iovec *l, const struct iovec *r) { return sc(N_process_vm_readv, p, (i64) l, 1, (i64) r, 1, 0); }
static inline ssize_t sys_vm_write(pid_t p, const struct iovec *l, const struct iovec *r) { return sc(N_process_vm_writev, p, (i64) l, 1, (i64) r, 1, 0); }
__attribute__((noreturn)) static inline void sys_exit(int code) { for (;;) sc(N_exit_group, code, 0, 0, 0, 0, 0); }

extern char **environ;
int sys_execv(const char *path, char *const argv[]);
void sys_cpu_pin(int cpu);                 /* restrict this process (and its children) to one CPU */
void sys_default_sigpipe(void);
i64 sys_monotonic_ns(void);

/* strings: the few the code uses */
size_t strlen(const char *s);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
char *strpbrk(const char *s, const char *set);
void *memchr(const void *s, int c, size_t n);
void *memcpy(void *d, const void *s, size_t n);
void *memmove(void *d, const void *s, size_t n);
void *memset(void *d, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);

/* `a` then `b` into out; the length (without the NUL), or -1 if it does not fit */
int cat2(char *out, size_t cap, const char *a, const char *b);
/* a copy of s into out; the length, or -1 */
int scpy(char *out, size_t cap, const char *s);
u64 atou(const char *s);

/* output to a file descriptor */
void put(int fd, const char *s);
void putu(int fd, u64 v);
void putms(int fd, i64 ns);                /* nanoseconds as seconds with three decimals */

#endif
