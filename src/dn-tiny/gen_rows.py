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

# One catalog: syscall number -> its class and the data the class needs.  Every
# view below is derived from it; there is no second table to keep in step.
#   path      a path argument to map          arg: the register(s)
#   result    a result rewritten at exit      result: (R_*, buffer register)
#   socket    an AF_UNIX sockaddr             addr, len, out
#   exec      the exec gate                   arg: the pathname register
#   identity  fake root, answered at entry    fake: F_*
#   effect    caught only to drop a copy      effect: E_*
#   none      caught only for the SELinux label
# A path entry may also carry a result (statx, readlinkat, the xattr lists), an
# effect (unlinkat, the link movers), or a fake (fchownat).
# R_*: 1 cwd, 2 readlinkat, 3 stat buffer, 4 statx buffer, 5 xattr list, 6 dirent.
# F_*: 1 zero, 2 resids, 3 groups, 5 own ids, 6 remembered set*id.
# E_*: 1 the link cache, 2 a cached cwd, 3 both.
CATALOG = {
    5:   {"cls": "path", "arg": (0,)},              # setxattr
    6:   {"cls": "path", "arg": (0,)},              # lsetxattr
    8:   {"cls": "path", "arg": (0,)},              # getxattr
    9:   {"cls": "path", "arg": (0,)},              # lgetxattr
    10:  {"cls": "none", "caught": True},           # fgetxattr: the name is checked
    11:  {"cls": "path", "arg": (0,), "result": (5, 1)},   # listxattr
    12:  {"cls": "path", "arg": (0,), "result": (5, 1)},   # llistxattr
    13:  {"cls": "result", "result": (5, 1), "caught": True},   # flistxattr
    14:  {"cls": "path", "arg": (0,)},              # removexattr
    15:  {"cls": "path", "arg": (0,)},              # lremovexattr
    17:  {"cls": "result", "result": (1, -1)},      # getcwd
    27:  {"cls": "path", "arg": (1,)},              # inotify_add_watch
    33:  {"cls": "path", "arg": (1,)},              # mknodat
    34:  {"cls": "path", "arg": (1,)},              # mkdirat
    35:  {"cls": "path", "arg": (1,), "effect": 3}, # unlinkat
    36:  {"cls": "path", "arg": (2,), "effect": 1}, # symlinkat: the link path
    37:  {"cls": "path", "arg": (1, 3), "effect": 1},   # linkat
    38:  {"cls": "path", "arg": (1, 3), "effect": 3},   # renameat
    43:  {"cls": "path", "arg": (0,)},              # statfs
    45:  {"cls": "path", "arg": (0,)},              # truncate
    48:  {"cls": "path", "arg": (1,)},              # faccessat
    49:  {"cls": "path", "arg": (0,), "effect": 2}, # chdir
    50:  {"cls": "effect", "effect": 2},            # fchdir
    51:  {"cls": "path", "arg": (0,)},              # chroot
    53:  {"cls": "path", "arg": (1,)},              # fchmodat
    54:  {"cls": "path", "arg": (1,), "fake": 1},   # fchownat
    61:  {"cls": "result", "result": (6, 1), "caught": True},   # getdents64: /proc pids
    55:  {"cls": "identity", "fake": 1},            # fchown
    56:  {"cls": "path", "arg": (1,)},              # openat
    78:  {"cls": "path", "arg": (1,), "result": (2, -1)},   # readlinkat
    79:  {"cls": "path", "arg": (1,), "result": (3, 2)},    # newfstatat
    88:  {"cls": "path", "arg": (1,)},              # utimensat
    123: {"cls": "none", "caught": True},           # sched_getaffinity: a stable mask
    90:  {"cls": "none", "caught": True},           # capget: the virtual Cap* sets
    91:  {"cls": "none", "caught": True},           # capset
    143: {"cls": "identity", "fake": 6},            # setregid
    144: {"cls": "identity", "fake": 6},            # setgid
    145: {"cls": "identity", "fake": 6},            # setreuid
    146: {"cls": "identity", "fake": 6},            # setuid
    147: {"cls": "identity", "fake": 6},            # setresuid
    148: {"cls": "identity", "fake": 2},            # getresuid
    149: {"cls": "identity", "fake": 6},            # setresgid
    150: {"cls": "identity", "fake": 2},            # getresgid
    151: {"cls": "identity", "fake": 6},            # setfsuid
    152: {"cls": "identity", "fake": 6},            # setfsgid
    158: {"cls": "identity", "fake": 3},            # getgroups
    159: {"cls": "identity", "fake": 1},            # setgroups
    167: {"cls": "none", "caught": True},           # prctl: PR_GET_NO_NEW_PRIVS answered 0
    174: {"cls": "identity", "fake": 5},            # getuid
    175: {"cls": "identity", "fake": 5},            # geteuid
    176: {"cls": "identity", "fake": 5},            # getgid
    177: {"cls": "identity", "fake": 5},            # getegid
    200: {"cls": "socket", "addr": 1, "len": 2, "out": 0},   # bind
    202: {"cls": "socket", "addr": 1, "len": 2, "out": 1},   # accept
    203: {"cls": "socket", "addr": 1, "len": 2, "out": 0},   # connect
    204: {"cls": "socket", "addr": 1, "len": 2, "out": 1},   # getsockname
    205: {"cls": "socket", "addr": 1, "len": 2, "out": 1},   # getpeername
    206: {"cls": "socket", "addr": 4, "len": 5, "out": 0},   # sendto
    207: {"cls": "socket", "addr": 4, "len": 5, "out": 1},   # recvfrom
    211: {"cls": "socket", "addr": 1, "len": -1, "out": 0},  # sendmsg
    212: {"cls": "socket", "addr": 1, "len": -1, "out": 1},  # recvmsg
    221: {"cls": "exec", "arg": (0,)},              # execve
    242: {"cls": "socket", "addr": 1, "len": 2, "out": 1},   # accept4
    276: {"cls": "path", "arg": (1, 3), "effect": 3},        # renameat2
    281: {"cls": "exec", "arg": (1,)},              # execveat
    291: {"cls": "path", "arg": (1,), "result": (4, 4)},     # statx
    437: {"cls": "path", "arg": (1,)},              # openat2
    439: {"cls": "path", "arg": (1,)},              # faccessat2
    452: {"cls": "path", "arg": (1,)},              # fchmodat2
}

CLS = {"path", "result", "socket", "exec", "identity", "effect", "none"}
for n, e in CATALOG.items():
    assert 0 <= n <= 470, (n, e)
    assert e["cls"] in CLS, (n, e)
    if e["cls"] == "path":
        assert "arg" in e, (n, e)
    if e["cls"] == "result":
        assert "result" in e, (n, e)
    if e["cls"] == "socket":
        assert "addr" in e and "len" in e and "out" in e, (n, e)
    if e["cls"] == "identity":
        assert "fake" in e, (n, e)
    if e["cls"] == "effect":
        assert "effect" in e, (n, e)
    if e["cls"] == "none":
        assert e.get("caught"), (n, e)

PATH_ARG = {n: e["arg"] for n, e in CATALOG.items() if "arg" in e}
SOCK = {n: (e["addr"], e["len"], e["out"]) for n, e in CATALOG.items() if "addr" in e}
FAKE = {n: e["fake"] for n, e in CATALOG.items() if "fake" in e}
RESULT = {n: e["result"] for n, e in CATALOG.items() if "result" in e}
EFFECT = {n: e["effect"] for n, e in CATALOG.items() if "effect" in e}
CAUGHT = {n for n, e in CATALOG.items() if e.get("caught")}
EXEC = {n for n, e in CATALOG.items() if e["cls"] == "exec"}

# The filter tests the rows in order, for every syscall that reaches it, so the
# rows met most often come first (a gcc build: readlinkat, openat, newfstatat).
HOT = [78, 56, 79, 48, 17, 221, 35, 291, 34, 38, 276, 49]
order = HOT + sorted(n for n in CATALOG if n not in HOT)

with open(out, "w") as f:
    for n in order:
        if n in SOCK:
            ar, lr, o = SOCK[n]
            sp = "SP_SOCK_OUT" if o else "SP_SOCK_IN"
            pa, pa2, ex = ar, lr, (1 if o else 0)
        else:
            a = PATH_ARG.get(n, ())
            sp = "SP_EXEC" if n in EXEC else "SP_LINK" if n == 37 else "SP_NONE"
            if n in EXEC:
                a = ()                                   # its path is mapped by the gate, not generically
            rs0, _ = RESULT.get(n, (0, -1))
            ex = 1 if rs0 else 0                         # only a result to rewrite needs the exit stop
            pa, pa2 = (a[0] if len(a) > 0 else -1), (a[1] if len(a) > 1 else -1)
        fk = FAKE.get(n, 0)
        rs, ra = RESULT.get(n, (0, -1))
        f.write("\t{ %d, %d, %d, %d, %s, %d, %d, %d, %d },\n" % (n, ex, pa, pa2, sp, fk, rs, ra, EFFECT.get(n, 0)))
print("rows:", len(order), " with exit stop:", len(RESULT), " with a path argument:", len(PATH_ARG),
      " faked:", len(FAKE), " result rewritten:", len(RESULT), " with an effect:", len(EFFECT),
      " af_unix:", len(SOCK))
