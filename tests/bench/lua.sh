#!/bin/sh
# The Lua 5.4 build wall (docs/performance.md, "In an app session"): clean,
# make -j1, run it, print the wall.  LUA_DIR is a guest path; the default is
# where the work prefixes keep it.
d=${LUA_DIR:-/home/bench/lua-5.4.7}
cd "$d" || { echo "lua.sh: no Lua source at $d (set LUA_DIR)" >&2; exit 1; }
make clean >/dev/null 2>&1
s=$(date +%s%N)
make -j1 linux >/dev/null 2>&1 || { echo "lua.sh: make failed" >&2; exit 1; }
e=$(date +%s%N)
./src/lua -e 'print("LUA_OK", _VERSION)'
awk -v a="$s" -v b="$e" 'BEGIN { printf "wall %d ms\n", (b - a) / 1000000 }'
