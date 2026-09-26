#!/bin/sh
# Print Termux's floor (docs/true-fusion.md, "Package tiers", tier 0): the
# installed non-arm64 packages marked "Essential: yes", plus everything they
# depend on (Depends/Pre-Depends, first installed alternative, through
# Provides). These are what the Termux app, apt and dpkg run on; the fused
# system never removes or crossgrades them. One package name per line.
#
# Usage: dn-floor.sh [STATUS_FILE]
set -eu
P=${PREFIX:-/data/data/com.termux/files/usr}
STATUS=${1:-$P/var/lib/dpkg/status}
awk -v RS= -F '\n' '
  function field(name,   i, v) {
    for (i = 1; i <= NF; i++)
      if (index($i, name ": ") == 1) return substr($i, length(name) + 3)
    return ""
  }
  {
    pkg = field("Package"); arch = field("Architecture")
    if (pkg == "" || arch == "arm64" || field("Status") !~ / installed$/) next
    inst[pkg] = 1
    deps[pkg] = field("Pre-Depends") ", " field("Depends")
    if (field("Essential") == "yes") { floor[pkg] = 1; queue[++qn] = pkg }
    np = split(field("Provides"), pv, ",")
    for (i = 1; i <= np; i++) { p = pv[i]; sub(/^ +/, "", p); sub(/ .*$/, "", p); if (p != "" && !(p in prov)) prov[p] = pkg }
  }
  END {
    for (qi = 1; qi <= qn; qi++) {
      n = split(deps[queue[qi]], alts, ",")
      for (i = 1; i <= n; i++) {
        m = split(alts[i], choice, "|")
        for (j = 1; j <= m; j++) {
          c = choice[j]; sub(/^ +/, "", c); sub(/ .*$/, "", c); sub(/:.*/, "", c)
          t = (c in inst) ? c : ((c in prov) ? prov[c] : "")
          if (t != "") { if (!(t in floor)) { floor[t] = 1; queue[++qn] = t }; break }
        }
      }
    }
    for (p in floor) print p
  }' "$STATUS" | sort
