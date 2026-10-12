/* dn-start: make dn-monitor survive Android's seccomp at its own startup.
 *
 * dn-monitor is the tree's root process -- nothing traces it -- so its own
 * startup must get past Android's seccomp unaided, exactly as the old
 * build-overlay-glibc.sh got past it by linking an Android-patched glibc.
 * This does the same job without a glibc fork: install a SIGSYS handler
 * before glibc initializes.
 *
 * Why a handler and not SIG_IGN: Android answers set_robust_list (and rseq,
 * clone3) with SECCOMP_RET_TRAP, which delivers SIGSYS via force_sig_info().
 * SIG_IGN does not defuse it -- the process is still killed.  A real handler
 * does: the handler runs, returns, and the syscall fails with ENOSYS, which
 * glibc tolerates (it is the same answer the tracer gives a traced program).
 *
 * Why before glibc: glibc issues set_robust_list very early, from
 * __libc_start_main, before constructors and before main.  The only place
 * early enough is the crt1 call itself, so this file is linked with
 * -Wl,--wrap=__libc_start_main and __wrap___libc_start_main installs the
 * handler, then hands the original arguments to the real one.
 *
 * It runs before glibc's TLS is set up, so it uses only the stack and raw
 * syscalls (no errno, no libc), and is compiled with -fno-stack-protector
 * (see the dn-start.o rule in GNUmakefile) so it never reads the canary.
 */

#define DN_SIGSYS 31
#define DN_SYS_rt_sigaction 134

static void dn_sigsys_handler(int signum)
{
	(void) signum;
}

static long dn_rt_sigaction(long signum, const void *act, void *oldact,
			    long sigsetsize)
{
	register long x8 __asm__("x8") = DN_SYS_rt_sigaction;
	register long x0 __asm__("x0") = signum;
	register long x1 __asm__("x1") = (long) act;
	register long x2 __asm__("x2") = (long) oldact;
	register long x3 __asm__("x3") = sigsetsize;

	__asm__ volatile ("svc #0"
			  : "+r"(x0)
			  : "r"(x8), "r"(x1), "r"(x2), "r"(x3)
			  : "memory");
	return x0;
}

extern int __real___libc_start_main(int (*main)(int, char **, char **),
				    int argc, char **argv,
				    void (*init)(void), void (*fini)(void),
				    void (*rtld_fini)(void), void *stack_end);

int __wrap___libc_start_main(int (*main)(int, char **, char **),
			     int argc, char **argv,
			     void (*init)(void), void (*fini)(void),
			     void (*rtld_fini)(void), void *stack_end)
{
	/* The kernel's struct sigaction, aarch64 layout: handler, flags,
	 * then sa_mask read as sigsetsize bytes.  SIG_IGN (1) is NOT enough;
	 * the address of a handler that returns is what defuses the TRAP. */
	unsigned long act[4] = { (unsigned long) dn_sigsys_handler, 0UL, 0UL, 0UL };

	(void) dn_rt_sigaction(DN_SIGSYS, act, 0, 8 /* sizeof(sigset_t) */);
	return __real___libc_start_main(main, argc, argv,
					init, fini, rtld_fini, stack_end);
}
