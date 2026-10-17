# The owner store: chown is kept in user.dn.owner and every stat answers it.
# Run inside a prefix (tests/bench/run.sh check).
fail=0
t() { [ "$1" = "$2" ] || { echo "owner: $3: $1, expected $2"; fail=1; }; }

rm -rf /tmp/own-d
mkdir /tmp/own-d
touch /tmp/own-d/a
chown 1000:1001 /tmp/own-d/a
t "$(stat -c %u:%g /tmp/own-d/a)" 1000:1001 single

chown -R 4321:4322 /tmp/own-d
t "$(stat -c %u:%g /tmp/own-d)" 4321:4322 directory
t "$(stat -c %u:%g /tmp/own-d/a)" 4321:4322 recursive

chown 0:0 /tmp/own-d/a
t "$(stat -c %u:%g /tmp/own-d/a)" 0:0 reset

ln /tmp/own-d/a /tmp/own-d/c                 # the copy carries the owner
t "$(stat -c %u:%g /tmp/own-d/c)" 0:0 link-carries
chown 1111:1112 /tmp/own-d/c
t "$(stat -c %u:%g /tmp/own-d/c)" 1111:1112 link-chown
t "$(stat -c %u:%g /tmp/own-d/a)" 0:0 link-copy-separate

rm -rf /tmp/own-d
[ "$fail" = 0 ] && echo "owner: ok"
exit "$fail"
