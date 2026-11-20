# access()/faccessat answer the world's view (tests/bench/run.sh check, root seed).
# The kernel would judge with the app's ids: it would deny the fake root's
# override and allow the real user what the world shows as root's.
fail=0
rm -f /tmp/dn-a440 /tmp/dn-a644
touch /tmp/dn-a440 /tmp/dn-a644
chmod 440 /tmp/dn-a440
test -w /tmp/dn-a440 || { echo "access: root cannot write a 0440 file"; fail=1; }
test -r /tmp/dn-a440 || { echo "access: root cannot read a 0440 file"; fail=1; }
test -w /tmp/dn-a644 || { echo "access: root cannot write a 0644 file"; fail=1; }
if command -v python3 >/dev/null 2>&1; then
  python3 -c 'import os,sys; sys.exit(0 if os.access("", os.X_OK) else 1)' &&
    { echo "access: an empty path is executable"; fail=1; }
fi
su -s /bin/sh user -c 'test -r /etc/shadow' && { echo "access: the real user reads /etc/shadow"; fail=1; }
su -s /bin/sh user -c 'test -w /usr/bin/bash' && { echo "access: the real user writes /usr/bin/bash"; fail=1; }
su -s /bin/sh user -c 'test -r /usr/bin/bash' || { echo "access: the real user cannot read /usr/bin/bash"; fail=1; }
rm -f /tmp/dn-a440 /tmp/dn-a644
[ "$fail" = 0 ] && echo "access: ok"
exit "$fail"
