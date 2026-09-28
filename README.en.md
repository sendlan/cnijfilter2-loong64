# cnijfilter2 — Canon IJ Printer Driver, Fully Native for loong64

[中文](README.md) · **English**

A native LoongArch rebuild of Canon's `cnijfilter2`. The repository contains no
non-LoongArch executables, needs no emulation layer at runtime, and refers to no
directory outside itself.

| | |
|---|---|
| Target | Loongnix 25 / LoongArch 3A6000 (also usable on other loong64 distros) |
| Upstream source | Canon `cnijfilter2` 6.90-1 (`cnijfilter2-source-6.90-1.tar.gz`) |
| Behavioral target | Official 6.90 runtime behavior, byte for byte |
| Artifact | `packages/cnijfilter2_6.90-6_loong64.deb` (180 KB) |
| License | GPL-2 (upstream source and PPDs are GPL-2; our changes are GPL-2.0-or-later) |

The **7 factory modules** (`lgmon3` `tocanonij` `tocnpwg` `cnijbe2`
`rastertocanonij` `cmdtocanonij2` `cmdtocanonij3`) are built from upstream source;
all porting changes are in `build/patches/loong64-native.patch`.

The **4 closed-source libraries are reimplemented from scratch** (Canon ships
binaries only): `libcnnet2` `libcnbpnet20` `libcnbpnet30` `libcnbpcnclapicom2` —
see `cncl/` and `cnnet/`.

---

## Build

Dependencies and full instructions: [`build/docs/BUILD.md`](build/docs/BUILD.md).

```bash
sudo apt install --yes gcc make autoconf automake libtool pkg-config \
     libcups2-dev libcupsimage2-dev libusb-1.0-0-dev libxml2-dev

bash build/build-all.sh                                    # build all 11 artifacts
bash build/make-deb.sh                                     # → packages/
echo <password> | sudo -S -p '' -E apt install --yes ./packages/*.deb
```

Build a single module: `bash build/build-all.sh lgmon3`
Package only (`build/built/` is already a snapshot): `bash build/make-deb.sh`

---

## Verification

```bash
/usr/bin/python3 tests/invariants.py           # invariants (zero reference)   31/31
/usr/bin/python3 tests/sweep/regress.py        # baseline · print path        66/66
/usr/bin/python3 tests/sweep/mnt-regress.py    # baseline · maintenance path    3/3
/usr/bin/python3 tests/sweep/selftest-guard.py # guard adversarial self-test 11/11
bash tests/check-native-only.sh                # delivery-surface self-proof
```

**Invariants** (`invariants.py`): properties that can be judged true or false
without any external reference — determinism, validity, arguments actually taking
effect, coverage. It still passes with `tests/sweep/` deleted.

**Baseline** (`tests/sweep/baseline*.tsv`): output fingerprints of the official
upstream implementation (normalized length + first 16 hex digits of SHA-256), read
on every regression run. Tracked in git and shipped with the repository.

**The reference can be produced on pure loong64** — no emulator, no official
binaries:

```bash
/usr/bin/python3 tests/sweep/selfref.py --outdir ~/refl --emit-refdir ~/ref
/usr/bin/python3 tests/sweep/relock.py --refdir ~/ref --reason "why the baseline must change"
```

It builds `rastertocanonij` / `tocnpwg` / `cmdtocanonij3` from the upstream source
in this repository with native gcc and runs the full reference set. Justification:
`rastertocanonij/README` states it emits the Canon command stream, `tocnpwg/README`
states it depends on `libcups` only, and `cmdtocanonij3/LICENSE` carries the GPL
exception permitting linkage against binary modules. Measured, its locked results
are **byte-identical** to a reference taken from the officially released binaries.

**Three locks** (`baseline_guard.py`) keep the baseline from being silently
rewritten: the `integrity` field at the end of each table checks itself, a
`BASELINE_DIGEST` inside the scripts cross-verifies, and `BASELINE-LOG.md` keeps an
append-only fingerprint chain. Any failing layer aborts immediately. Changing the
baseline always leaves an explicit diff in 3 files, visible at a glance in review.

---

## Layout

```
cnijfilter2/
├── build/                        build and packaging
│   ├── build-all.sh / build-one.sh / make-deb.sh
│   ├── verify-net-print.sh       packet-capture check of the network print path (root + online printer)
│   ├── test-postinst-layout.sh   postinst layout-branch self-test (needs sudo)
│   ├── docs/BUILD.md             ★ build guide
│   ├── patches/loong64-native.patch   every change against factory 6.90-1
│   └── pkg/{control,postinst,postrm}  deb metadata
│
├── cncl/      our CNCL API library       ├── cnnet/   our network transport library
│
├── lgmon3/ tocanonij/ tocnpwg/           ┐
├── cnijbe2/ rastertocanonij/             │ factory source modules (6.90-1)
├── cmdtocanonij2/ cmdtocanonij3/         ┘
│
├── com/ini/cnnet.ini             factory configuration
├── ppd/                          179 PPDs (full official 6.90 set, GPL-2) + NEWS
├── packages/                     ★ the one place build artifacts land
├── tests/
│   ├── small.raster              test input (shared by both suites)
│   ├── params.py                 single source of the parameter matrix
│   ├── invariants.py             ★ invariants: self-judging tests (zero reference)
│   ├── check-native-only.sh      ★ self-proof: zero foreign-architecture traces
│   └── sweep/                    ★ baseline: fidelity regression
│       ├── regress.py / mnt-regress.py    print path 66 combinations / maintenance path 3 jobs
│       ├── selfref.py            self-produced reference (builds reference artifacts natively from in-repo upstream source)
│       ├── relock.py             relock (read reference dir → byte compare → write baseline)
│       ├── baseline_guard.py / selftest-guard.py   three locks / guard adversarial self-test
│       ├── BASELINE-LOG.md       baseline change ledger (append-only)
│       └── baseline.tsv / baseline-mnt.tsv   the baseline
└── README.md / README.en.md / LICENSE / NOTICE.md
```

---

## Source and resource versions

**Source is the full 6.90-1**, same version as the PPDs. All 7 factory modules come
from `cnijfilter2-source-6.90-1.tar.gz` with only the 8 porting changes in
`build/patches/loong64-native.patch` applied (8 files, +132/-21 lines).

The 179 PPDs are likewise the complete 6.90 set. `*%CNSizeToPrintArea` and
`*ImageableArea` are self-contained in each PPD (179/179), with nothing to add
from outside.

---

## Key facts and gotchas

**Network path**: discovery = SNMP v1 (community `canon_admin`, Canon enterprise
number 1602) → printing = raw TCP 9100 → status = CHMP over TCP 80 (hard-requires
`X-CHMP-Version: 1.0.0`; **POST only returns a 200 empty ack, you must GET the same
URL afterwards to get the chunked body**). Broadcast gets dropped by conntrack, so
it degrades to unicast ARP plus a /24 scan.

**CNCL semantics**: the official headers declare `CNCL_GetInfoResponse` /
`CNCL_GetStatus` as `unsigned short`, but **they actually return full 32-bit
negative values** → the implementation returns `int`, otherwise `err < 0` checks
break; for the 5 dictionary tables, **the index is the return value**. Job
completion: once jobinfo disappears, `GetStatus2` must return **10** — returning 0
forever makes the job never finish.

**Build notes**: the old autotools trees need the manual command sequence (do not
use `./autogen.sh`); gcc 10 and later need an explicit `-fcommon`; our libraries use
symbol version scripts (`.map`) to constrain exported prefixes (otherwise the three
libraries each export 62 same-named symbols and clobber each other); the `libxml2`
soname split (`.so.2` / `.so.16`) is handled with dual dlopen probing.

---

## License and self-audit

**All source is under GPL-2 and may be redistributed freely.** The root
[`LICENSE`](LICENSE) is the full GPL-2.0 text (byte-identical to the gnu.org
original). Upstream source is GPL-2 (each of the 7 factory modules ships the full
`COPYING`; `lgmon3/` and `cnijbe2/` also carry Canon's `LICENSE.canon.txt`); each
of the 179 PPDs under `ppd/` declares the GPL-2 in its own header; our new and
modified code is GPL-2.0-or-later. See [`NOTICE.md`](NOTICE.md) for the layer
breakdown and the audit commands.

The only executables in this repository are LoongArch artifacts built from this
repository's own source. To verify (expect `LoongArch` and nothing else):

```bash
find . -not -path './.git/*' -type f | while read -r f; do
    head -c4 "$f" 2>/dev/null | grep -q ELF && \
    readelf -h "$f" 2>/dev/null | sed -n 's/.*\(System\|Machine\): *//p'
done | sort -u
```
