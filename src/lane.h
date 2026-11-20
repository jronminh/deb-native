/* lane.h -- the shim's lane, shared by the tracer's filter (catch.c) and the shim
 * (lane.c), and nothing else: it includes no header, so a libc build can use it.
 *
 * A call the shim makes itself, its path already resolved, carries this value in x5
 * (a register none of these calls uses), and the filter lets it through without a
 * stop.  Only calls that read take the lane, so everything that changes the tree (and
 * the tracer's copies of it) still reaches the tracer. */
#ifndef LANE_H
#define LANE_H

#define DN_LANE_LO 0xd7f2a541u
#define DN_LANE_HI 0x9e1ab6c3u
#define DN_LANE_NRS { 78, 79, 80, 48, 291, 9, 12, 8, 11, 17, \
                      34, 33, 53, 88, 27, 43, 45, 5, 6, 14, 15 }   /* readlinkat newfstatat fstat
                                                           * faccessat statx lgetxattr llistxattr
                                                           * getxattr listxattr getcwd, and the
                                                           * path-only writes the shim does now:
                                                           * mkdirat mknodat fchmodat utimensat
                                                           * inotify_add_watch statfs truncate
                                                           * set/lset/remove/lremove xattr */
/* openat takes the lane only to read: none of O_WRONLY, O_RDWR, O_CREAT, O_TRUNC,
 * O_APPEND in its flags (x2); O_TMPFILE needs write access, so it is excluded too */
#define DN_LANE_OPENAT 56
#define DN_LANE_OPEN_DENY 0x643u

#endif
