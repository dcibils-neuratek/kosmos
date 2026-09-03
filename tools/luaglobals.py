#!/usr/bin/env python3
"""
Every global the Lua in this repository reads, checked against what will
actually be there.

`luacheck` catches syntax. This catches the mistake that has cost four
debugging sessions and is not a syntax error at all: a name that used to be
a local and is not any more. Lua compiles a read of an undefined name
perfectly happily - it is a global, and globals may be nil - so the failure
arrives later, at run time, as `attempt to call a nil value (global 'reap')`,
in a process that cannot tell you because it prints by asking the console
server and a dead process asks nothing.

The most recent one was exactly that: an edit removed the block a local was
defined in and left three calls to it, and the symptom was a shell that
vanished the moment anybody typed a program name.

How it works: `luac -l -p` lists the bytecode, and every read of a global
appears as `_ENV "name"`. Anything not in the environment that file will run
in is reported. It is a small, exact check - it says nothing about whether a
*field* exists, only whether the name at the root of the expression will.
"""

import re
import subprocess
import sys

GLOBAL = re.compile(r'_ENV "([A-Za-z_][A-Za-z0-9_]*)"')

# What upstream Lua puts in a fresh state, minus what this system removes.
# `dofile` and `loadfile` are deliberately absent: `kosmos_lua_open` sets
# them to nil, because there is no path to open.
LUA = {
    "_G", "_VERSION", "assert", "collectgarbage", "coroutine", "error",
    "getmetatable", "ipairs", "load", "math", "next", "pairs", "pcall",
    "print", "rawequal", "rawget", "rawlen", "rawset", "select",
    "setmetatable", "string", "table", "tonumber", "tostring", "type",
    "utf8", "xpcall",
}

# Absent on purpose, and read on purpose: `luatest.lua` asserts they are nil,
# which is a read. Naming them here rather than widening LUA keeps the
# distinction between "this exists" and "this is checked for absence".
ABSENT = {"io", "os", "debug", "package", "dofile", "loadfile", "require"}

# What each kind of file runs with, on top of the above.
ENVIRONMENTS = {
    # The image's own chunks: a fresh state with `sys` and `gfx` opened.
    "default": {"sys", "gfx"},

    # A program in /bin gets an environment built by the runner.
    #
    # `doom` is there only in an image built with `make DOOM=1`, and is
    # listed here anyway - which is the honest way round. This checker asks
    # "will this name exist", and the answer for `doom` is "in the image
    # that has Doom in it". The application checks for itself before using
    # it, because a program that assumes an optional global is a program
    # that fails with a nil index instead of a sentence.
    "user/bin/": {"sys", "gfx", "fs", "args", "cwd", "run",
                  "interrupted", "use", "doom"},

    # A library is loaded into the environment of whoever asked for it, so it
    # sees the same names a program does - minus `args`, which belongs to the
    # program and not to what it loaded.
    "user/lib/": {"sys", "gfx", "fs", "cwd", "run", "interrupted", "use"},
}


# Some files do not run in a Lua state at all. A replicant is loaded by its
# host into an environment built by hand from a `needs` list, so it has no
# `print`, no `assert`, no `sys` - and checking it against the ordinary base
# would let a replicant using `print` through, to fail in somebody else's
# window at run time.
#
# So this is an *exact* set and not an addition to one. It has to match
# `ui.replicant` in user/lib/ui.lua; when that list changes, this one does.
EXACT = {
    "-replicant.lua": {
        "gfx", "fs", "ticks", "theme",
        "math", "string", "table",
        "tostring", "tonumber", "ipairs", "pairs", "select",
        "type", "error", "pcall",
    },
}


def environment_for(path):
    for suffix, exact in EXACT.items():
        if path.endswith(suffix):
            return set(exact)

    allowed = set(LUA) | set(ABSENT) | ENVIRONMENTS["default"]

    for prefix, extra in ENVIRONMENTS.items():
        if prefix != "default" and prefix in path:
            allowed |= extra

    return allowed


def check(luac, path):
    try:
        listing = subprocess.run([luac, "-l", "-p", path],
                                 capture_output=True, text=True, check=True)
    except subprocess.CalledProcessError as e:
        print(f"{path}: {e.stderr.strip()}", file=sys.stderr)
        return 1

    allowed = environment_for(path)
    unknown = sorted({m for m in GLOBAL.findall(listing.stdout)} - allowed)

    if not unknown:
        return 0

    print(f"{path}: reads {len(unknown)} name(s) that will not be there:",
          file=sys.stderr)

    for name in unknown:
        print(f"    {name}", file=sys.stderr)

    print("  Either it should be a local and the definition was lost, or the "
          "environment\n  this file runs in needs to say it provides it "
          "(tools/luaglobals.py).", file=sys.stderr)

    return 1


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: luaglobals.py <luac> <file.lua>...")

    luac, files = sys.argv[1], sys.argv[2:]
    failures = sum(check(luac, path) for path in files)

    if failures:
        raise SystemExit(1)

    print(f"luaglobals: {len(files)} file(s), every global accounted for")


if __name__ == "__main__":
    main()
