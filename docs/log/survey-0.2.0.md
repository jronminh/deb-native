# Survey 0.2.0: 100 Debian packages in the prefix

<!-- template: templates/docs.template.md -->

The first survey of the 0.2.0 prefix: real Debian packages installed with the
prefix's own apt and their programs run by name, the method of sudo-less's
[`survey.md`](https://github.com/jronminh/sudo-less/blob/main/docs/survey.md)
(same install and run classes). "Supported" is
[`standard.md`](../reference/standard.md)'s: in scope, installed to `ii`, and its
programs run from the prefix by name.

Raw data in [`survey-0.2.0/`](survey-0.2.0/): `list.tsv` + `results.tsv`
(the main run), `list-run1.tsv` + `results-run1.tsv` (the first, unfiltered
run, stopped after 10). Columns: `scripts/survey/survey-prefix.sh`'s header.

## Contents

- [Method](#method)
- [Results (main run, lightweight)](#results-main-run-lightweight)
- [Heavier packages (run 1, unfiltered)](#heavier-packages-run-1-unfiltered)
- [What this says](#what-this-says)

## Related docs

- [`findings/first-random-sample-survey.md`](findings/first-random-sample-survey.md)
  — the earlier, 0.1.x-era survey (2 of 30 installed) this run's
  99-of-100 result should be read against.
- [`../reference/standard.md`](../reference/standard.md) — the scope this survey's
  package selection follows.
- [`../../scripts/survey/README.md`](../../scripts/survey/README.md) —
  the scripts that produced this run and its raw data.

## Method

- **Device:** `fe2`, vanilla Termux + `make`, `libtalloc`; fresh
  `install.sh` of `dev-0.2.0` (3m13s, 23 packages), 2026-09-27.
- **Archive:** Debian 13.7 "trixie", arm64, `main`.
- **Sample** (`scripts/survey/survey-sample.py`): 100 packages, seeded random,
  spread evenly over the 43 in-scope sections, without the admin signals
  (Priority required/important/standard, Essential, a dependency on
  `adduser` or `init-system-helpers`). The main run is **lightweight**: a
  package plus the dependencies apt would add must stay under 1.5 MB.
- **Per package** (`scripts/survey/survey-prefix.sh`): the prefix restored from a
  snapshot, `apt-get install -y --no-install-recommends`, then up to 6
  programs from its bin dirs run by name with `--version`, else `--help`,
  no display. How each is reached is recorded: `native` (a symlink: ld-dn
  or a translated `#!`), `trace`, `dn-run`, `script`, `hidden`; a failing
  program is retried under the tracer.
- 52 minutes for the 100.

## Results (main run, lightweight)

| install | packages |
|---|---|
| **ok** | **99** |
| maintainer script failed | 1 (`nethack-common`) |

| run (of the 99) | packages |
|---|---|
| no program (libraries, data, docs, fonts) | 73 |
| **ok** | **18** |
| partial | 3 |
| fail | 3 |
| untested (needs a terminal) | 2 |

**How programs were reached:** 48 programs ran; 47 **native** (ld-dn, no
wrapper), 1 through `dn-run` (`gorst`), **none needed the tracer**.

### Every failure

| package / program | cause | deb-native's? |
|---|---|---|
| `camv-rnd-core`: `camv-rnd` | `librnd-hid.so.4` not found: librnd installs to `/usr/lib`, which Termux's ld.so does not search | **yes — fixed** |
| `nethack-common` | postinst calls `update-rc.d` (and `deb-systemd-helper`): no such command | **yes — fixed** |
| `unlambda` | an interpreter reading its program from stdin: "Parse error at end of file" on empty input | no (survey limit) |
| `poa` | runs, wants a matrix file | no (survey limit) |
| `phipack`: `phipack-ppma_2_bmp` | runs, prints its banner, wants input files | no (survey limit) |
| `yodl`: `yodlverbinsert` | runs, rejects `--version`/`--help` | no (survey limit) |
| `libts-bin`: `ts_finddev` | runs, looks for a touchscreen device | no (hardware) |
| `perl-openssl-defaults`: `dh_perl_openssl` | a debhelper add-on; debhelper is not installed | no (not a dependency) |
| `bastet` | needs a terminal | no (untested) |

**Lightweight packages: 98 of 100 install and run within the survey's
limits; the other 2 are fixed** (both re-tested on `fe2`: `camv-rnd
--version` runs, `nethack-common` reaches `ii`).

## Heavier packages (run 1, unfiltered)

The first run used the same sections without the size filter and was
stopped after 10 packages. It hit three issues the lightweight sample
avoids, because they live in large dependency chains:

| issue | packages | status |
|---|---|---|
| `passwd`'s postinst: `getent group shadow` reaches Termux's glibc getent, whose NSS reads `$PREFIX/glibc/etc`, not the prefix's `/etc`; `groupadd` then aborts | `ipp-usb`, `ceph-immutable-object-cache-dbg` | **fixed** (priv `getent`) |
| `perl-base` ships a hard link (`perl5.40.1`); Android refuses `link(2)` in app data | `dibbler-client-dbg` | **fixed** (hard links become copies) |
| `libc6-dev` needs `libc6 (= 2.41-12+deb13u4)`; the stand-in is `2.44-0dn1` | `libghc-asn1-parse-doc` (via `ghc`): **every toolchain** | **open**: the stand-in should carry Debian's exact `libc6` identity ([`TODO.md`](../../TODO.md)) |

Also seen: `81voltd` runs natively, then needs the system D-Bus (a
service); `poxml`'s `swappo` prints nothing on `--version`/`--help`.

## What this says

- The install path works: 99 of 100 lightweight packages reach `ii`
  through translation, the stand-ins and the priv layer.
- ld-dn carries the runtime: every program that ran, ran natively; the
  tracer was not needed in this sample.
- The limits are in the heavy chains: toolchains (the `libc6` identity),
  and Perl modules with C parts (`dn-perl` runs Termux's Perl 5.42, Debian
  trixie builds for 5.40) — not yet measured.
- Next survey: the unfiltered sample after the `libc6` identity, to put a
  number on heavy packages.
