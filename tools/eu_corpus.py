#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The assembler's corpus: what Mesa's assembler makes of each line.

    python3 tools/eu_corpus.py      # writes tools/eu_corpus.txt

`user/lib/eu.lua` assembles the GPU's instructions on Kosmos
(`docs/compute.md` C1, D1), and this is what holds it to the authority:
each candidate line below is assembled for Skylake by Mesa's `gentool`
(built on the Mac by hand, `testing.md` 18.681), printed back in its
verbose canonical form, and written with its sixteen bytes. The host test
(`tools/test_eu.lua`) assembles every canonical line with `eu.lua` and
holds it to the bytes, bit for bit.

Run by hand, not by the build, because it needs `gentool`, which the build
must not: what it writes is committed, as `tools/lineicons.py`'s pictures
are. Lines gentool refuses are said and left out.
"""

import os
import re
import subprocess
import sys

import scratch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GENTOOL = os.path.join(ROOT, "build", "downloads", "mesa", "out", "gentool")
OUT = os.path.join(ROOT, "tools", "eu_corpus.txt")

# Every form the subset takes: moves and conversions, the arithmetic, the
# logic and shifts, three-source `mad`, comparisons into a flag, predication
# and `sel`, the branch, and the sends a compute kernel makes.
CANDIDATES = """
mov (16|M0) r10.0<1>:ud r0.1<0;1,0>:ud
mov (8|M0) r10.0<1>:ud r0.0<8;8,1>:ud
mov (16|M0) r5.0<1>:ud r1.0<0;1,0>:ud
mov (1|M0) r4.2<1>:ud 0x0000000f:ud
mov (2|M0) r4.0<1>:ud r2.0<2;2,1>:ud
mov (8|M0) r10.0<1>:f 0x3f800000:f
mov (16|M0) r10.0<1>:f 0x00000000:f
mov (16|M0) r5.0<1>:ud r6.0<8;8,1>:f
mov (16|M0) r5.0<1>:f r6.0<8;8,1>:d
mov (16|M0) r5.0<1>:f r6.0<8;8,1>:ud
mov (8|M0) r4.0<1>:uw 0x76543210:v
mov (16|M0) r20.0<1>:d r20.0<8;8,1>:d
mov (8|M0) r112.0<1>:ud r0.0<8;8,1>:ud
mov (4|M0) r1.0<1>:ub r1.0<0;1,0>:ub
add (16|M0) r10.0<1>:f r11.0<8;8,1>:f r12.0<8;8,1>:f
add (16|M0) r10.0<1>:d r11.0<8;8,1>:d 0x00000001:d
add (16|M0) r10.0<1>:f r11.0<8;8,1>:f -r12.0<8;8,1>:f
add (16|M0) r10.0<1>:ud r10.0<8;8,1>:ud r2.0<0;1,0>:ud
add (8|M0) r30.0<1>:f r31.0<8;8,1>:f 0x40000000:f
mul (16|M0) r20.0<1>:f r16.0<8;8,1>:f r16.0<8;8,1>:f
mul (1|M0) r2.0<1>:ud r0.1<0;1,0>:ud 0x00000010:ud
mul (16|M0) r22.0<1>:f r18.0<8;8,1>:f r1.2<0;1,0>:f
mul (16|M0) r22.0<1>:d r18.0<8;8,1>:d 0x00000003:d
mad (16) r14<0>:f r12<0;0>:f r4<0;0>:f r4<0>:f {Align16}
mad (16) r12<0>:f r1<0;0>.r:f r10<0;0>:f r1.2<0>.r:f {Align16}
mad (8) r26<0>:f r22<0;0>:f r2.4<0;0>.r:f r5<0>:f {Align16}
cmp (16|M0) (lt)f0.0 null<1>:f r24.0<8;8,1>:f 0x40800000:f
cmp (16|M0) (ge)f0.0 null<1>:f r24.0<8;8,1>:f r25.0<8;8,1>:f
cmp (16|M0) (lt)f0.0 null<1>:d r40.0<8;8,1>:d r1.4<0;1,0>:d
cmp (16|M0) (eq)f0.0 null<1>:d r40.0<8;8,1>:d 0x00000000:d
(f0.0) sel (16|M0) r30.0<1>:f r31.0<8;8,1>:f r32.0<8;8,1>:f
(f0.0) add (16|M0) r40.0<1>:d r40.0<8;8,1>:d 0x00000001:d
(f0.0) mov (16|M0) r16.0<1>:f r26.0<8;8,1>:f
(~f0.0) mov (16|M0) r16.0<1>:f r26.0<8;8,1>:f
and (16|M0) r5.0<1>:ud r6.0<8;8,1>:ud 0x000000ff:ud
or (16|M0) r5.0<1>:ud r6.0<8;8,1>:ud r7.0<8;8,1>:ud
xor (16|M0) r5.0<1>:ud r6.0<8;8,1>:ud r7.0<8;8,1>:ud
shl (16|M0) r5.0<1>:ud r6.0<8;8,1>:ud 0x00000008:ud
shr (16|M0) r5.0<1>:ud r6.0<8;8,1>:ud 0x00000010:ud
(f0.0.any16h) jmpi (1) 0x00000010:d
(f0.0.any16h) jmpi (1|M0) -64:d
jmpi (1) -64:d
send.ts (16|M0) null r112 a0.0 0x02000010 {EOT}
send.hdc1 (16|M0) acc0 r4 a0.0 0x060A8000
(f0.0) if (16|M0) jip: 32 uip: 64
else (16|M0) jip: 32 uip: 32
endif (16|M0) jip: 16
while (16|M0) jip: -48
(f0.0) while (16|M0) jip: -96
(f0.0) break (16|M0) jip: 16 uip: 32
(f0.0) continue (16|M0) jip: 16 uip: -32
(f0.0) if (8|M0) jip: 48 uip: 48
math.inv (16|M0) r12.0<1>:f r10.0<8;8,1>:f null<8;8,1>:f
math.sqt (16|M0) r12.0<1>:f r10.0<8;8,1>:f null<8;8,1>:f
math.rsq (8|M0) r12.0<1>:f r10.0<8;8,1>:f null<8;8,1>:f
math.sin (16|M0) r12.0<1>:f r10.0<8;8,1>:f null<8;8,1>:f
math.cos (16|M0) r12.0<1>:f r10.0<8;8,1>:f null<8;8,1>:f
math.log (16|M0) r12.0<1>:f r10.0<8;8,1>:f null<8;8,1>:f
math.exp (16|M0) r12.0<1>:f r10.0<8;8,1>:f null<8;8,1>:f
math.fdiv (16|M0) r12.0<1>:f r10.0<8;8,1>:f r11.0<8;8,1>:f
math.pow (16|M0) r12.0<1>:f r10.0<8;8,1>:f r11.0<8;8,1>:f
mov (8|M0) r10.0<1>:f r11.0<8;8,1>:f
add (8|M0) r10.0<1>:f r11.0<8;8,1>:f r12.0<8;8,1>:f
mad (8) r14<0>:f r12<0;0>:f r4<0;0>:f r4<0>:f {Align16}
cmp (8|M0) (lt)f0.0 null<1>:f r24.0<8;8,1>:f 0x40800000:f
(f0.0) sel (8|M0) r30.0<1>:f r31.0<8;8,1>:f r32.0<8;8,1>:f
mov (16|M0) r30.0<1>:f (abs)r31.0<8;8,1>:f
mov (16|M0) r30.0<1>:d r31.0<8;8,1>:f
rndd (16|M0) r30.0<1>:f r31.0<8;8,1>:f
frc (16|M0) r30.0<1>:f r31.0<8;8,1>:f
not (16|M0) r30.0<1>:ud r31.0<8;8,1>:ud
asr (16|M0) r30.0<1>:d r31.0<8;8,1>:d 0x00000002:d
"""


BRANCH = re.compile(r"^(\(~?f\d\.\d[^)]*\)\s*)?(jmpi|if|else|endif|while|break|continue)\b")


def run(args, data=None):
    return subprocess.run(args, input=data, capture_output=True, text=False)


def main():
    if not os.path.exists(GENTOOL):
        print("eu_corpus: no gentool at %s - see testing.md 18.681" % GENTOOL)
        return 1

    kept = []

    d = scratch.directory("eu")

    for line in [l.strip() for l in CANDIDATES.splitlines() if l.strip()]:
        asm = os.path.join(d, "a.asm")
        binf = os.path.join(d, "a.bin")

        with open(asm, "w") as f:
            f.write(line + "\n")

        if os.path.exists(binf):
            os.remove(binf)

        r = run([GENTOOL, "asm", "-p", "skl", "-i", asm, "-o", binf])

        if r.returncode != 0 or not os.path.exists(binf):
            print("eu_corpus: refused: %s - %s" % (line, r.stderr.decode().strip().splitlines()[-1:]))
            continue

        raw = open(binf, "rb").read()

        if len(raw) != 16:
            print("eu_corpus: not one instruction: %s (%d bytes)" % (line, len(raw)))
            continue

        # A branch is kept as written: its targets are offsets in bytes,
        # which gentool prints as labels and will not print at all when they
        # fall outside a one-line program.
        if BRANCH.match(line):
            kept.append((line, raw.hex()))
            continue

        dis = run([GENTOOL, "disasm", "-p", "skl", "-v", binf]).stdout.decode().strip()
        canon = dis.split("//")[0].strip()

        # The canonical form must assemble to the same bytes again.
        with open(asm, "w") as f:
            f.write(canon + "\n")

        os.remove(binf)
        r = run([GENTOOL, "asm", "-p", "skl", "-i", asm, "-o", binf])

        if r.returncode != 0 or open(binf, "rb").read() != raw:
            # A branch is printed with a label or a bare number gentool
            # reads back differently; eu.lua takes the line as written,
            # with its offset in bytes, so that is what is kept.
            # And a one-source `math`, whose null second source gentool
            # prints without the region it reads back.
            if BRANCH.match(line) or (line.startswith("math.") and line.endswith("null<8;8,1>:f")):
                kept.append((line, raw.hex()))
                continue

            print("eu_corpus: does not round-trip: %s -> %s" % (line, canon))
            continue

        kept.append((canon, raw.hex()))

    with open(OUT, "w") as f:
        f.write("# The assembler's corpus, written by tools/eu_corpus.py from Mesa 26.2.5's\n")
        f.write("# gentool for Skylake: a canonical line, a bar, its sixteen bytes in order.\n")

        for canon, hx in kept:
            f.write("%s | %s\n" % (canon, hx))

    print("eu_corpus: %d lines into %s" % (len(kept), OUT))
    return 0


if __name__ == "__main__":
    sys.exit(main())
