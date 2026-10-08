#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Every program's `kosmos: needs` line fits what the /bin store holds.

`binfs` keeps `BIN_NEEDS_MAX` words of a program's `needs` and can tell
nobody about the rest: it has no console, and the program is the one too
greedy rather than the protocol. That dropped an authority without a word
three times - the camera, the right to profile, and on 8 October the map's
tiles from the window manager, which Maps then never had. So the build
says so instead: a program in the image or installed beside it that
declares more words than the store holds fails `make test`'s host suite.

Usage: check_needs.py [ROOT]
"""

import os
import re
import sys

ROOT = sys.argv[1] if len(sys.argv) > 1 else os.path.join(os.path.dirname(__file__), "..")


def main():
    with open(os.path.join(ROOT, "user/include/binproto.h")) as f:
        most = int(re.search(r"#define BIN_NEEDS_MAX\s+(\d+)u", f.read()).group(1))

    with open(os.path.join(ROOT, "user/init/init.lua")) as f:
        lua = re.search(r"local BIN_NEEDS_MAX = (\d+)", f.read())

    fails, programs, widest = [], 0, (0, "")

    if not lua or int(lua.group(1)) != most:
        fails.append("init.lua reads %s needs words and binproto.h holds %d"
                     % (lua and lua.group(1), most))

    for top in ("user/bin", "user/installed"):
        for folder, _, files in os.walk(os.path.join(ROOT, top)):
            for name in files:
                if not name.endswith(".lua"):
                    continue
                path = os.path.join(folder, name)
                with open(path, errors="replace") as f:
                    head = f.read(4096)
                m = re.search(r"^--\s*kosmos:\s*needs\s+(.*)$", head, re.M)
                if not m:
                    continue
                programs += 1
                words = m.group(1).split()
                if len(words) > widest[0]:
                    widest = (len(words), os.path.relpath(path, ROOT))
                if len(words) > most:
                    fails.append("%s declares %d needs and /bin holds %d: %s would be dropped"
                                 % (os.path.relpath(path, ROOT), len(words), most,
                                    " ".join(words[most:])))

    if fails:
        print("FAIL: %d of the needs lines do not fit:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: %d programs' needs lines fit the %d words /bin holds (the most, %d, %s)"
          % (programs, most, widest[0], widest[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
