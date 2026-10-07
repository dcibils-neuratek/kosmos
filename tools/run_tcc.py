#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""C compiled inside Kosmos (`docs/tinycc.md`, step C3).

A disk carries the developer files (`installed.py`'s list, `/Home/Developer`)
and a project: the loader's test kit's source, `apptest.c`; a program that
builds it with the C Kit through `tccbuild.lua` into `build/apptest.elf`; a
program whose header names that image; and a file with an error in it.
Then, in the machine, with nothing from the Mac:

  - **built**: the C Kit compiles and links, and the image is written to the
    project's `build` folder - Diego's decision 4;
  - **run**: a program in that image asks its kit, and is answered 42;
  - **a problem at its line**: the broken file answers one error, on line 3,
    in TinyCC's words, and nothing is written;
  - **a pack from another build refused** - the runtime's protocol stamp
    changed on the disk - in words that say so, before anything is linked.

Usage: run_tcc.py IMAGE [DEVELOPER]   - the developer folder `make apps` made
"""

import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import installed                                            # noqa: E402
import scratch                                              # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")

MAKE = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
local r, why = use("/Kosmos/Libraries/tccbuild.lua").build{
  sources = { "/Home/t/" .. ((args or "") ~= "" and args or "apptest.c") },
  out = "/Home/t/build/apptest.elf" }
if not r then print("BUILD refused: " .. tostring(why)) return end
print(("BUILD %s %s bytes %s ms %d problems"):format(tostring(r.ok), tostring(r.bytes),
      tostring(r.milliseconds), #r.problems))
for _, p in ipairs(r.problems) do
  print(("PROBLEM %s:%s %s %s"):format(tostring(p.file), p.line, p.severity, p.text))
end
"""

RUN = """-- kosmos: image build/apptest.elf
print("ANSWER " .. tostring(use("apptest.elf").answer()))
"""

# **A build after one whose image was refused** (18.431): `/Kosmos` takes no
# file, so the first image goes the long way - `regions.write_file` reads it
# into a string to write that way too, which maps it - and the C Kit then
# gives it back. The second build, in the same process, must still link.
TWICE = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
local tccbuild = use("/Kosmos/Libraries/tccbuild.lua")
local function build(out)
  local r, why = tccbuild.build{ sources = { "/Home/t/apptest.c" }, out = out }
  if not r then print("TWICE refused " .. tostring(why)) return end
  local first = r.problems[1]
  print(("TWICE %s %s %s"):format(out, tostring(r.ok), first and first.text or "-"))
end
build("/Kosmos/apptest.elf")
build("/Home/t/build/twice.elf")
"""

BROKEN = """#include "lua.h"
int f(void) {
    return undeclared_name;
}
"""


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    arch = "x86_64" if "x86_64" in image else "aarch64"
    work = scratch.directory("tcc-" + arch)
    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    for name, text in (("make.lua", MAKE), ("run.lua", RUN), ("broken.c", BROKEN),
                       ("twice.lua", TWICE)):
        with open(os.path.join(work, name), "w") as f:
            f.write(text)

    shutil.copy(os.path.join(ROOT, "user", "kits", "apptest", "apptest.c"), work)

    def disk(pairs, name):
        path = os.path.join(work, name)

        if os.path.exists(path):
            os.remove(path)

        subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", path, "320"]
                       + pairs + ["%s:/Home/t/%s" % (os.path.join(work, n), n)
                                  for n in ("apptest.c", "make.lua", "run.lua", "broken.c",
                                            "twice.lua")]
                       # Two projects for the IDE (C5): one that builds, one that does not.
                       + ["%s:/Home/Good/%s" % (os.path.join(work, n), n)
                          for n in ("apptest.c", "run.lua")]
                       + ["%s:/Home/Bad/broken.c" % os.path.join(work, "broken.c"),
                          "%s:/Home/Bad/run.lua" % os.path.join(work, "run.lua"),
                          "%s:/Home/templates.lua" % os.path.join(HERE, "tcc_templates.lua")],
                       check=True, capture_output=True, cwd=ROOT)
        return path

    developer = installed.developer(arch, sys.argv[2] if len(sys.argv) > 2 else None)
    good = disk(developer, "disk.img")

    # The same pack, its runtime's protocol stamp changed: another build's.
    stale_runtime = os.path.join(work, "runtime-stale.o")
    runtime = [p.split(":", 1)[0] for p in developer if p.endswith("/runtime.o")][0]
    data = bytearray(open(runtime, "rb").read())
    at = data.find(b"KOSMOS-PROTOSTAMP:") + len(b"KOSMOS-PROTOSTAMP:")
    data[at:at + 16] = b"0123456789abcdef"
    open(stale_runtime, "wb").write(bytes(data))
    stale = disk([p if not p.endswith("/runtime.o")
                  else "%s:/Home/Developer/runtime.o" % stale_runtime
                  for p in developer], "stale.img")

    import run_screenshot as R                              # noqa: E402

    def ide_lines(parts):
        return "\n".join("%s: %s" % (k, l) for k in ("f6", "f5")
                         for l in parts.get(k, "").splitlines() if l.startswith("ide:"))[-1500:]

    def ide_new(path):
        """The IDE opened on New Project: Enter, F6, F5 - and the screen."""
        guest = WA.with_disk(image, path)

        try:
            guest.wait_for("kosmos> ", "a prompt")
            guest.type("wm ide:new")
            guest.wait_for("wm: window ", "the IDE's window")
            time.sleep(3)

            for key, until in (("ret", "ide: created"), ("f6", "ide: built"),
                               ("f5", "as process")):
                guest.sendkey(key)
                deadline = time.monotonic() + 90

                while time.monotonic() < deadline and until not in guest.seen:
                    guest._read_available()
                    time.sleep(0.3)

            time.sleep(10)
            guest._read_available()
            _, _, px = R.parse_ppm(guest.screendump())
            dark = sum(1 for i in range(0, len(px) - 2, 3)
                       if px[i] == 0x05 and px[i + 1] == 0x06 and px[i + 2] == 0x0a)
            return guest.seen, dark
        finally:
            guest.close()

    def ide(path, project):
        """The desktop with the IDE on `project`: F6, then F5 - what each
        caused, apart, since F5 builds too when the image is stale."""
        guest = WA.with_disk(image, path)

        try:
            guest.wait_for("kosmos> ", "a prompt")
            guest.type("wm ide:" + project)
            guest.wait_for("wm: window ", "the IDE's window")
            time.sleep(3)

            parts = {}

            for key, until in (("f6", ("ide: built", "ide: build:")),
                               ("f5", ("ended, code", "ide: build:", "could not"))):
                mark = len(guest.seen)
                guest.sendkey(key)
                deadline = time.monotonic() + 90

                while time.monotonic() < deadline:
                    guest._read_available()

                    if any(u in guest.seen[mark:] for u in until):
                        break

                    time.sleep(0.3)

                time.sleep(1)
                guest._read_available()
                parts[key] = guest.seen[mark:]

            return parts
        finally:
            guest.close()
    import run_writeapp as WA                               # noqa: E402

    def plasma(path):
        """The Plasma template, built by `templates.lua`, on the desktop: two
        screens a second apart, then Escape - what it said, and how many
        pixels differ between the two."""
        guest = WA.with_disk(image, path)

        try:
            guest.wait_for("kosmos> ", "a prompt")
            guest.type("wm /Home/P/Plasma/plasma.lua")
            guest.wait_for("wm: window ", "Plasma's window")
            time.sleep(4)
            _, _, one = R.parse_ppm(guest.screendump())
            time.sleep(1.5)
            _, _, two = R.parse_ppm(guest.screendump())
            guest.sendkey("esc")
            deadline = time.monotonic() + 30

            while time.monotonic() < deadline and "plasma: " not in guest.seen:
                guest._read_available()
                time.sleep(0.3)

            time.sleep(0.5)
            guest._read_available()
            moved = sum(1 for i in range(0, min(len(one), len(two)), 3)
                        if one[i:i + 3] != two[i:i + 3])
            return guest.seen, moved
        finally:
            guest.close()

    def session(path, steps):
        guest = WA.with_disk(image, path)

        try:
            guest.wait_for("kosmos> ", "a prompt")

            for command, until in steps:
                mark = len(guest.seen)
                guest.type(command)

                # The program's end, or the launcher's refusal - an image it
                # would not start ends nothing - within the harness's while.
                deadline = time.monotonic() + 120

                while time.monotonic() < deadline:
                    guest._read_available()
                    after = guest.seen[mark:]

                    if until in after or "\nrun: " in after:
                        break

                    time.sleep(0.2)

            time.sleep(0.5)
            guest._read_available()
            return guest.seen
        finally:
            guest.close()

    said = session(good, [("run /Home/t/make.lua", "(make) ended"),
                          ("run /Home/t/run.lua", "(run) ended"),
                          ("run /Home/t/make.lua broken.c", "(make) ended"),
                          ("run /Home/t/twice.lua", "(twice) ended")])

    built = re.search(r"^BUILD true (\d+) bytes (\d+) ms 0 problems", said, re.M)
    check(built is not None and int(built.group(1)) > 1000000,
          "apptest.c was not built into an image:\n" + said[-1500:])
    check(re.search(r"^ANSWER 42", said, re.M) is not None,
          "a program in the built image was not answered 42 by its kit:\n" + said[-1000:])
    check(re.search(r"^BUILD false nil bytes \d+ ms 1 problems", said, re.M) is not None
          and re.search(r"^PROBLEM /Home/t/broken\.c:3 error .*undeclared", said, re.M) is not None,
          "the broken file was not one error on line 3:\n"
          + "\n".join(l for l in said.splitlines() if "BUILD" in l or "PROBLEM" in l))

    check(re.search(r"^TWICE /Kosmos/apptest\.elf false /Kosmos/apptest\.elf would not be "
                    r"written", said, re.M) is not None
          and re.search(r"^TWICE /Home/t/build/twice\.elf true -", said, re.M) is not None,
          "a build after one whose image was refused did not build (18.431):\n"
          + "\n".join(l for l in said.splitlines() if "TWICE" in l or "tcc:" in l))

    # **`tcc` at the prompt** (C4): the same build, from the folder it is
    # typed in, its problems as `file:line: severity: text`.
    said = session(good, [("cd /Home/t", "kosmos>"),
                          ("tcc apptest.c -o build/apptest.elf", "(tcc) ended"),
                          ("run /Home/t/run.lua", "(run) ended"),
                          ("tcc broken.c -o build/broken.elf", "(tcc) ended")])
    check(re.search(r"^tcc: /Home/t/build/apptest\.elf, [0-9.]+ MB - built in \d+ ms", said, re.M)
          is not None and re.search(r"^ANSWER 42", said, re.M) is not None,
          "`tcc apptest.c -o build/apptest.elf` did not build an image that runs:\n"
          + said[-1200:])
    check(re.search(r"^/Home/t/broken\.c:3: error: .*undeclared", said, re.M) is not None
          and "tcc: 1 problem; nothing written" in said,
          "`tcc broken.c` did not say its problem as file:line: error:\n" + said[-800:])

    # **The IDE's Build and Run** (C5), in the desktop: F6 builds the
    # project's C into the image its Lua names; F5 runs that Lua in it; a
    # project with an error answers it at its line, and runs nothing.
    parts = ide(good, "/Home/Good")
    check(re.search(r"^ide: built build/apptest\.elf - [0-9.]+ MB in \d+ ms", parts["f6"], re.M)
          is not None,
          "F6 in the IDE did not build the project's image:\n" + ide_lines(parts))
    check(re.search(r"^ide: run\.lua, as process \d+", parts["f5"], re.M) is not None
          and re.search(r"^ide: run\.lua ended, code 0", parts["f5"], re.M) is not None
          and "ide: built" not in parts["f5"],
          "F5 in the IDE did not run the project's Lua in its image, without building "
          "again what F6 had just built:\n" + ide_lines(parts))

    parts = ide(good, "/Home/Bad")
    check(re.search(r"^ide: build: 1 problem, the first broken\.c:3 - .*undeclared",
                    parts["f6"], re.M) is not None and "as process" not in parts["f5"],
          "F6 and F5 on a project with an error did not stop at its line:\n" + ide_lines(parts))

    # **The templates** (C6): each copied out of /Kosmos/Templates, the three
    # with C built, and the two that print run - each saying what it should.
    said = session(good, [("run /Home/templates.lua", "(templates) ended"),
                          ("run /Home/P/SumBothWays/sum.lua", "(sum) ended"),
                          ("run /Home/P/Primes/primes.lua 1000000", "(primes) ended")])
    check(re.search(r"^TEMPLATES HelloWindow Mandelbrot Plasma Primes SumBothWays", said, re.M)
          is not None, "/Kosmos/Templates does not hold the five templates:\n" + said[-600:])
    for name in ("Mandelbrot", "SumBothWays", "Primes", "Plasma"):
        check(re.search(r"^TEMPLATE %s built true" % name, said, re.M) is not None,
              "the %s template did not build:\n%s" % (name, "\n".join(
                  l for l in said.splitlines() if l.startswith("TEMPLATE"))))
    check("the same answer" in said and re.search(r"^  C: +29999997 in \d+ ms", said, re.M),
          "Sum, both ways did not give the same answer in Lua and C:\n" + said[-600:])
    check(re.search(r"^78498 primes below 1000000", said, re.M) is not None,
          "Primes did not count the 78,498 primes below a million:\n" + said[-400:])

    # **A window from C** (`docs/windowkit.md`, W1): Plasma, built above,
    # opens its window through the Window Kit, draws a frame after frame -
    # the screen a second later is not the same - and Escape closes it, in
    # its own words.
    seen, moved = plasma(good)
    check(re.search(r"^plasma: \d+ frames", seen, re.M) is not None,
          "Plasma did not close on Escape, saying its frames:\n"
          + "\n".join(l for l in seen.splitlines() if "plasma" in l or "wm:" in l)[-1000:])
    check(moved > 50000,
          "Plasma's window is not animating: %d pixels changed in a second and a half" % moved)

    # **New Project, in the IDE** (C6): opened on it, Enter creates the
    # default - a Lua and C app from Mandelbrot - F6 builds it and F5 runs it,
    # and the window it opens is the fractal: its black interior on the screen.
    seen, picture = ide_new(good)
    check("ide: created /Home/Projects/Mandelbrot from the Mandelbrot template" in seen
          and "ide: built build/mandelbrot.elf" in seen
          and "ide: mandelbrot.lua, as process" in seen,
          "New Project, F6 and F5 did not make, build and run Mandelbrot:\n"
          + "\n".join(l for l in seen.splitlines() if l.startswith("ide:"))[-1200:])
    check(picture > 20000,
          "Mandelbrot's window does not show the fractal: %d of its dark pixels" % picture)

    said = session(stale, [("run /Home/t/make.lua", "(make) ended")])
    check("BUILD refused: the developer files in /Home/Developer are from another Kosmos" in said,
          "a pack from another build was not refused in words:\n"
          + "\n".join(l for l in said.splitlines() if "BUILD" in l))

    if fails:
        print("FAIL: %d of %d checks on C built inside Kosmos:\n  %s"
              % (len(fails), checks, "\n  ".join(fails)))
        return 1

    print("PASS: %d checks on C built inside Kosmos (apptest.c compiled and linked by "
          "the C Kit into build/apptest.elf, %s bytes in %s ms; a program in it answered "
          "42; a build after a refused image; a broken file one error on line 3; the same at the prompt with tcc; the "
          "IDE's F6 and F5; the five templates, built and run; Plasma's window from C, animating and closed; New Project making, "
          "building and running Mandelbrot; a pack from another build refused)"
          % (checks, built.group(1), built.group(2)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
