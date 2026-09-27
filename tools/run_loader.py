#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Programs from a file (`docs/elf.md` step 4).

Booted with a /Home of its own holding `/Home/apps/apptest/`:

  apptest.lua    `-- kosmos: image apptest.elf`, and prints what the
                 apptest kit answers - 42, which only its own image can
                 say: the kit is in no other (`make apps` checks)
  apptest.elf    that image, as the build linked it, stripped
  plain.lua      the same program with no image line: run in the system's
                 image, where there is no kit called apptest
  broken.lua     names broken.elf, the image's first 200 KB: refused with
                 the reader's sentence, and nothing started
  stranger.lua   names stranger.elf, the image's first page with the other
                 processor's number in it: refused as built for that one

and `run` at the prompt for each, then apptest.lua again - from the image
made the first time, which the launcher says only when it makes one. How
long each took is said.

Usage: run_loader.py IMAGE
"""

import glob
import os
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
X86 = "x86_64" in IMAGE
WORK = scratch.directory("loader")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")


def this_boards_image():
    """The newest apptest.elf `make apps` linked for this board."""
    found = [p for p in glob.glob(os.path.join(ROOT, "build", "user*", "apps", "apptest.elf"))
             if ("x86_64" in p) == X86]
    return max(found, key=os.path.getmtime) if found else None


PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- The loader's test program (`tools/run_loader.py`).
{line}
local kit = use("/Kosmos/Kits/apptest")
print("apptest: " .. tostring(kit.answer()))
"""

app = this_boards_image()

if app is None:
    print("FAIL: no apptest.elf for this board - `make apps` (and ARCH=x86_64) builds it")
    sys.exit(1)

stripped = os.path.join(WORK, "apptest.elf")
subprocess.run([("x86_64-elf-" if X86 else "aarch64-none-elf-") + "objcopy",
                "--strip-debug", app, stripped], check=True)

with open(stripped, "rb") as f:
    whole = f.read()

with open(os.path.join(WORK, "broken.elf"), "wb") as f:
    f.write(whole[:200 * 1024])

stranger = bytearray(whole[:4096])
stranger[18:20] = (183 if X86 else 62).to_bytes(2, "little")

with open(os.path.join(WORK, "stranger.elf"), "wb") as f:
    f.write(bytes(stranger))

for name, line in (("apptest", "-- kosmos: image apptest.elf"), ("plain", ""),
                   ("broken", "-- kosmos: image broken.elf"),
                   ("stranger", "-- kosmos: image stranger.elf")):
    with open(os.path.join(WORK, name + ".lua"), "w") as f:
        f.write(PROGRAM.format(line=line))

files = ["apptest.lua", "plain.lua", "broken.lua", "stranger.lua", "apptest.elf",
         "broken.elf", "stranger.elf"]
subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK, "64"]
               + ["%s:/Home/apps/apptest/%s" % (os.path.join(WORK, n), n) for n in files],
               check=True, capture_output=True, cwd=ROOT)
os.environ["KOSMOS_DISK"] = HOME_DISK

import run_screenshot as R                                   # noqa: E402


def main():
    guest = R.Guest(IMAGE, 120)
    failed = []
    checks = 0
    times = []

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def run(name, want, seconds):
        """`run` the program; the first line after it holding `want`.

        **And then the prompt, before the next one is typed.** A line a
        program says can arrive after the shell has printed its prompt, and
        this typed the next `run` at once - so on 27 September `plain`'s
        check read its own echoed command, which names `apptest`, and the
        `broken` check read `plain`'s late error. The echo of what was typed
        is never an answer either.
        """
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/apptest/%s.lua" % name)
        deadline = started + seconds
        found = None

        while time.monotonic() < deadline and found is None:
            for line in guest.seen[mark:].split("\n")[1:]:
                if want in line and not line.lstrip().startswith("kosmos>"):
                    found = line.strip()
                    break
            if found is None:
                time.sleep(0.1)
                guest._read_available()

        took = time.monotonic() - started

        if found is not None:
            at = guest.seen.find(found, mark) + len(found)

            while time.monotonic() < deadline and "kosmos> " not in guest.seen[at:]:
                time.sleep(0.1)
                guest._read_available()

        return found, took

    try:
        guest.wait_for("kosmos> ", "reached a prompt")

        first = len(guest.seen)
        said, took = run("apptest", "apptest:", 180)
        times.append(took)
        check(said == "apptest: 42",
              "apptest.lua, in its own image, did not say 42: %r" % said)
        check("image: made /Home/apps/apptest/apptest.elf, " in guest.seen[first:],
              "the first start did not make the image from the file")

        said, _ = run("plain", "apptest", 60)
        check(said is not None and "there is no kit called apptest" in said,
              "plain.lua, in the system's image, found the apptest kit: %r" % said)

        said, _ = run("broken", "run:", 60)
        check(said is not None and "broken.elf: a segment runs past the end of the file" in said,
              "broken.elf was not refused as the reader refuses it: %r" % said)

        said, _ = run("stranger", "run:", 60)
        other = "an AArch64 processor" if X86 else "an x86-64 processor"
        check(said is not None and ("stranger.elf: built for %s, not this one" % other) in said,
              "stranger.elf was not refused as built for another processor: %r" % said)

        again = len(guest.seen)
        said, took = run("apptest", "apptest:", 180)
        times.append(took)
        check(said == "apptest: 42", "apptest.lua did not say 42 the second time: %r" % said)
        check("image: made" not in guest.seen[again:],
              "the second start made the image again rather than using the one kept")
    finally:
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on programs from a file:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on programs from a file (a program run in the %.1f MB image "
          "beside it, where its kit is and the system's has none; a truncated image and "
          "one for another processor refused with the reader's sentences; started again "
          "from the image made the first time - %.1f s, then %.1f s)"
          % (checks, len(whole) / 1e6, times[0], times[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
