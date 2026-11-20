#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
int main(void)
{
	unsigned char *f = (unsigned char *) dlsym(RTLD_DEFAULT, "getpid");
	unsigned char *pg = (unsigned char *) ((unsigned long) f & ~4095UL);
	unsigned int insn;
	int r;

	printf("getpid at %p\n", (void *) f);
	r = mprotect(pg, 4096, PROT_READ | PROT_WRITE);
	printf("mprotect RW: %d %s\n", r, r ? strerror(errno) : "");
	if (r) return 1;
	memcpy(&insn, f, 4);
	memcpy(f, &insn, 4);                    /* a write: the page is now a private copy */
	r = mprotect(pg, 4096, PROT_READ | PROT_EXEC);
	printf("mprotect RX after write (execmod): %d %s\n", r, r ? strerror(errno) : "");
	if (r) return 1;
	__builtin___clear_cache((char *) pg, (char *) pg + 4096);
	printf("getpid() through the patched page: %d (real %ld)\n", getpid(), (long) syscall(172));
	void *a = mmap(NULL, 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	printf("anonymous RWX mmap: %s\n", a == MAP_FAILED ? strerror(errno) : "ok");
	void *b = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	r = mprotect(b, 4096, PROT_READ | PROT_EXEC);
	printf("anonymous RW -> RX: %d %s\n", r, r ? strerror(errno) : "");
	return 0;
}
