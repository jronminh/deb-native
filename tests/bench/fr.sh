echo "id: $(id)"
echo "stat(statx) /usr/bin: $(stat -c '%U:%G %u' /usr/bin)  link size: $(stat -c %s /etc/alternatives/awk) (target $(readlink /etc/alternatives/awk | wc -c) -1)"
ls -ln /usr/bin/bash | cut -d' ' -f3,4
python3 - <<'PY'
import os, subprocess
f = open('/etc/hostname' if os.path.exists('/etc/hostname') else '/etc/passwd')
print('py stat', os.stat('/usr').st_uid, 'lstat', os.lstat('/bin').st_uid, 'fstat', os.fstat(f.fileno()).st_uid, 'ids', os.getresuid(), os.getresgid())
os.setresgid(100, 100, 100); os.setresuid(1000, 1000, 1000)
print('after setresuid', os.getuid(), os.geteuid(), os.getresuid(), os.getgid())
pid = os.fork()
if pid == 0:
    print('child', os.getuid(), os.getegid()); os._exit(0)
os.waitpid(pid, 0)
print('exec id -u:', subprocess.run(['id', '-u'], capture_output=True, text=True).stdout.strip())
PY
apt-get -s install hello 2>&1 | tail -1
dpkg -l | wc -l
