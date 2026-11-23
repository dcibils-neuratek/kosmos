#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`tools/suitedeps.py` held to this tree.

    python3 tools/test_suitedeps.py

Each check is a real application, library, kit, server or driver and a file
it must - or must not - reach. The control at the end turns the transitive
follow off and shows the checks that depend on it failing, so a pass here
is not the closure doing nothing.
"""

import os
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import suitedeps as sd                              # noqa: E402


def reaches(subjects, path):
    return path in sd.suite_files(subjects)


def refused(subject):
    try:
        sd.suite_files([subject])
    except sd.SubjectError:
        return True
    return False


# The checks that need the follow: each is a file reached only through
# another file, never named by the subject itself.
TRANSITIVE = [
    ("app:paint reaches ui.lua through paint.lua",
     lambda: reaches(["app:paint"], "user/lib/ui.lua")),
    ("app:paint reaches regions.lua two steps down (paint.lua -> ui.lua -> regions.lua)",
     lambda: reaches(["app:paint"], "user/lib/regions.lua")
     and len(sd.why("user/lib/regions.lua", ["app:paint"])) >= 4),
    ("server:audio reaches user/include/audioproto.h",
     lambda: reaches(["server:audio"], "user/include/audioproto.h")),
    ("server:audio reaches user/include/pcmconvert.h",
     lambda: reaches(["server:audio"], "user/include/pcmconvert.h")),
    ("a library's subfolder resolves: app:groove reaches user/lib/groove/app.lua",
     lambda: reaches(["app:groove"], "user/lib/groove/app.lua")),
    ("a kit's folder by its build function: app:groove reaches the Synth Kit's C",
     lambda: reaches(["app:groove"], "user/kits/synth/synth_kosmos.c")),
    ("require with a computed end: app:solar reaches every font, f24 among them",
     lambda: reaches(["app:solar"], "user/lib/solar/fonts/f24.lua")),
    ("luacheck through lint.lua's require: /Kosmos/Libraries/luacheck is runtime/upstream",
     lambda: reaches(["lib:lint.lua"], "runtime/upstream/luacheck/src/luacheck/init.lua")),
    ("a header reaches the source beside it: driver:usb/xhci reaches usb_decode.c",
     lambda: reaches(["driver:usb/xhci"], "user/drivers/usb/usb_decode.c")),
    ("an installed application's C reaches its engine's headers: app:doom, doomgeneric.h",
     lambda: reaches(["app:doom"], "runtime/upstream/doom/doomgeneric.h")),
    ("a template's image is its own C: files: is not followed, the Lua is",
     lambda: not reaches(["files:user/templates/Primes/primes.lua"], "user/templates/Primes/primes.c")
     and "user/templates/Primes/primes.c" in sd.lua_closure("user/templates/Primes/primes.lua")),
]


def check_kit_names():
    """`sys.kit`'s names whose folder is not the name: from where the build
    function is defined, not from a list written here."""
    t = sd.kit_table()
    return (t.get("network") == "user/kits/network"
            and t.get("fractal") == "user/bin/apps/fractal"
            and t.get("doom") == "user/installed/Doom")


def check_c_fallback():
    """With no build's `.d` files, `#include` lines still reach the protocol."""
    saved = sd.VARIANTS
    sd.VARIANTS = []
    sd._cache.clear()
    try:
        files = sd.suite_files(["server:audio"])
        return "user/include/audioproto.h" in files and "user/include/pcmconvert.h" in files
    finally:
        sd.VARIANTS = saved
        sd._cache.clear()


def check_kernel_not_paired():
    """`kernel/syscall.h` is the ABI's numbers: a server includes it and
    does not stand on the kernel's `syscall.c`."""
    files = sd.suite_files(["server:audio"])
    return "kernel/syscall.h" in files and "kernel/syscall.c" not in files


def check_comments_not_followed():
    """findbar.lua's opening comment shows `use(".../ui.lua")` and
    `use(".../pixelkit.lua")` as usage; its code uses neither. lint.lua has
    `use(\\"/Kosmos/Libraries/clock.lua\\")` inside a string of advice."""
    findbar, _ = sd.lua_uses("user/lib/findbar.lua")
    lint, _ = sd.lua_uses("user/lib/lint.lua")
    text = open(os.path.join(sd.ROOT, "user/lib/findbar.lua")).read()
    return ('use("/Kosmos/Libraries/pixelkit.lua")' in text and findbar == []
            and "user/lib/clock.lua" not in lint)


def check_why():
    lines = sd.why("user/include/pcmconvert.h", ["server:audio"])
    return (lines is not None and lines[0] == "server:audio"
            and lines[-1].endswith("user/include/pcmconvert.h"))


def check_every_app_fast():
    """Every application's closure, from a cold cache, in under a second."""
    sd._cache.clear()
    names = sorted({n[:-4] if n.endswith(".lua") else n
                    for n in os.listdir(os.path.join(sd.ROOT, "user/bin/apps"))})
    start = time.perf_counter()
    for n in names:
        sd.suite_files(["app:" + n])
    took = time.perf_counter() - start
    print("# %d applications in %.3f s" % (len(names), took))
    return took < 1.0


def check_no_unresolved_literals():
    """Every literal path an application uses resolves: what is left is
    computed in code (`use(HERE .. name)`), never a string that names
    nothing."""
    bad = []
    for n in os.listdir(os.path.join(sd.ROOT, "user/bin/apps")):
        sd.suite_files(["app:" + (n[:-4] if n.endswith(".lua") else n)], unresolved=bad)
    left = [b for b in bad if b[3] != "dynamic"]
    for b in left:
        print("# unresolved: %s:%d: %s (%s)" % b)
    return not left


CHECKS = TRANSITIVE + [
    ("kit names map to the folder their build function is in", check_kit_names),
    ("a subject that does not exist is refused: app:nosuchapp",
     lambda: refused("app:nosuchapp")),
    ("refused: kit:nosuchkit, server:nosuch, driver:usb/nosuch, lib:nosuch.lua",
     lambda: all(refused(s) for s in ("kit:nosuchkit", "server:nosuch",
                                      "driver:usb/nosuch", "lib:nosuch.lua"))),
    ("refused: a subject of no kind", lambda: refused("widget:paint") and refused("paint")),
    ("use( in comments and in strings of prose is not followed", check_comments_not_followed),
    ("without a .d, #include lines are scanned", check_c_fallback),
    ("kernel headers are not paired with the kernel's sources", check_kernel_not_paired),
    ("--why gives the chain", check_why),
    ("no literal use() in any application is left unresolved", check_no_unresolved_literals),
    ("every application's closure in under a second", check_every_app_fast),
]


def run(checks):
    failed = 0
    for name, fn in checks:
        try:
            ok = bool(fn())
        except Exception as e:                      # a check that raises fails
            print("# %s: %s" % (type(e).__name__, e))
            ok = False
        print("%s - %s" % ("ok" if ok else "not ok", name))
        failed += not ok
    return failed


def control():
    """The follow off: only what a subject names. The transitive checks
    must fail now - at least one, and the ones that cannot pass without it."""
    print("# control: suitedeps.FOLLOW = False")
    sd._cache.clear()
    sd.FOLLOW = False
    try:
        failed = 0
        for name, fn in TRANSITIVE:
            try:
                ok = bool(fn())
            except Exception:
                ok = False
            print("#   %s - %s" % ("passes" if ok else "fails", name))
            failed += not ok
    finally:
        sd.FOLLOW = True
        sd._cache.clear()
    ok = failed > 0
    print("%s - control: %d of %d transitive checks fail without the follow"
          % ("ok" if ok else "not ok", failed, len(TRANSITIVE)))
    return 0 if ok else 1


def main():
    failed = run(CHECKS)
    failed += control()
    print("%d failed" % failed if failed else "all passed")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
