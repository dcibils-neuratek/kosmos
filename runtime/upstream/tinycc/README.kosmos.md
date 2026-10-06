# TinyCC, vendored

Upstream: <https://repo.or.cz/tinycc.git>, branch `mob`, commit
`43c7708b85681a2fd4451c8a541af4494a8919b2` (3 October 2026), version
`0.9.28rc`, fetched on 6 October 2026.

Licence: the GNU Lesser General Public License 2.1, in `COPYING` beside
this file; `RELICENSING` records the authors who also allow their parts
under a BSD-style licence.

**Every file tracked upstream, unmodified, except two directories left
out**: `tests/`, TinyCC's own test suite, and `win32/`, its Windows port -
neither is built for Kosmos.

**What Kosmos changes is a build step**, never this tree:
`runtime/patches/tinycc/tccelf.c.patch` is applied to a copy under
`build/host/tinycc/` and compiled with `-DTCC_KOSMOS_LAYOUT` - `.text.start`
first, everything read-only joined to the code in two segments, the code a
page into the file at the base, and `__bss_start` and `__bss_end` defined -
so that what TinyCC links is an image Kosmos's loader takes
(`docs/tinycc.md`, step C1).
