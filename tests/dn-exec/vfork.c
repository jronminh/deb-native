/* vfork/exec check for dn-exec: a sentinel stays intact in the parent. */
typedef unsigned long u64;
typedef long i64;

#define SYS_write 64
#define SYS_exit 93
#define SYS_clone 220
#define SYS_mmap 222
#define SYS_execve 221
#define SYS_wait4 260

static i64 sc3(i64 n, i64 a, i64 b, i64 c)
{
	register i64 x8 __asm__("x8") = n;
	register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2) : "memory");
	return x0;
}

static i64 sc6(i64 n, i64 a, i64 b, i64 c, i64 d, i64 e, i64 f)
{
	register i64 x8 __asm__("x8") = n;
	register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c;
	register i64 x3 __asm__("x3") = d, x4 __asm__("x4") = e, x5 __asm__("x5") = f;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3), "r"(x4), "r"(x5) : "memory");
	return x0;
}

static i64 sc4(i64 n, i64 a, i64 b, i64 c, i64 d)
{
	register i64 x8 __asm__("x8") = n;
	register i64 x0 __asm__("x0") = a, x1 __asm__("x1") = b, x2 __asm__("x2") = c, x3 __asm__("x3") = d;
	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory");
	return x0;
}

static u64 slen(const char *s) { u64 n = 0; while (s[n]) n++; return n; }
static int same_n(const char *a, const char *b, u64 n) { while (n--) if (*a++ != *b++) return 0; return 1; }
static void put(const char *s) { sc3(SYS_write, 2, (i64) s, (i64) slen(s)); }
static void putn(i64 v) { char b[24]; int n = 0; if (v < 0) { put("-"); v = -v; } do { b[n++] = (char)('0' + v % 10); v /= 10; } while (v); while (n) { sc3(SYS_write, 2, (i64) &b[--n], 1); } }
static void out(const char *s) { sc3(SYS_write, 1, (i64) s, (i64) slen(s)); }
static u64 num(const char *s) { u64 v = 0; while (*s >= '0' && *s <= '9') v = v * 10 + (u64) (*s++ - '0'); return v; }

__attribute__((used)) void dn_main(u64 *sp)
{
	int argc = (int) sp[0];
	char **argv = (char **) &sp[1];
	char **envp = argv + argc + 1;
	volatile u64 sentinel[64];
	u64 n = 4;
	u64 stack_top = 0;               /* vfork: the child uses the parent stack */

	if (argc < 3) { out("bad-arg\n"); goto bad; }
	if (argc > 3)
		n = num(argv[3]);
	for (int i = 0; i < 64; i++)
		sentinel[i] = 0xfeed000000000000UL + (u64) i;

	for (u64 i = 0; i < n; i++) {
		char *child[3];
		i64 pid;

		u64 al = slen(argv[1]);

		child[0] = argv[1];                 /* dn-exec, or the program */
		child[1] = 0;
		child[2] = 0;
		if (al >= 7 && same_n(argv[1] + al - 7, "dn-exec", 7) &&
		    (al == 7 || argv[1][al - 8] == '/'))
			child[1] = argv[2];         /* dn-exec <program> */
		pid = sc6(SYS_clone, 0x4111 /* CLONE_VM|CLONE_VFORK|SIGCHLD */, (i64) stack_top, 0, 0, 0, 0);
		if (pid == 0) {
			i64 er = sc3(SYS_execve, (i64) argv[1], (i64) child, (i64) envp);
			out("child-exec-fail "); putn(er); out("\n");
			sc3(SYS_exit, 127, 0, 0);
		}
		if (pid < 0) { out("bad-clone\n"); goto bad; }
		{
			int st = 0;
			if (sc4(SYS_wait4, pid, (i64) &st, 0, 0) < 0) { out("bad-wait\n"); goto bad; }
			if (st != 0) { out("bad-status\n"); goto bad; }
		}
	}
	for (int i = 0; i < 64; i++)
		if (sentinel[i] != 0xfeed000000000000UL + (u64) i) { out("bad-sentinel\n"); goto bad; }
	out("vfork-ok\n");
	sc3(SYS_exit, 0, 0, 0);
bad:
	out("vfork-bad\n");
	sc3(SYS_exit, 1, 0, 0);
	for (;;) {}
}

__attribute__((naked, noreturn)) void _start(void)
{
	__asm__ volatile("mov x0, sp\n\tbl dn_main\n\tmov x0, #1\n\tmov x8, #93\n\tsvc #0");
}
