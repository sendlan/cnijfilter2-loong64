# cnijfilter2 — Canon IJ Printer Driver, Fully Native for loong64

[中文](README.md) · **English**

A native LoongArch rebuild of Canon's `cnijfilter2`: every ELF is built for this
architecture, no emulation layer involved.

| | |
|---|---|
| Target | Loongnix 25 / LoongArch 3A6000 (also usable on other loong64 distros) |
| Upstream source | Canon `cnijfilter2` 6.90-1 (`cnijfilter2-source-6.90-1.tar.gz`) |
| Behavioral target | Official 6.90 runtime behavior, byte for byte |
| Artifact | `packages/cnijfilter2_6.90-6_all.deb` (180 KB) — `Architecture: all`, one package installs on both `loong64` (Debian/Loongnix) and `loongarch64` (AOSC) |
| License | GPL-2 (upstream source and PPDs are GPL-2; our changes are GPL-2.0-or-later) |

The **7 factory modules** (`lgmon3` `tocanonij` `tocnpwg` `cnijbe2`
`rastertocanonij` `cmdtocanonij2` `cmdtocanonij3`) are built from upstream source;
all porting changes are in `build/patches/loong64-native.patch`.

**4 libraries are native implementations**: `libcnnet2` `libcnbpnet20`
`libcnbpnet30` `libcnbpcnclapicom2` — see `cncl/` and `cnnet/`.

---

## Build

Dependencies and full instructions: [`build/docs/BUILD.md`](build/docs/BUILD.md).

```bash
sudo apt install --yes gcc make autoconf automake libtool pkg-config \
     libcups2-dev libcupsimage2-dev libusb-1.0-0-dev libxml2-dev

bash build/build-all.sh                                    # build all 11 artifacts
bash build/make-deb.sh                                     # → packages/ (a single "all" package)
echo <password> | sudo -S -p '' -E apt install --yes ./packages/*.deb
```

The `Architecture: all` package covers both arch names: `loong64`
(Debian/Loongnix) and `loongarch64` (AOSC) are the same binaries, differing only
in the dpkg field; `preinst` rejects non-LoongArch hosts. For per-arch packages:

```bash
ARCHS="loong64 loongarch64" bash build/make-deb.sh   # two per-arch packages
ARCH=loong64                bash build/make-deb.sh   # loong64 only
DEPS="cups, libcups2, libc6" bash build/make-deb.sh  # override Depends
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

- `invariants.py` (invariants): zero reference, self-contained criteria.
- `sweep/baseline*.tsv` (baseline): output fingerprints matched against the
  official implementation, shipped with the repository.
- After changing sources, rebuild the baseline: `selfref.py` builds reference
  executables natively, `relock.py` locks them in (see BUILD.md).
- `baseline_guard.py` keeps the baseline from being silently rewritten.

---

## Layout

```
cnijfilter2/
├── build/                        build and packaging
│   ├── build-all.sh / build-one.sh / make-deb.sh
│   ├── verify-net-print.sh       online check of the network print path (root + online printer)
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

Source is the full official 6.90-1 (same version as the PPDs, 179 PPDs complete),
with only the 8 porting changes from `build/patches/loong64-native.patch`;
printable areas are self-contained per PPD.

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

All source is under GPL-2: the root [`LICENSE`](LICENSE) is the full GPL-2.0 text,
upstream modules ship their `COPYING`, each PPD declares GPL-2 in its header, and
our new/modified code is GPL-2.0-or-later. Layer breakdown: [`NOTICE.md`](NOTICE.md).

The only executables in this repository are LoongArch artifacts built from this
repository's own source. To verify (expect `LoongArch` and nothing else):

```bash
find . -not -path './.git/*' -type f | while read -r f; do
    head -c4 "$f" 2>/dev/null | grep -q ELF && \
    readelf -h "$f" 2>/dev/null | sed -n 's/.*\(System\|Machine\): *//p'
done | sort -u
```
