o=/tmp/reg-$1; rm -rf $o; mkdir -p $o
ls -lR /usr/share/doc > $o/ls 2>&1
find /usr /etc -printf '%p %s %u %y %l\n' 2>&1 | sort > $o/find
find -L /etc/alternatives -maxdepth 1 -printf '%p %y %s\n' 2>&1 | sort > $o/findL
for f in /bin /bin/sh /etc/alternatives/awk /usr/lib/aarch64-linux-gnu/libc.so.6 /etc/resolv.conf /nonexist /usr/bin/../lib; do
  echo "$f: $(stat -c '%s %U %G %F' $f 2>&1) | $(stat -L -c '%s %F' $f 2>&1) | $(readlink $f 2>&1) | $(readlink -f $f 2>&1) | $(realpath -s $f 2>&1)"
done > $o/stat
readlink /proc/self/exe > $o/exe
(cd /bin && pwd -P && pwd && cd /etc/alternatives && pwd -P && python3 -c 'import os; print(os.getcwd())') > $o/cwd 2>&1
python3 - > $o/py 2>&1 <<'PY'
import os
d = os.open('/etc/alternatives', os.O_RDONLY | os.O_DIRECTORY)
for n in ('awk', 'pager', 'nonexist'):
    try:
        fd = os.open(n, os.O_RDONLY, dir_fd=d); print(n, 'open', os.read(fd, 4))
    except OSError as e: print(n, 'open ERR', e.errno)
    for f in (True, False):
        try: st = os.stat(n, dir_fd=d, follow_symlinks=f); print(n, 'stat', f, st.st_size, st.st_uid, oct(st.st_mode))
        except OSError as e: print(n, 'stat ERR', f, e.errno)
    try: print(n, 'readlink', os.readlink(n, dir_fd=d))
    except OSError as e: print(n, 'readlink ERR', e.errno)
    print(n, 'access', os.access(n, os.R_OK, dir_fd=d))
u = os.open('/usr', os.O_RDONLY | os.O_DIRECTORY)
for n in ('bin/awk', 'lib/../bin/make', '../etc/hostname', 'share/doc'):
    try: st = os.stat(n, dir_fd=u); print(n, 'stat', st.st_size, oct(st.st_mode))
    except OSError as e: print(n, 'stat ERR', e.errno)
p = os.open('/proc/self', os.O_RDONLY | os.O_DIRECTORY)
print('proc rel', os.readlink('exe', dir_fd=p).endswith('python3.13'), os.stat('status', dir_fd=p).st_size)
print('xattr', sorted(os.listxattr('/usr/bin')), os.getxattr('/usr/bin', 'security.selinux')[:2])
PY
(cd /usr/share && tar cf - doc | md5sum) > $o/tar
cp -a /usr/share/doc/bash /tmp/cpa-$1 && diff -r /usr/share/doc/bash /tmp/cpa-$1 && echo same > $o/cp; rm -rf /tmp/cpa-$1
md5sum $o/* | sed "s#$o/##"
