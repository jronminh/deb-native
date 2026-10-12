/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * dn-emu -- the emulation suite.
 *
 * deb-native's second authority (docs/spec/monitor.md, docs/spec/emulation.md):
 * for a **refused** or **divergent** operation, `dn-emu` decides *emulate or
 * refuse*, from its own models, and answers.  It never makes the kernel do it;
 * it answers in the kernel's place — with the value a permitted system would
 * give, or with a clear refusal (`EPERM` / `ENOSYS` + a reason).
 *
 * It is a shared library, not a process: both executors link it — `dn-shim`
 * (inside the program) and `dn-trace` (inside the monitor) — and read the same
 * **state**, which lives in the tree's store (`RT`), because the two are
 * different processes.  One model, two enforcement points.
 *
 * B1: the unit and its entry points exist; every answer is `DN_EMU_PASS`, so
 * nothing changes yet and the existing emulation (`tracee/seccomp.c`, the
 * owner store) keeps doing the work.  The entry points are where the
 * subsystems land.
 */
#ifndef DN_EMU_H
#define DN_EMU_H

#ifdef __cplusplus
extern "C" {
#endif

/* A refused/divergent operation, as the caller has it.  `nr` is the kernel
 * syscall number (the shim fills it from the libc call it saw; the tracer from
 * the tracee's registers).  `symbol` is how the shim found the call, and is
 * only a hint for diagnostics. */
typedef struct {
	long nr;
	unsigned long args[6];
} DnEmuCall;

typedef enum {
	DN_EMU_PASS,    /* not dn-emu's to answer: the executor proceeds */
	DN_EMU_ANSWER,  /* answer with .ret; the kernel is never reached */
	DN_EMU_REFUSE,  /* clear refusal: .ret, and .reason names it */
} DnEmuOutcome;

typedef struct {
	DnEmuOutcome outcome;
	long ret;            /* the value the program sees */
	const char *reason;  /* a name for DN_EMU_REFUSE, and for diagnostics */
} DnEmuDecision;

/* The syscall face: the dispatcher calls this for a refused/divergent syscall,
 * before the executor's own handling.  A `DN_EMU_PASS` means "as you were". */
DnEmuDecision dn_emu_syscall(const DnEmuCall *call);

/* The libc face: dn-shim calls this for a symbol before it issues the call.
 * The same decision, from the same models; only the way the call was found
 * differs. */
DnEmuDecision dn_emu_libc(const char *symbol, const DnEmuCall *call);

#ifdef __cplusplus
}
#endif

#endif /* DN_EMU_H */
