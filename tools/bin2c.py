#!/usr/bin/env python3
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
"""

import sys


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

    with open(out, "w") as f:
        f.write("\n".join(lines))


if __name__ == "__main__":
    main()
