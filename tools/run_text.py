#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""W5b's text on Kosmos (`docs/w5.md`).

`texttest.elf` - HarfBuzz 15.1.0 and FreeType 2.15.0 with
`user/kits/texttest/texttest.c` - given HarfBuzz's own shaping suite and
every font it ships, from a /Home of its own; what it says, a line a test
file and a line a font, held to what the same file says on this Mac,
compiled natively against the same releases (`tools/w5_build.py host`),
and the shaping held case by case to the glyphs the suite expects.

The suite is flattened here from the `.tests` files HarfBuzz's Meson build
names, as `test/shape/run-tests.py` reads them: its directives, its
font-functions and shapers, its skips, and the `--verify
--unsafe-to-concat --safe-to-insert-tatweel` it adds to every case with an
expectation.

Usage: run_text.py IMAGE [APPS]

The controls: `KOSMOS_TEXT_BREAK=fonts` makes the reference from fonts with
one byte changed in each, and the guest must disagree; `=expected` changes
the glyphs one case expects, on both sides, and HarfBuzz's expectations
must not be met.
"""

import fcntl
import os
import re
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402
import upstream                                              # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
X86 = "x86_64" in IMAGE
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(
    ROOT, "build", "user-x86_64" if X86 else "user", "apps")
WORK = scratch.directory("text")
FOLDER = os.path.join(WORK, "text")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")
HOST = os.path.join(ROOT, "build", "w5", "host", "install")
TEST = os.path.join(ROOT, "user", "kits", "texttest")
SETS = ["in-house", "aots", "text-rendering-tests"]

# What this build of HarfBuzz has - `texttest` says the same on its first
# line, and a disagreement is a failure.
FONT_FUNCS = ["ot", "ft"]
SHAPERS = ["ot", "fallback"]
FACE_LOADERS = ["ot", "ft"]

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- W5b's text test (`tools/run_text.py`).
-- kosmos: image texttest.elf
local kit = use("texttest.elf")
for _, line in ipairs(kit.run("/Home/apps/text/text")) do
  print(line)
end
"""


def meson_list(path, name):
    """The file names in the `<set>_tests = [...]` list, which is what the
    Meson build runs - not the `disabled_..._tests` list beside it."""
    text = open(path, encoding="utf8").read()
    block = re.search(r"^%s\s*=\s*\[(.*?)\]" % re.escape(name), text, re.S | re.M)
    return [m.group(1) for line in block.group(1).splitlines()
            if not line.strip().startswith("#")
            for m in [re.search(r"'([^']+\.tests)'", line)] if m]


def flatten(data):
    """Every case `run-tests.py` would run, as `NAME \\t FONT \\t OPTIONS
    \\t EXPECTED`, FONT relative to `data`; and how many it would skip."""
    cases, skips = [], 0
    supported = {"shaper": SHAPERS, "face-loader": FACE_LOADERS, "font-funcs": FONT_FUNCS}

    for s in SETS:
        for test in meson_list(os.path.join(data, s, "meson.build"),
                               s.replace("-", "_") + "_tests"):
            filename = os.path.join(data, s, "tests", test)
            name = s + "/" + test
            every = {"shaper": ["ot"], "face-loader": ["ot"], "font-funcs": list(FONT_FUNCS)}

            for line in open(filename, encoding="utf8"):
                comment = False
                if line.startswith("#"):
                    comment = True
                    line = line[1:]
                    if line.startswith(" "):
                        continue
                line = line.strip()
                if not line:
                    continue
                if line.startswith("@"):
                    line = line.split("#")[0].strip()[1:]
                    for what in ("shaper", "face-loader", "font-funcs"):
                        whats = what if what.endswith("s") else what + "s"
                        if line.startswith(what) or line.startswith(whats):
                            command, values = line.split("=")
                            values = values.strip().split(",")
                            if command[-1] == "-":
                                every[what] = [v for v in supported[what] if v not in values]
                            else:
                                every[what] = [v for v in values if v in supported[what]]
                            break
                    else:
                        raise SystemExit("%s: unrecognised directive %s" % (filename, line))
                    continue

                fontfile, options, unicodes, expected = line.split(";")
                options = options.split()
                if fontfile.startswith("/") or fontfile.startswith('"/'):
                    skips += 1                      # a font of this Mac's own
                    continue
                font = os.path.relpath(os.path.normpath(
                    os.path.join(os.path.dirname(filename), fontfile)), data)
                if comment:
                    continue

                chosen, kept, skip = {}, [], False
                it = iter(options)
                for option in it:
                    for what in ("shaper", "face-loader", "font-funcs"):
                        if option.startswith("--" + what):
                            backend = option.split("=")[1] if "=" in option else next(it)
                            if backend not in supported[what]:
                                skip = True
                            chosen[what] = backend
                            break
                    else:
                        kept.append(option)
                    if skip:
                        break
                if skip:
                    skips += 1
                    continue

                for loader in [chosen["face-loader"]] if "face-loader" in chosen \
                        else every["face-loader"]:
                    if loader != "ot":
                        raise SystemExit("%s: face loader %s is not run here" % (filename, loader))
                    for shaper in [chosen["shaper"]] if "shaper" in chosen else every["shaper"]:
                        funcs_for_shaper = every["font-funcs"] if shaper == "ot" else [None]
                        for funcs in [chosen["font-funcs"]] if "font-funcs" in chosen \
                                else funcs_for_shaper:
                            extra = ["--shaper=" + shaper]
                            if funcs:
                                extra.append("--font-funcs=" + funcs)
                            if expected != "*":
                                extra += ["--verify", "--unsafe-to-concat",
                                          "--safe-to-insert-tatweel"]
                            args = ["--unicodes", unicodes] + kept + extra
                            cases.append("\t".join([name, font, " ".join(args), expected]))
    return cases, skips


def gather(broken):
    """The suite's fonts under the folder, `cases.txt` and `fonts.txt`."""
    upstream.unpack("harfbuzz")
    data = os.path.join(upstream.where("harfbuzz"), "test", "shape", "data")
    shutil.rmtree(FOLDER, ignore_errors=True)

    fonts = []
    for s in SETS:
        where = os.path.join(data, s, "fonts")
        for n in sorted(os.listdir(where)):
            if n.lower().endswith((".ttf", ".otf", ".ttc", ".dfont")):
                fonts.append(s + "/fonts/" + n)
    for f in fonts:
        into = os.path.join(FOLDER, f)
        os.makedirs(os.path.dirname(into), exist_ok=True)
        shutil.copyfile(os.path.join(data, f), into)
        if broken == "fonts":
            # The control: one byte in the middle of each font turned
            # over, for the reference only.
            b = bytearray(open(into, "rb").read())
            b[len(b) // 2] ^= 0xff
            open(into, "wb").write(bytes(b))

    cases, skips = flatten(data)
    if broken == "expected":
        # The other control: one case expects a glyph it will not get.
        at = next(i for i, c in enumerate(cases) if not c.endswith("\t*"))
        name, font, args, expected = cases[at].split("\t")
        cases[at] = "\t".join([name, font, args, expected.replace("[", "[gid9999|", 1)])
    with open(os.path.join(FOLDER, "cases.txt"), "w", encoding="utf8") as f:
        f.write("".join(c + "\n" for c in cases))
    with open(os.path.join(FOLDER, "fonts.txt"), "w") as f:
        f.write("".join(n + "\n" for n in fonts))
    return fonts, cases, skips


def reference():
    """What this Mac says: the same file, natively, against the same
    releases."""
    os.makedirs(os.path.join(ROOT, "build", "w5"), exist_ok=True)
    with open(os.path.join(ROOT, "build", "w5", ".host-lock"), "w") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        subprocess.run([sys.executable, os.path.join(HERE, "w5_build.py"), "host"],
                       check=True, capture_output=True)
    binary = os.path.join(WORK, "texttest-mac")
    lib = os.path.join(HOST, "lib")
    subprocess.run(["/usr/bin/clang", "-std=c11", "-O2", "-Wall", "-Wextra", "-Werror",
                    "-DTEXTTEST_HOST", "-I" + os.path.join(HOST, "include", "harfbuzz"),
                    "-I" + os.path.join(HOST, "include", "freetype2"),
                    os.path.join(TEST, "texttest.c"),
                    os.path.join(lib, "libharfbuzz.a"), os.path.join(lib, "libfreetype.a"),
                    os.path.join(lib, "libpng.a"), os.path.join(lib, "libz.a"),
                    "-lc++", "-o", binary], check=True)
    return subprocess.run([binary, FOLDER], capture_output=True, text=True,
                          check=True).stdout.splitlines()


def main():
    built = os.path.join(APPS, "texttest.elf")

    if not os.path.exists(built) and not os.environ.get("KOSMOS_TEXT_MAC_ONLY"):
        print("FAIL: no %s - `make apps` (and ARCH=x86_64) builds it" % built)
        return 1

    broken = os.environ.get("KOSMOS_TEXT_BREAK")
    if broken not in (None, "fonts", "expected"):
        print("FAIL: KOSMOS_TEXT_BREAK is fonts or expected, not %s" % broken)
        return 1
    fonts, cases, skips = gather(broken)
    want = reference()
    if broken == "fonts":
        gather(None)                    # the guest gets the true fonts

    if os.environ.get("KOSMOS_TEXT_MAC_ONLY"):
        print("\n".join(want))
        return 0

    image = os.path.join(WORK, "texttest.elf")
    subprocess.run([("x86_64-elf-" if X86 else "aarch64-elf-") + "objcopy",
                    "--strip-debug", built, image], check=True)
    with open(os.path.join(WORK, "texttest.lua"), "w") as f:
        f.write(PROGRAM)

    files = [(os.path.join(WORK, n), "/Home/apps/text/" + n)
             for n in ("texttest.lua", "texttest.elf")]
    files += [(os.path.join(FOLDER, n), "/Home/apps/text/text/" + n)
              for n in fonts + ["cases.txt", "fonts.txt"]]
    megabytes = sum(os.path.getsize(src) for src, _ in files) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)] + ["%s:%s" % f for f in files],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 300)
    said = []
    done = False

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/text/texttest.lua")

        while time.monotonic() < started + 300 and not done:
            time.sleep(0.2)
            guest._read_available()
            lines = guest.seen[mark:].replace("\r", "").split("\n")[1:]
            said = [ln for ln in lines if ln.split(":")[0] in
                    ("versions", "shape", "shape-not-expected", "freetype", "texttest")]
            done = any(ln.startswith("texttest: ") for ln in said)
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_TEXT_SERIAL"):
        with open(os.environ["KOSMOS_TEXT_SERIAL"], "w") as f:
            f.write(guest.seen)

    differ = [(w, s) for w, s in zip(want, said) if w != s]
    if len(want) != len(said) or differ:
        print("FAIL: the text on Kosmos is not what it is on this Mac: "
              "%d lines here, %d there, %d differ" % (len(want), len(said), len(differ)))
        for w, s in differ[:12]:
            print("  Mac:    %s\n  Kosmos: %s" % (w, s))
        if not done:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    funcs = "; font funcs %s; %s" % (" ".join(FONT_FUNCS), " ".join(SHAPERS))
    if not want[0].endswith(funcs):
        print("FAIL: this HarfBuzz is not the one the cases were chosen for: %s" % want[0])
        return 1
    total = re.match(r"texttest: (\d+) cases, (\d+) as expected, (\d+) not; (\d+) fonts",
                     want[-1])
    if not total or int(total.group(1)) != len(cases) or total.group(3) != "0":
        print("FAIL: HarfBuzz's own expectations are not met: %s" % want[-1])
        for ln in want:
            if ln.startswith("shape-not-expected"):
                print("  " + ln)
        return 1

    print("PASS: %d shaping cases from %d test files (%d skipped: they name macOS's own fonts), "
          "every one as HarfBuzz expects, and %d fonts rendered by FreeType - "
          "every line as on this Mac, in %.1f s"
          % (len(cases), sum(1 for ln in want if ln.startswith("shape: ")), skips,
             len(fonts), took))
    return 0


if __name__ == "__main__":
    sys.exit(main())
