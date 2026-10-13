/* substtest.c -- freestanding guest program: calls the syscalls the policy
 * substitutes or fakes, prints what it saw.  Meant to run under dn-tiny with
 * guest paths; the same binary natively shows what the kernel itself answers. */
typedef unsigned long u64;
typedef long i64;

static inline i64 sc(i64 n, i64 a, i64 b, i64 c, i64 d)
{
	register i64 x8 __asm__("x8") = n;
	register i64 x0 __asm__("x0") = a;
	register i64 x1 __asm__("x1") = b;
	register i64 x2 __asm__("x2") = c;
	register i64 x3 __asm__("x3") = d;

	__asm__ volatile("svc 0" : "+r"(x0) : "r"(x8), "r"(x1), "r"(x2), "r"(x3) : "memory", "cc");
	return x0;
}

static void out(const char *s)
{
	u64 n = 0;

	while (s[n])
		n++;
	sc(64, 1, (i64) s, (i64) n, 0);
}

static void outd(i64 v)
{
	char b[24];
	int i = 23;
	int neg = v < 0;

	if (neg)
		v = -v;
	b[i] = 0;
	do {
		b[--i] = (char) ('0' + v % 10);
		v /= 10;
	} while (v);
	if (neg)
		b[--i] = '-';
	out(b + i);
}

static void show(const char *what, i64 r)
{
	out(what);
	out(" = ");
	outd(r);
	out("\n");
}

#define AT_FDCWD (-100)

__attribute__((noreturn)) void c_start(void)
{
	char buf[17];
	unsigned ids[3] = { 77, 77, 77 };
	unsigned grp[4] = { 77, 77, 77, 77 };
	u64 how[3] = { 0, 0, 0 };
	i64 fd;

	out("-- substituted (killed by the platform in the app)\n");
	show("faccessat2(/etc/os-release, R_OK, 0)        [0 expected]", sc(439, AT_FDCWD, (i64) "/etc/os-release", 4, 0));
	show("faccessat2(/etc/missing, F_OK, 0)           [-2 ENOENT expected]", sc(439, AT_FDCWD, (i64) "/etc/missing", 0, 0));
	show("faccessat2(..., AT_SYMLINK_NOFOLLOW)        [-38 ENOSYS: glibc falls back]", sc(439, AT_FDCWD, (i64) "/etc/os-release", 4, 0x100));

	fd = sc(56, AT_FDCWD, (i64) "/tmp/substfile", 0x41 /* O_WRONLY|O_CREAT */, 0600);
	show("openat(/tmp/substfile, O_CREAT)             [fd >= 0]", fd);
	if (fd >= 0)
		sc(57, fd, 0, 0, 0);
	show("fchmodat2(/tmp/substfile, 0640, 0)          [0 expected]", sc(452, AT_FDCWD, (i64) "/tmp/substfile", 0640, 0));
	show("fchmodat2(..., AT_SYMLINK_NOFOLLOW)         [-38 ENOSYS]", sc(452, AT_FDCWD, (i64) "/tmp/substfile", 0640, 0x100));

	how[0] = 0;                                        /* O_RDONLY */
	fd = sc(437, AT_FDCWD, (i64) "/etc/os-release", (i64) how, 24);
	show("openat2(/etc/os-release, O_RDONLY)          [fd >= 0]", fd);
	if (fd >= 0) {
		i64 n = sc(63, fd, (i64) buf, 16, 0);

		buf[n > 0 ? n : 0] = 0;
		out("  first bytes: ");
		out(buf);
		out("\n");
		sc(57, fd, 0, 0, 0);
	}
	how[2] = 0x04;                                     /* RESOLVE_NO_SYMLINKS: openat cannot honour it */
	show("openat2(..., RESOLVE_NO_SYMLINKS)           [-38 ENOSYS]", sc(437, AT_FDCWD, (i64) "/etc/os-release", (i64) how, 24));

	out("-- fake root (answered at entry, never run)\n");
	show("getuid()                                    [0 expected]", sc(174, 0, 0, 0, 0));
	show("geteuid()                                   [0]", sc(175, 0, 0, 0, 0));
	show("getgid()                                    [0]", sc(176, 0, 0, 0, 0));
	show("getegid()                                   [0]", sc(177, 0, 0, 0, 0));
	show("getresuid(&r,&e,&s)                         [0]", sc(148, (i64) &ids[0], (i64) &ids[1], (i64) &ids[2], 0));
	show("  r e s written                             [0 0 0]", (i64) ids[0] * 100 + (i64) ids[1] * 10 + (i64) ids[2]);
	show("getgroups(0, NULL)                          [1 group]", sc(158, 0, 0, 0, 0));
	show("getgroups(4, list)                          [1]", sc(158, 4, (i64) grp, 0, 0));
	show("  list[0]                                   [0]", (i64) grp[0]);
	show("setuid(0)                                   [0]", sc(146, 0, 0, 0, 0));
	show("setgid(0)                                   [0]", sc(144, 0, 0, 0, 0));
	show("setresuid(0,0,0)                            [0]", sc(147, 0, 0, 0, 0));
	show("setgroups(0, NULL)                          [0]", sc(159, 0, 0, 0, 0));
	show("fchownat(/tmp/substfile, 0, 0)              [0]", sc(54, AT_FDCWD, (i64) "/tmp/substfile", 0, 0));

	show("setreuid(0,0)                               [0]", sc(145, 0, 0, 0, 0));
	show("setregid(0,0)                               [0]", sc(143, 0, 0, 0, 0));
	show("setresgid(0,0,0)                            [0]", sc(149, 0, 0, 0, 0));
	show("setfsuid(0)                                 [0]", sc(151, 0, 0, 0, 0));
	show("setfsgid(0)                                 [0]", sc(152, 0, 0, 0, 0));

	out("-- equivalent call\n");
	show("accept(0,0,0) -> accept4                    [-88 ENOTSOCK, as accept4 says]", sc(202, 0, 0, 0, 0));

	out("-- refused, as for any non-root user\n");
	show("mount(...)                                  [-1 EPERM]", sc(40, 0, 0, 0, 0));
	show("clock_settime(...)                          [-1 EPERM]", sc(112, 0, 0, 0, 0));
	show("settimeofday(...)                           [-1 EPERM]", sc(170, 0, 0, 0, 0));
	show("sethostname(...)                            [-1 EPERM]", sc(161, 0, 0, 0, 0));
	show("init_module(...)                            [-1 EPERM]", sc(105, 0, 0, 0, 0));
	sc(94, 0, 0, 0, 0);
	for (;;) ;
}

__asm__(".global _start\n_start:\n\tbl c_start\n");
