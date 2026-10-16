#!/bin/sh
# The coverage probe: print the tree's view of every surface a program can read a
# coordinate from (docs/spec/coverage.md).  Run by scripts/coverage.sh inside a
# prefix; every line is compared against the host frame there.

echo "## getcwd"
pwd -P 2>/dev/null
pwd 2>/dev/null

echo "## readlink exe"
readlink /proc/self/exe 2>/dev/null

echo "## readlink cwd"
readlink /proc/self/cwd 2>/dev/null

echo "## readlink fd"
readlink /proc/self/fd/0 2>/dev/null
readlink /proc/self/fd/1 2>/dev/null

echo "## readlink link"
readlink /etc/alternatives/awk 2>/dev/null
realpath /bin/sh 2>/dev/null

echo "## stat"
stat -c '%U %G %n' /etc /etc/passwd /usr 2>/dev/null

echo "## ids"
id -u 2>/dev/null
id -g 2>/dev/null

echo "## mounts"
cat /proc/self/mounts 2>/dev/null

echo "## mountinfo"
cat /proc/self/mountinfo 2>/dev/null

echo "## mtab"
cat /etc/mtab 2>/dev/null

echo "## maps"
cat /proc/self/maps 2>/dev/null

echo "## smaps"
head -40 /proc/self/smaps 2>/dev/null

echo "## df"
df / 2>/dev/null

echo "## findmnt"
findmnt 2>/dev/null

echo "## python"
if command -v python3 >/dev/null 2>&1; then
	python3 - <<'PY'
import os
print("getcwd", os.getcwd())
print("realpath", os.path.realpath("/usr/bin/../lib"))
print("stat uid", os.stat("/etc").st_uid)
try:
    print("xattr", sorted(os.listxattr("/usr")))
except OSError as e:
    print("xattr err", e.errno)
d = os.open("/etc", os.O_RDONLY | os.O_DIRECTORY)
print("dirfd", os.stat("passwd", dir_fd=d).st_size)
PY
fi
