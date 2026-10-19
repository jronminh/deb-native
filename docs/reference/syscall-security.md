# Restricted syscalls through a security lens

Status: **survey** (2026-10-19).  The general security background for a runtime
that runs a prefix under a reduced syscall interface.  It is not an Android
version guide and not a promise that a prefix is a sandbox: the compatibility
runtime and the security boundary are different products.

The syscall layer is where a host says *what a program may ask the kernel*.
That makes it both a containment mechanism and an attack surface.  This page
collects the white-hat work and the incident patterns worth designing against;
the defensive rules are the last section.

## The model and its limits

A seccomp filter sees a small, read-only `struct seccomp_data`: syscall number,
architecture, instruction pointer, and up to six raw argument values.  It cannot
dereference a pointer.  That gives it one real strength and one real weakness:

- **Strength:** the filter sees a snapshot of the registers at syscall entry;
  changing them mid-call does not change what the filter evaluated.
- **Weakness:** paths, sizes and structures are behind pointers the filter
  cannot see, and the kernel uses those pointers *after* the filter.  Another
  thread can therefore change the memory between the check and the use.  Any
  policy that needs the contents must be implemented with fd-based operations
  or by a supervisor, and the supervisor is where the explicit
  time-of-check/time-of-use races live.

The kernel documentation is explicit: **syscall filtering is not a sandbox**
([kernel docs](https://docs.kernel.org/userspace-api/seccomp_filter.html)).  It
reduces the kernel surface; policy, paths and information flow need other
layers.  An allow-list is the recommended shape.  A deny-list has to be updated
for every new call, flag and ABI, and a value can often be re-encoded without
changing its meaning.

Three details matter before any bypass:

- **Architecture must be checked.**  The x86-64 and x32 ABIs share
  `AUDIT_ARCH_X86_64`; a policy that ignores `__X32_SYSCALL_BIT` can be bypassed
  by setting the bit.  Kernels before 5.4 had additional number aliasing.  On
  arm64 the same question exists for AArch32 compat tasks.
- **The wrapper is not the syscall.**  `exit` is `exit_group`, `fork` is
  `clone`, `open` became `openat` in glibc 2.26; the same C call can be a
  different number on another architecture or glibc
  ([seccomp(2)](https://man7.org/linux/man-pages/man2/seccomp.2.html)).
- **Not every "call" is a syscall.**  vDSO implementations run in user space,
  so a filter never sees them; the legacy vsyscall page has its own quirks.
  New multiplexers (`io_uring`) are a bigger problem, below.

`no_new_privs` and `TSYNC` are part of the model: a filter needs either
`CAP_SYS_ADMIN` or `no_new_privs`, and `TSYNC` applies it to every thread.
Filters are inherited across `fork`/`clone` and preserved across `execve` when
those calls are allowed.

## A taxonomy of bypasses

The classes below are the ones a prefix runtime should assume an attacker will
try.  They are described at the level of mechanism, not as exploit steps.

### 1. Architecture and ABI confusion

The filter checks `nr` and `arch`; a program can sometimes issue the same
operation through a different convention.  The manual names x32; the same
lesson applies to compat ABIs and to applications that use raw `syscall`
instead of libc.

**Design response:** allow-list per architecture; verify `arch` before `nr`;
reject unknown ABI bits; test with 32-bit/compat binaries if the host can run
them.

### 2. Syscall multiplexing: `io_uring`

`io_uring` submits operations through a shared ring and one
`io_uring_enter`; the kernel performs `openat`, `connect`, `send`, `read`,
`write`, and more as ring opcodes.  The seccomp filter sees only the enter
call.  This is documented as a seccomp bypass in practice
([0x74696d, 2022](https://blog.0x74696d.com/posts/iouring-and-seccomp/); a
2026 pentest write-up lists it as the first multiplexing category,
[Antitree](https://www.antitree.com/2026/04/seccomp-in-k8s-2/3-bypasses-and-breakouts/)).
`io_uring` also has `IORING_SETUP_SQPOLL`, a kernel thread that runs
submissions out of band, and a history of kernel bugs of its own.

**Design response:** treat `io_uring_setup/enter/register` as a high-risk
surface.  Block it unless a workload needs it; if it is needed, restrict
opcodes with `IORING_REGISTER_RESTRICTIONS` and keep the kernel patched.  The
Linux 6.6 `kernel.io_uring_disabled` knob is the blunt control.

### 3. User-space notification is not a policy engine

`SECCOMP_RET_USER_NOTIF` delegates a call to a supervisor.  The supervisor can
inspect pointers, choose a return value, inject an fd
(`SECCOMP_IOCTL_NOTIF_ADDFD`), or tell the kernel to continue the call
(`SECCOMP_USER_NOTIF_FLAG_CONTINUE`).  The manual is blunt: this mechanism
**must not** be used to make security policy; `CONTINUE` has a
time-of-check/time-of-use window in which the target can rewrite its arguments
([seccomp_unotify(2)](https://www.man7.org/linux/man-pages/man2/seccomp_unotify.2.html)).

Races named there are the ones to design around:

- the target can be killed or interrupted, its tid reused, and `/proc/tid/mem`
  can point at another process — use `SECCOMP_IOCTL_NOTIF_ID_VALID` around
  every read and before every use;
- a signal handler with `SA_RESTART` can make the same call notify twice;
- a blocking call can be abandoned by the target while the supervisor blocks;
- a supervisor that trusts the target's pointers has already lost;
- if the filter lets the target install a filter with higher precedence, the
  notifier can be bypassed entirely.

Public write-ups have used exactly these properties for "notify injection"
and CTF escapes; the kernel documentation's own warning is the primary source.

**Design response:** if a supervisor is used, it is trusted, the target is not;
copy arguments into supervisor memory, validate with an ID check, never
`CONTINUE` on a pointer-bearing call unless another layer re-checks it, and
never let the target own the listener fd.

### 4. The tracer can rewrite the call

`SECCOMP_RET_TRACE` notifies a ptrace tracer before the call; the tracer can
change the syscall number, skip it, or change the return value.  Before Linux
4.8 the seccomp check was not re-run after the tracer, so a sandbox that
allowed ptrace could be escaped by the tracer; the kernel documentation still
says a seccomp sandbox **must not** allow ptrace without extreme care.  A
privileged tracer can also use `PTRACE_O_SUSPEND_SECCOMP` to suspend the
filter.

**Design response:** ptrace is not an unprivileged tool inside a sandbox; do
not allow it.  If a debugger is needed, it is a host-side, explicitly granted
operation outside the untrusted profile.

### 5. Descriptor and handle smuggling

A path check is not an fd check.  Once an fd exists, `SCM_RIGHTS`,
`pidfd_getfd`, `/proc/pid/fd`, or `open_by_handle_at` can move it or reopen it
without ever naming the path again.  `open_by_handle_at` needs
`CAP_DAC_READ_SEARCH`; when it is present it is a classic container escape.
`pidfd_getfd` needs ptrace-like access; when it is allowed it duplicates an
fd from a more privileged process.

**Design response:** enforce on the opened object, not only on the name;
validate `SCM_RIGHTS` fds; close inherited fds; do not grant
`CAP_DAC_READ_SEARCH`; treat fd passing as a capability transfer.

### 6. Execution that never names a path

`memfd_create` + `fexecve`, `execveat(AT_EMPTY_PATH)`, `O_TMPFILE`, and
`binfmt_misc` can execute bytes that never had a stable path for a path-based
hook to inspect.  A shim that only wraps `execve` will miss them if the raw
syscall path is not covered.

**Design response:** enforce at the syscall boundary, not only in libc; decide
whether executable memfds are allowed; if not, reject `MFD_EXEC`/exec-of-memfd
and audit `execveat`.

### 7. Threads, fork, exec, and privilege transitions

`TSYNC` exists because filters are per-thread.  A thread created before the
filter, a process that installs a weaker filter on another thread, or a
`setuid`/file-capability transition can invalidate the intended policy.
`no_new_privs` exists specifically so an unprivileged filter cannot be bypassed
by executing a setuid binary.

**Design response:** install with `TSYNC`; forbid setuid/file caps and userns
transitions; re-check after `exec`; treat a new thread as a policy event.

### 8. The kernel surface the filter allows

A correct filter can still leave a dangerous call allowed.  The container
escape literature is mostly this category, not filter bypass:

- `unshare(CLONE_NEWUSER)` + overlayfs / cgroups / nf_tables bug (CVE-2021-3493,
  CVE-2022-0185, CVE-2022-0492, CVE-2023-0386, CVE-2022-25636,
  CVE-2023-32233, CVE-2024-1086);
- `io_uring` kernel bugs and restriction-bypass reports (a 2026 write-up
  describes restrictions being bypassed after `exec`);
- Dirty Pipe (CVE-2022-0847) through `splice`/pipe, Dirty COW
  (CVE-2016-5195);
- `runc` escapes (CVE-2019-5736, CVE-2024-21626) that abuse a privileged
  helper or a leaked fd, not a seccomp number;
- `TIOCSTI` terminal injection into another process on the same TTY (needs
  `CAP_SYS_ADMIN` on modern kernels; Landlock's device-ioctl right can restrict
  it); `open_by_handle_at` and `pidfd_getfd` as above.

**Design response:** shrink the allowed surface, not just "block the obvious
syscalls".  No userns, no nf_tables, no io_uring, no keyrings, no `perf`, no
raw sockets, no module loading; patch the kernel; do not treat seccomp as
isolation from kernel bugs.

### 9. Userland hook evasion

`LD_PRELOAD` and a shim can be bypassed by a static binary, a raw `syscall`,
or a deliberate anti-hook technique.  On Windows the same idea is known as
direct system calls (Hell's Gate, SysWhispers); on Linux a raw `svc`/`syscall`
does the same to a preload-only sandbox.  This is why a shim alone is never an
enforcement boundary; the tracer (or a kernel filter) must be the net.

**Design response:** the shim is the fast path; the tracer is the authority
for anything security-relevant.  Do not let a guest-visible component be the
only enforcement point — the guest owns that address space.

### 10. Device and ioctl surface

Every `ioctl` is a small parser with a driver behind it.  A sandbox that allows
`ioctl` "because it is not a syscall" has moved its attack surface into the
driver.  `TIOCSTI` is the terminal example; GPU, DRM, input, audio and
networking ioctls have their own histories.

**Design response:** allow `ioctl` only by (device, command, size, direction)
table; validate every pointer and length; never pass a tracee pointer to a
provider; fuzz the parser.

## White-hat foundations worth reading

**Classic interposition and its failures**
- **Garfinkel, “Traps and Pitfalls: Practical Problems in System Call
  Interposition Based Security Tools”** (NDSS 2003) — the canonical list:
  TOCTOU, fd passing, signals, exec, races.
- **Janus** (USENIX 1996), **Ostia** (NDSS 2004), **Systrace** (Provos) — the
  systems the pitfalls were learned from.
- **Capsicum: Practical Capabilities for UNIX** (Watson et al., USENIX Security
  2010) — stop asking "which syscall" and remove the namespace; the capability
  mode is the stronger model.
- **OpenBSD pledge/unveil**, **Linux Landlock** — policy at the process and
  path layer, composed with seccomp.

**Modern fast paths and emulation**
- **Syscall User Dispatch** (Linux 5.11), **gVisor Systrap**,
  **zpoline/lazypoline (DSN 2024)**, **FlexSC (OSDI 2010)** — the cost and
  fidelity trade of interception.
- **Charliecloud, “Zero-consistency root emulation”** (arXiv 2405.06085) — a
  seccomp filter that answers privileged calls with success; the limit of
  "pretend the operation happened".
- **gVisor** — the largest working per-call kernel in user space; read its
  device models, not just its syscall table.

**Stronger boundaries**
- **seL4** (SOSP 2009), **CHERI/Morello**, **Native Client** (IEEE S&P 2009),
  **SFI** (SOSP 1993), **WebAssembly/WASI** — isolation that does not depend on
  a syscall filter being perfect.
- **Firecracker** (NSDI 2020), **crosvm/pKVM/AVF**, **Kata** — a VM boundary
  when the program must run untrusted native code.

**Primary kernel sources**
- [seccomp(2)](https://man7.org/linux/man-pages/man2/seccomp.2.html)
- [Seccomp BPF](https://docs.kernel.org/userspace-api/seccomp_filter.html)
- [seccomp_unotify(2)](https://www.man7.org/linux/man-pages/man2/seccomp_unotify.2.html)
- [Syscall User Dispatch](https://docs.kernel.org/admin-guide/syscall-user-dispatch.html)
- [Landlock](https://docs.kernel.org/userspace-api/landlock.html)
- [io_uring and seccomp](https://blog.0x74696d.com/posts/iouring-and-seccomp/)
- [A 2026 pentest survey of seccomp bypass classes](https://www.antitree.com/2026/04/seccomp-in-k8s-2/3-bypasses-and-breakouts/)

## Rules for a prefix runtime

These are the design rules this survey supports.  They are about robustness,
not about claiming a sandbox.

1. **Enforcement in the tracer, not the shim.**  The shim runs inside the
   untrusted process; it can be patched, skipped by raw syscalls, or fed
   inconsistent state.  The tracer is the net.
2. **Allow-list, with architecture.**  Kill unknown numbers; check `arch` and
   ABI bits; keep a table for arm64 and compat; refuse new calls until a
   workload and a test exist.
3. **Never decide security on a pointer.**  Copy arguments in; use fd-based
   operations (`openat2`, `O_PATH`, dirfd walks); do not `CONTINUE` a
   pointer-bearing call unless another layer will re-check the object.
4. **A supervisor is trusted; the target is not.**  ID-validate before every
   use; never let the target hold the listener fd or choose the supervisor.
5. **Filter lifecycle is part of the policy.**  `TSYNC`; no thread escapes; no
   setuid/file-caps; `no_new_privs`; re-check after `exec`.
6. **Shrink the kernel surface.**  No ptrace, userns, io_uring, keyrings, perf,
   bpf, nf_tables, raw sockets, module loading; keep the host patched.
7. **Devices are parsers.**  A synthetic device needs an ioctl table, size and
   direction checks, bounded copies, no host paths, no authority; fuzz it.
8. **The runtime is not a sandbox.**  If isolation is a requirement, use a VM
   (AVF/Firecracker), seL4/CHERI, or a real LSM; the prefix remains a
   compatibility layer.
9. **Measure the policy.**  Generate it from `strace`/workloads, test negative
   cases, fuzz with syzkaller, and keep a regression for every new call.

## What changes on a newer Android host

The host side of the same rule: the newer the Android, the less kernel surface
an app gets (stricter seccomp/SELinux, exec/W^X, fewer devices) and the more
the runtime has to answer itself.  That is a reason to keep the *runtime* the
authority and to grow emulation one row at a time — not a reason to weaken the
rules above.  The Android-specific targetSdk/exec details belong in the host
and boot specs ([`../spec/host.md`](../spec/host.md), TODO, "A prefix in any
app"), not in this security model.

## See also

[`../spec/contract.md`](../spec/contract.md) (what is promised/refused),
[`../spec/devices.md`](../spec/devices.md) (device table and providers),
[`prior-art.md`](prior-art.md), [`research.md`](research.md),
[`kernel-mechanisms.md`](kernel-mechanisms.md).
