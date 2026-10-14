#!/usr/bin/env python3
"""Generate policy-rows.inc: dn-tiny's traced set.  A syscall is caught only when
the tracer does something with it -- maps a path, runs the exec gate, answers it
at entry (fake root), rewrites its result at exit, or drops a copy the tracer
keeps (the link cache, a working directory) -- because a caught call costs
a stop whether or not anything is done.  Only the rows that rewrite a result ask
for the exit stop.

Usage: gen_rows.py OUT"""
import sys

out = sys.argv[1]

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

# what a call may change that the tracer keeps a copy of (E_LINKS=1: the link cache,
# E_CWD=2: the working directory).  A rename or an unlink of a directory moves every
# path below it, so they drop the cached working directories too.
EFFECT = {35: 3, 36: 1, 37: 1, 38: 3, 276: 3,     # unlinkat symlinkat linkat renameat renameat2
          49: 2, 50: 2}                          # chdir fchdir

# The filter tests the rows in order, for every syscall that reaches it, so the
# rows met most often come first (a gcc build: readlinkat, openat, fstat, newfstatat).
HOT = [78, 56, 80, 79, 48, 17, 221, 35, 291, 34, 38, 276, 49]
order = HOT + sorted(n for n in set(PATH_ARG) | set(FAKE) | set(RESULT) | set(EFFECT) if n not in HOT)

with open(out, "w") as f:
    for n in order:
        a = PATH_ARG.get(n, ())
        sp = "SP_EXEC" if n == 221 else "SP_LINK" if n == 37 else "SP_NONE"       # execve has its own part: the exec gate
        if n == 221:
            a = ()                                       # its path is mapped by the gate, not generically
        fk = FAKE.get(n, 0)
        rs, ra = RESULT.get(n, (0, -1))
        ex = 1 if rs else 0                              # only a result to rewrite needs the exit stop
        f.write("\t{ %d, %d, %d, %d, %s, %d, %d, %d, %d },\n" % (n, ex, a[0] if len(a) > 0 else -1, a[1] if len(a) > 1 else -1, sp, fk, rs, ra, EFFECT.get(n, 0)))
print("rows:", len(order), " with exit stop:", len(RESULT), " with a path argument:", len(PATH_ARG),
      " faked:", len(FAKE), " result rewritten:", len(RESULT), " with an effect:", len(EFFECT))
