/* census.c -- every syscall number from 0 to 470: how does this process fare?
 * Freestanding aarch64.  One forked child per number, zero arguments, the result
 * reported as "nr<TAB>ok|errno N|signal N".  A few numbers are skipped because
 * they end or hang the process, stop it under a tracer, or change global state. */
typedef unsigned long u64;
typedef long i64;

static inline i64 sys(i64 n, i64 a, i64 b, i64 c, i64 d)
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
	sys(64, 1, (i64) s, (i64) n, 0);
}

static void outd(i64 v)
{
	char b[24];
	int i = 23;

	b[i] = 0;
	if (v == 0)
		b[--i] = '0';
	while (v > 0) {
		b[--i] = (char) ('0' + v % 10);
		v /= 10;
	}
	out(b + i);
}

/* ends or hangs the process, stops it under a tracer, or changes global state */
static const int SKIP[] = { 58, 72, 73, 81, 93, 94, 104, 117, 133, 139, 142, 220, 267, 277, -1 };

__attribute__((noreturn)) void c_start(void)
{
	int nr, i;

	for (nr = 0; nr <= 470; nr++) {
		i64 pid;
		int skip = 0;

		for (i = 0; SKIP[i] >= 0; i++)
			if (SKIP[i] == nr)
				skip = 1;
		if (skip)
			continue;
		pid = sys(220, 17, 0, 0, 0);                    /* clone(SIGCHLD) */
		if (pid == 0) {
			i64 r = sys(nr, 0, 0, 0, 0);

			sys(94, r < 0 && r > -4096 ? -r : 0, 0, 0, 0);
			for (;;) ;
		} else {
			int st = 0;

			sys(260, pid, (i64) &st, 0, 0);
			outd(nr);
			out("\t");
			if ((st & 0x7f) != 0 && (st & 0x7f) != 0x7f) {
				out("signal ");
				outd(st & 0x7f);
			} else if (((st >> 8) & 0xff) == 0)
				out("ok");
			else {
				out("errno ");
				outd((st >> 8) & 0xff);
			}
			out("\n");
		}
	}
	sys(94, 0, 0, 0, 0);
	for (;;) ;
}

__asm__(".global _start\n_start:\n\tbl c_start\n");
