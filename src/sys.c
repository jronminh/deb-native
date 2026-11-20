/* sys.c -- the entry point and the helpers that are not one-line wrappers. */
#include "sys.h"

char **environ;
int main(int argc, char **argv);

__attribute__((noreturn, used)) void c_start(u64 *sp)
{
	int argc = (int) sp[0];
	char **argv = (char **) (sp + 1);

	environ = argv + argc + 1;
	sys_exit(main(argc, argv));
}

__asm__(".global _start\n_start:\n\tmov x0, sp\n\tbl c_start\n");

int sys_execv(const char *path, char *const argv[])
{
	return (int) sc(N_execve, (i64) path, (i64) argv, (i64) environ, 0, 0, 0);
}

void sys_cpu_pin(int cpu)
{
	u64 mask[16];
	int i;

	for (i = 0; i < 16; i++)
		mask[i] = 0;
	mask[cpu / 64] = 1UL << (cpu % 64);
	sc(N_sched_setaffinity, 0, sizeof mask, (i64) mask, 0, 0, 0);
}

void sys_default_sigpipe(void)
{
	struct { u64 handler, flags, restorer, mask; } act = { 0, 0, 0, 0 };

	sc(N_rt_sigaction, 13 /* SIGPIPE */, (i64) &act, 0, 8, 0, 0);
}

i64 sys_monotonic_ns(void)
{
	struct { i64 s, ns; } ts = { 0, 0 };

	sc(N_clock_gettime, 1 /* CLOCK_MONOTONIC */, (i64) &ts, 0, 0, 0, 0);
	return ts.s * 1000000000L + ts.ns;
}

/* ---- strings ------------------------------------------------------------- */
size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }

int strcmp(const char *a, const char *b)
{
	while (*a && *a == *b) { a++; b++; }
	return (unsigned char) *a - (unsigned char) *b;
}

int strncmp(const char *a, const char *b, size_t n)
{
	while (n && *a && *a == *b) { a++; b++; n--; }
	return n ? (unsigned char) *a - (unsigned char) *b : 0;
}

char *strchr(const char *s, int c)
{
	for (; *s; s++)
		if (*s == (char) c)
			return (char *) s;
	return c == 0 ? (char *) s : 0;
}

char *strpbrk(const char *s, const char *set)
{
	for (; *s; s++)
		if (strchr(set, *s))
			return (char *) s;
	return 0;
}

void *memchr(const void *s, int c, size_t n)
{
	const unsigned char *p = s;

	for (; n; n--, p++)
		if (*p == (unsigned char) c)
			return (void *) p;
	return 0;
}

/* The compiler may turn a struct copy or a zeroing into calls to these, even in
 * freestanding code, so they must exist.  -fno-tree-loop-distribute-patterns
 * keeps it from turning these very loops back into calls to themselves. */
void *memcpy(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;

	while (n--)
		*a++ = *b++;
	return d;
}

void *memmove(void *d, const void *s, size_t n)
{
	unsigned char *a = d;
	const unsigned char *b = s;

	if (a < b)
		while (n--)
			*a++ = *b++;
	else
		while (n--)
			a[n] = b[n];
	return d;
}

void *memset(void *d, int c, size_t n)
{
	unsigned char *a = d;

	while (n--)
		*a++ = (unsigned char) c;
	return d;
}

int memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char *x = a, *y = b;

	for (; n; n--, x++, y++)
		if (*x != *y)
			return *x - *y;
	return 0;
}

int cat2(char *out, size_t cap, const char *a, const char *b)
{
	size_t la = strlen(a), lb = strlen(b);

	if (la + lb + 1 > cap)
		return -1;
	memcpy(out, a, la);
	memcpy(out + la, b, lb + 1);
	return (int) (la + lb);
}

int scpy(char *out, size_t cap, const char *s)
{
	size_t n = strlen(s);

	if (n + 1 > cap)
		return -1;
	memcpy(out, s, n + 1);
	return (int) n;
}

u64 atou(const char *s)
{
	u64 v = 0;

	for (; *s >= '0' && *s <= '9'; s++)
		v = v * 10 + (u64) (*s - '0');
	return v;
}

/* ---- output --------------------------------------------------------------- */
void put(int fd, const char *s) { sys_write(fd, s, strlen(s)); }

void putu(int fd, u64 v)
{
	char b[24];
	int i = 23;

	b[i] = 0;
	do {
		b[--i] = (char) ('0' + v % 10);
		v /= 10;
	} while (v);
	put(fd, b + i);
}

void putms(int fd, i64 ns)
{
	putu(fd, (u64) (ns / 1000000000L));
	put(fd, ".");
	{
		u64 ms = (u64) (ns % 1000000000L) / 1000000;

		if (ms < 100) put(fd, "0");
		if (ms < 10) put(fd, "0");
		putu(fd, ms);
	}
}
