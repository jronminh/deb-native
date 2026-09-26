/* -*- c-set-style: "K&R"; c-basic-offset: 8 -*-
 *
 * This file is part of PRoot.
 *
 * Copyright (C) 2015 STMicroelectronics
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA.
 */

#ifndef ARCH_H
#define ARCH_H

#ifndef NO_LIBC_HEADER
#include <sys/ptrace.h>    /* linux.git:c0a3a20b  */
#include <linux/audit.h>   /* AUDIT_ARCH_*,  */
#endif

typedef unsigned long word_t;
typedef unsigned char byte_t;

#define SYSCALL_AVOIDER ((word_t) -2)
#define SYSTRAP_NUM SYSARG_NUM

/* fork-lite: AArch64 only -- the multi-arch branches are pruned. */
#if !defined(__aarch64__)
#    error "fork-lite supports AArch64 only"
#endif
#define ARCH_ARM64 1

/* Architecture specific definitions. */
#if defined(ARCH_ARM64)

    #define SYSNUMS_HEADER1 "syscall/sysnums-arm64.h"

    #define SYSNUMS_ABI1    sysnums_arm64

    #define SYSTRAP_SIZE 4

    #ifndef AUDIT_ARCH_AARCH64
        #define AUDIT_ARCH_AARCH64 (EM_AARCH64 | __AUDIT_ARCH_64BIT | __AUDIT_ARCH_LE)
    #endif

    #define SECCOMP_ARCHS {									\
		{ .value = AUDIT_ARCH_AARCH64, .nb_abis = 1, .abis = { ABI_DEFAULT } },	\
	}

    #define HOST_ELF_MACHINE {183, 0};
    #define RED_ZONE_SIZE 0
    #define OFFSETOF_STAT_UID_32 24
    #define OFFSETOF_STAT_GID_32 28

    #define LOADER_ADDRESS     0x2000000000
    #define EXEC_PIC_ADDRESS   0x3000000000
    #define INTERP_PIC_ADDRESS 0x3f00000000
    #define HAS_POKEDATA_WORKAROUND true

    /* Syscall -2 appears to cause some odd side effects, use -1. */
    /* See https://github.com/termux/termux-packages/pull/390 */
    #undef SYSCALL_AVOIDER
    #define SYSCALL_AVOIDER ((word_t) -1)

#else

    #error "Unsupported architecture"

#endif

#endif /* ARCH_H */
