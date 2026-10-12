/* dn-emu -- the emulation suite.  See dn-emu.h.
 *
 * B1: the unit exists and the dispatcher calls it, but every answer is
 * `DN_EMU_PASS` — nothing changes yet, and the emulation that lives in the
 * tracer keeps doing the work.  Each subsystem (docs/spec/emulation.md) lands
 * here, with its whole trick surface.
 */
#include <stddef.h>  /* NULL */

#include "dn-emu.h"

static DnEmuDecision pass(void)
{
	DnEmuDecision decision;

	decision.outcome = DN_EMU_PASS;
	decision.ret = 0;
	decision.reason = NULL;

	return decision;
}

DnEmuDecision dn_emu_syscall(const DnEmuCall *call)
{
	(void) call;
	return pass();
}

DnEmuDecision dn_emu_libc(const char *symbol, const DnEmuCall *call)
{
	(void) symbol;
	(void) call;
	return pass();
}
