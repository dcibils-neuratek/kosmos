#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Doom and Quake start, and come back to Lua, on a machine with no game.

On 29 September Quake died on the M700 every time it was started, with a
general protection fault at the `ret` of `l_start` and the text "e EFI sy"
where its return address should have been: `quake_kosmos.c.o` was compiled
before `struct sysinfo` grew to 2744 bytes, and `l_start` kept the old size
on its stack while the kernel wrote the new one over it (`testing.md`
18.281). **Nothing in the gate had ever started either engine**: their games
are id's, a person's own, and never in this repository - `arm-launch` starts
Doom without a WAD, and `doom.lua` stops before the engine when there is
none.

And writing this found the second fault under the first: `exit` from an
engine landed in `kosmos_exit_arm`'s frame after it had returned, so an
engine that gave up during its start came back as one that had *started*,
and Quake drew frames of a half-made game until it said "load failed."

Neither needs a game to be watched starting. A pak holding no files - "PACK",
where its directory is, and that it is empty - and a WAD holding no lumps -
"IWAD", none, and where their table is - twelve bytes each, written here,
take each engine through everything that broke: its image off the disk and
started, the data into its region, `l_start` with the machine's description
on its stack, the engine's own stack, its first error, `exit`, and back.
**The engine has to say why it stopped, the program has to say it stopped** -
which it can only do if `l_start` returned where it should - and nothing may
have died. One boot a game.

Usage: run_nogame.py IMAGE
"""

import os
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

# What the kernel says of a process a fault ended, on either board: "process
# died: general protection fault" on x86-64, `process "quake" died:
# instruction abort` on ARM.
DIED = " died: "

GAMES = [
    # `says`: the engine's own words when it gives up - Quake's `Sys_Error`,
    # and Doom's lookup of a lump the WAD does not have.
    {"name": "Quake", "program": "quake.lua", "image": "quake.elf",
     "data": "id1/pak0.pak", "bytes": b"PACK" + struct.pack("<II", 12, 0),
     "says": "Error: "},
    {"name": "Doom", "program": "doom.lua", "image": "doom.elf",
     "data": "doom1.wad", "bytes": b"IWAD" + struct.pack("<ii", 0, 12),
     "says": "W_GetNumForName"},
]


def installed(arch, work, game):
    """`host:guest` pairs putting the game with nothing to play on a disk."""
    name = game["name"]
    folder = "/Home/Apps/" + name

    # The engine's image as the gate links it against the test userland, or
    # `make apps` against the lean one - stripped here, into this suite's own
    # directory rather than the one every installed application shares.
    tested = os.path.join(ROOT, "build", "user-x86_64-test" if arch == "x86_64"
                          else "user-test", "apps", game["image"])
    built = tested if os.path.exists(tested) else os.path.join(
        ROOT, "build", "user-x86_64" if arch == "x86_64" else "user", "apps",
        game["image"])

    if not os.path.exists(built):
        sys.exit("FAIL: no %s to install - `make apps` builds it" % game["image"])

    objcopy = ("x86_64-elf-" if arch == "x86_64" else "aarch64-none-elf-") + "objcopy"
    elf = os.path.join(work, game["image"])
    subprocess.run([objcopy, "--strip-debug", built, elf], check=True)

    data = os.path.join(work, name.lower() + "-" + os.path.basename(game["data"]))

    with open(data, "wb") as f:
        f.write(game["bytes"])

    return [os.path.join(ROOT, "user", "installed", name, game["program"])
            + ":" + folder + "/" + game["program"],
            elf + ":" + folder + "/" + game["image"],
            data + ":" + folder + "/" + game["data"]]


def run(image, game):
    """One boot: the game started with nothing to play. The failures."""
    name = game["name"]
    low = name.lower()
    folder = "/Home/Apps/" + name

    import run_screenshot as R                                  # noqa: E402

    reached = "%s: %s/%s, 0 KB" % (low, folder, game["data"])
    stopped = "%s: %s stopped during startup" % (low, low)
    guest = R.Guest(image, 180)

    try:
        guest.wait_for("kosmos>", "a prompt")
        guest.type("wm %s/%s" % (folder, game["program"]))

        # Either way, a line says it: the program's, or the kernel's.
        for _ in range(900):
            guest._read_available()

            if stopped in guest.seen or DIED in guest.seen:
                break

            time.sleep(0.1)

        time.sleep(0.5)
        guest._read_available()
    finally:
        guest.close()

    said = guest.seen
    at = said.find(reached)

    if at < 0:
        return ["%s never got as far as its data:\n%s" % (name, said[-800:])]

    after = said[at:]

    if DIED in after:
        return ["%s's process died on the way:\n%s"
                % (name, after[after.find(DIED):][:600])]

    fails = []

    # What the engine said, up to the program's own last word.
    before = after[:after.find(stopped)] if stopped in after else after

    if game["says"] not in before:
        fails.append("%s did not say why it stopped:\n%s" % (name, after[:1200]))

    if stopped not in after:
        fails.append("the program never heard back from %s's start - l_start "
                     "did not return where it was armed" % name)

    game["why"] = next((line.strip() for line in after.splitlines()
                        if game["says"] in line), "")
    return fails


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    arch = "x86_64" if "x86_64" in image else "aarch64"
    work = scratch.directory("nogame")
    disk = os.path.join(work, "disk.img")
    pairs = []

    for game in GAMES:
        pairs += installed(arch, work, game)

    # Both games on one disk, booted once for each: the harness puts the disk
    # into QEMU's arguments when it is imported, so it is one disk a run.
    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "96"] + pairs,
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = disk
    fails = []

    for game in GAMES:
        fails += run(image, game)

    checks = 3 * len(GAMES)

    if fails:
        print("FAIL: %d of %d checks on Doom and Quake starting with no game:"
              % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on Doom and Quake starting with no game (each "
          "engine's data read and handed over, each saying why it stopped - "
          "%s - and back to Lua with nothing dead)."
          % (checks, "; ".join('%s "%s"' % (g["name"], g["why"]) for g in GAMES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
