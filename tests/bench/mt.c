#define _GNU_SOURCE
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
static const char *in[] = { "/usr/bin/awk", "/bin/sh", "/usr/lib/aarch64-linux-gnu/libc.so.6", "/etc/alternatives/pager", "/usr/bin/../lib/../bin/make", "/lib/aarch64-linux-gnu" };
static char want[6][PATH_MAX];
static long bad;
static void *run(void *a)
{
	char out[PATH_MAX];
	long k = (long) a;
	for (int i = 0; i < 20000; i++) {
		int j = (int) ((i + k) % 6);
		struct stat st;
		if (!realpath(in[j], out) || strcmp(out, want[j]) != 0 || stat(in[j], &st) != 0 || st.st_uid != 0 || getuid() != 0)
			__atomic_add_fetch(&bad, 1, __ATOMIC_RELAXED);
	}
	return NULL;
}
int main(void)
{
	pthread_t t[8];
	char here[PATH_MAX], c[PATH_MAX], p[PATH_MAX];
	for (int j = 0; j < 6; j++)
		if (!realpath(in[j], want[j])) { perror(in[j]); return 1; }
	for (long k = 0; k < 8; k++) pthread_create(&t[k], NULL, run, (void *) k);
	for (int k = 0; k < 8; k++) pthread_join(t[k], NULL);
	printf("threads: %ld wrong of 160000\n", bad);
	chdir("/usr/share");
	realpath(".", here);
	pid_t v = vfork();
	if (v == 0) {
		chdir("/tmp");
		realpath(".", c);
		setuid(1000);
		if (getuid() != 1000) _exit(3);
		_exit(strcmp(c, "/tmp") == 0 ? 0 : 2);
	}
	int st;
	waitpid(v, &st, 0);
	realpath(".", p);
	printf("vfork child rc=%d; parent cwd %s -> %s, uid %u\n", WEXITSTATUS(st), here, p, getuid());
	fflush(stdout); if (fork() == 0) { printf("fork child cwd ok=%d uid=%u\n", realpath(".", c) && !strcmp(c, "/usr/share"), getuid()); fflush(stdout); _exit(0); }
	wait(NULL);
	return 0;
}
