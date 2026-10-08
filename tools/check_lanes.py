#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The lanes are instructions, not calls (`roadmap.md` 6zz h).

A vector loop here loads and stores through a `memcpy` of a vector's size,
which a compiler makes one instruction - unless it may not take `memcpy` to
be the C library's, which `-ffreestanding` tells GCC. Then every sixteen
bytes is a call into the libc's loop, and on ARM the fills that were to go
four times faster went three times slower. A test on the Mac could not
show it: clang inlines the copy there. So this reads what was built, for
both machines, and asks of the code that is meant to be lanes that it
calls no `memcpy` - `__builtin_memcpy` is the way to ask for the
instruction.

An object not built is said and skipped: an image without the browser has
no rasteriser, and a Mac without the x86 compiler has no x86 objects.
"""

import os
import re
import subprocess
import sys

# The object, and the function in it that is lanes - None for all of it.
LANES = [
    ("user/kits/gfx/rows.c.o", None),
    ("user/kits/gfx/pack.c.o", None),
    ("user/kits/gfx/raster.c.o", None),
    ("user/kits/gfx/gfx.c.o", "l_blend"),
]

MACHINES = [
    ("aarch64-none-elf-objdump", "build/user-web-ffmpeg"),
    ("x86_64-elf-objdump", "build/user-x86_64-web-ffmpeg"),
]

CALL = re.compile(r"\bR_\w+\s+memcpy\b")
FUNCTION = re.compile(r"^[0-9a-f]+ <([^>]+)>:$")


def calls_in(objdump, path, only):
    """The functions of `path` that call memcpy, as (function, count)."""
    out = subprocess.run([objdump, "-dr", path], check=True,
                         capture_output=True, text=True).stdout
    found, where = {}, None

    for line in out.splitlines():
        m = FUNCTION.match(line)

        if m:
            where = m.group(1)
        elif CALL.search(line) and (only is None or where == only):
            found[where] = found.get(where, 0) + 1

    return sorted(found.items())


def main():
    looked, wrong = 0, []

    for objdump, root in MACHINES:
        if subprocess.run(["which", objdump], capture_output=True).returncode != 0:
            print(f"  {objdump}: not installed, skipped")
            continue

        for obj, only in LANES:
            path = os.path.join(root, obj)

            if not os.path.exists(path):
                print(f"  {path}: not built, skipped")
                continue

            looked += 1

            for function, n in calls_in(objdump, path, only):
                wrong.append(f"{path}: {function} calls memcpy {n} time(s)")

    if wrong:
        print("FAIL: code meant to be lanes calls memcpy - ask for "
              "__builtin_memcpy, which is the instruction:")
        for w in wrong:
            print("  " + w)
        return 1

    if looked == 0:
        print("FAIL: none of the objects with lanes in them was built")
        return 1

    print(f"PASS: {looked} objects' lanes call no memcpy, on every machine "
          "built for.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
