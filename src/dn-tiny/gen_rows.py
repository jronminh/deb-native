#!/usr/bin/env python3
"""Generate policy-rows.inc: dn-monitor's traced set (arm64 numbers and the
exit-stop flag, read from the tracer's own sources) plus the register index of
the path argument for the path syscalls."""
import re, sys

src, out = sys.argv[1], sys.argv[2]

# arm64 syscall number -> index of the path argument register(s); the second
# is for the two-path calls (rename, link).  symlinkat's target (arg 0) is data,
# not a path to map: only the link path (arg 2) is.
PATH_ARG = {
    56: (1,),       # openat
    48: (1,),       # faccessat
    439: (1,),      # faccessat2
    437: (1,),      # openat2 (the platform allows it on newer Android; on older it is killed and substituted)
    452: (1,),      # fchmodat2 (the same)
    79: (1,),       # newfstatat
    291: (1,),      # statx
    78: (1,),       # readlinkat
    221: (0,),      # execve
    35: (1,),       # unlinkat
    34: (1,),       # mkdirat
    33: (1,),       # mknodat
    53: (1,),       # fchmodat
    54: (1,),       # fchownat
    88: (1,),       # utimensat
    36: (2,),       # symlinkat (link path)
    37: (1, 3),     # linkat
    38: (1, 3),     # renameat
    276: (1, 3),    # renameat2
    49: (0,),       # chdir
    51: (0,),       # chroot
    43: (0,),       # statfs
    45: (0,),       # truncate
    27: (1,),       # inotify_add_watch
    5: (0,), 6: (0,), 8: (0,), 9: (0,), 11: (0,), 12: (0,), 14: (0,), 15: (0,),   # *xattr by path
}

# fake root: syscalls answered at entry without running them (F_ZERO=1, F_RESIDS=2, F_GROUPS=3)
FAKE = {174: 5, 175: 5, 176: 5, 177: 5,          # getuid geteuid getgid getegid: the process's own
        148: 2, 150: 2, 158: 3,                  # getresuid getresgid getgroups
        146: 6, 144: 6, 145: 6, 143: 6, 147: 6, 149: 6, 151: 6, 152: 6,   # set*id: remembered
        159: 1,                                  # setgroups
        55: 1, 54: 1}                            # fchown fchownat

# results rewritten at exit: nr -> (R_*, register index of the buffer)
#   R_CWD=1 getcwd, R_LINK=2 readlinkat, R_STAT=3 stat buffer, R_STATX=4 statx buffer
RESULT = {17: (1, -1),                           # getcwd
          78: (2, -1),                           # readlinkat
          79: (3, 2), 80: (3, 1),                # newfstatat (buf in x2), fstat (buf in x1)
          291: (4, 4)}                           # statx (buf in x4)

nums = {}
for line in open(f"{src}/syscall/sysnums-arm64.h", encoding="utf-8", errors="replace"):
    m = re.search(r"\[\s*(\d+)\s*\]\s*=\s*(PR_\w+)", line)
    if m:
        nums[m.group(2)] = int(m.group(1))

text = open(f"{src}/monitor/monitor.c", encoding="utf-8", errors="replace").read()
flags, order = {}, []
for name in ("proot_sysnums", "fakeroot_sysnums"):
    blk = re.search(r"static FilteredSysnum %s\[\] = \{(.*?)FILTERED_SYSNUM_END" % name, text, re.S)
    for e in re.finditer(r"\{\s*(PR_\w+)\s*,\s*(FILTER_SYSEXIT|0)\s*\}", blk.group(1)):
        pr, fl = e.group(1), (1 if e.group(2) == "FILTER_SYSEXIT" else 0)
        if pr not in nums:
            continue
        n = nums[pr]
        if n not in flags:
            order.append(n)
            flags[n] = 0
        flags[n] |= fl

# a path row the filter would not catch is useless: catch it (one stop, no exit stop)
added = [n for n in list(PATH_ARG) + list(FAKE) + list(RESULT) if n not in flags]
for n in added:
    order.append(n)
    flags[n] = 0

with open(out, "w") as f:
    for n in order:
        a = PATH_ARG.get(n, ())
        sp = "SP_EXEC" if n == 221 else "SP_LINK" if n == 37 else "SP_NONE"       # execve has its own part: the exec gate
        if n == 221:
            a = ()                                       # its path is mapped by the gate, not generically
        fk = FAKE.get(n, 0)
        if fk:
            flags[n] = 0                                 # a faked call is skipped at entry: no exit stop
        rs, ra = RESULT.get(n, (0, -1))
        if rs:
            flags[n] = 1                                 # a result to rewrite needs the exit stop
        f.write("\t{ %d, %d, %d, %d, %s, %d, %d, %d },\n" % (n, flags[n], a[0] if len(a) > 0 else -1, a[1] if len(a) > 1 else -1, sp, fk, rs, ra))
print("rows:", len(order), " with exit stop:", sum(flags.values()), " with a path argument:",
      sum(1 for n in order if n in PATH_ARG), " faked:", sum(1 for n in order if n in FAKE),
      " result rewritten:", sum(1 for n in order if n in RESULT),
      " added to the traced set:", added)
