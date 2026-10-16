#!/bin/sh
# The fingerprint probe: print KEY=value for the kernel, the hardware and the
# runtime's traces, so scripts/fingerprint.sh can compare them (docs/spec/phase2.md).

echo "uname_s=$(uname -s 2>/dev/null)"
echo "uname_r=$(uname -r 2>/dev/null)"
echo "uname_m=$(uname -m 2>/dev/null)"
echo "uname_o=$(uname -o 2>/dev/null)"
echo "version=$(head -c 120 /proc/version 2>/dev/null | tr -d '\n')"
echo "cpu_model=$(sed -n 's/^model name[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)"
echo "cpu_hw=$(sed -n 's/^Hardware[[:space:]]*:[[:space:]]*//p' /proc/cpuinfo 2>/dev/null | head -1)"
echo "state=$(sed -n 's/^State:[[:space:]]*//p' /proc/self/status 2>/dev/null)"
echo "name=$(sed -n 's/^Name:[[:space:]]*//p' /proc/self/status 2>/dev/null)"
echo "tracerpid=$(sed -n 's/^TracerPid:[[:space:]]*//p' /proc/self/status 2>/dev/null)"
echo "seccomp=$(sed -n 's/^Seccomp:[[:space:]]*//p' /proc/self/status 2>/dev/null)"
echo "uid=$(id -u 2>/dev/null)"
echo "arch=$(dpkg --print-architecture 2>/dev/null)"
echo "os_id=$(sed -n 's/^ID=//p' /etc/os-release 2>/dev/null | head -1)"
