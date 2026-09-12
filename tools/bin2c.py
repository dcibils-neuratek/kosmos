#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
Turns a file into a C array.

Used for two things that both have to travel inside the kernel image: the
init process's binary, and the Lua source it runs. There is no filesystem to
load either from until M8, and no namespace to ask for one until M5, so the
image carries them.

An assembly `.incbin` would be shorter. A C array is used instead because it
goes through the same compiler as everything else, which means the length is
a real symbol rather than a linker-script convention, and because a missing
file is a build error here rather than a zero-length section discovered at
run time.

**And beside the bytes, a record of what they were.**

The init image is the one blob in this system that every process runs out
of: `process_create` maps its read-only half straight out of the kernel's
own copy, so one set of physical pages carries the code, the fonts and the
Lua source of every program on the machine. Nothing can write a byte of it
through that mapping - and on the ThinkPad it arrives corrupted anyway, with
the only symptom a Lua syntax error in whichever server happened to parse
first.

A checksum computed here is the only reference there is. The bytes in RAM
can be compared against it at any moment of the boot, which turns "something
is wrong with the source" into "these bytes changed, between these two
points". Per page as well as in total, because the page is what says *where*
- and an address is what can be looked for in the memory map.

FNV-1a, which is four lines and needs no table. This is a canary, not a
signature: nothing here is defending against an adversary choosing the bytes.
"""

import sys

FNV64_BASIS = 0xcbf29ce484222325
FNV64_PRIME = 0x100000001b3
FNV32_BASIS = 0x811c9dc5
FNV32_PRIME = 0x01000193

PAGE = 4096


def fnv64(data):
    h = FNV64_BASIS
    for b in data:
        h = ((h ^ b) * FNV64_PRIME) & 0xffffffffffffffff
    return h


def fnv32(data):
    h = FNV32_BASIS
    for b in data:
        h = ((h ^ b) * FNV32_PRIME) & 0xffffffff
    return h


def main():
    if len(sys.argv) != 4:
        sys.exit("usage: bin2c.py <input> <symbol> <output.c>")

    path, symbol, out = sys.argv[1], sys.argv[2], sys.argv[3]

    with open(path, "rb") as f:
        data = f.read()

    if not data:
        sys.exit(f"bin2c: {path} is empty")

    lines = [
        f"/* Generated from {path}. Do not edit. */",
        "",
        f"const unsigned long {symbol}_len = {len(data)}UL;",
        "",
        # **A page, not sixteen bytes.**
        #
        # The kernel maps the read-only half of the init image straight out
        # of its own copy - one set of physical pages for every process
        # rather than one each - and a mapping starts at a page boundary or
        # it starts at the wrong bytes. `process_create` refuses an image
        # that is not aligned, so getting this wrong is a boot that stops
        # and says so rather than a process that runs on somebody else's
        # memory.
        #
        # It costs up to 4095 bytes of padding per blob, against 2.8 MB a
        # process. The Lua source does not need it and gets it anyway,
        # because one rule here is easier to keep true than two.
        f"__attribute__((aligned(4096)))",
        f"const unsigned char {symbol}[] = {{",
    ]

    for i in range(0, len(data), 16):
        chunk = ", ".join(f"0x{b:02x}" for b in data[i:i + 16])
        lines.append(f"    {chunk},")

    # A terminator, so a text blob can be handed straight to something that
    # expects a C string. It is not counted in _len, which stays the real
    # size of the file: a caller that wants bytes gets bytes, and a caller
    # that wants a string gets one, without either having to know about the
    # other. Without it, strlen walks off the end of the array and the Lua
    # parser reports a syntax error on a line the file does not have.
    lines.append("    0x00")
    lines.append("};")
    lines.append("")

    #
    # And what those bytes were when this ran, so the kernel can ask later.
    #
    # The whole-blob sum answers "are these still the bytes"; the per-page
    # table answers "which page is not", and a page index times 4096 added
    # to the array's address is a physical address somebody can look for in
    # the memory map. A short last page is hashed over the bytes it has,
    # never over the padding after them.
    #
    pages = [data[i:i + PAGE] for i in range(0, len(data), PAGE)]

    lines.append("/* What the build put there. See tools/bin2c.py. */")
    lines.append(f"const unsigned long {symbol}_sum = "
                 f"0x{fnv64(data):016x}UL;")
    lines.append(f"const unsigned long {symbol}_page_bytes = {PAGE}UL;")
    lines.append(f"const unsigned long {symbol}_pages = {len(pages)}UL;")
    lines.append("")
    lines.append(f"const unsigned {symbol}_page_sum[] = {{")

    for i in range(0, len(pages), 6):
        row = ", ".join(f"0x{fnv32(page):08x}" for page in pages[i:i + 6])
        lines.append(f"    {row},")

    lines.append("};")
    lines.append("")

    with open(out, "w") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
