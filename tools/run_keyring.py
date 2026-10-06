#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The keyring, on the machine (`docs/keyring.md`, step K4).

Three boots, each of a disk made here:

  1. **A disk made before there was a keyring** - no `/Keyring` on it. The
     keyring makes the folder and a key, writes an empty keyring, and the
     `keyring` program - from the image, so handed the `manage` door - says
     "0 entries, the file new". Through that door, what only smbfs's may ask
     is refused: `keyring get` answers "not this door's to ask". **And the
     forgery**: the same program copied into `/Home`, saying the same
     `kosmos: needs keyring`, is handed nothing.
  2. **The same disk again**: the file the first boot wrote opens - "the
     file opened" - with the key the first boot made, read back.
  3. **A keyring that does not open**: the disk carries a key and, at
     `/Keyring/keyring`, bytes that are not a keyring. It is kept aside under
     a name of its own, byte for byte, and an empty one begun - "the file set
     aside" - and nothing is overwritten.

Afterwards each disk is read on the Mac (`tools/kfs.lua`): the key is 32
bytes, and in the third the set-aside file holds exactly the bytes put
there.

What this cannot hold yet, said: an entry put and got back, which only
smbfs's door can put (K5); and the disk server's second door refusing
`/Home`, which has no caller but the keyring (`testing.md` 18.418).

Usage: run_keyring.py IMAGE
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

import scratch                                              # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")
KFS = os.path.join(HERE, "kfs.lua")
PROGRAM = os.path.join(ROOT, "user", "bin", "programs", "keyring.lua")
# **The size of a keyring of one entry** (`keyfile_bytes(1)`: a 44-byte
# header, 8 of counts, an 832-byte entry, a 16-byte tag), so it is refused by
# the seal and not by its size - the path a damaged keyring takes.
GARBAGE = (b"not a keyring, and kept as it is: " + bytes(range(256)) * 4)[:900]


def kfs(*args):
    return subprocess.run([LUA, KFS, *args], check=True, capture_output=True,
                          text=True, cwd=ROOT).stdout


def boot(R, image, commands, last):
    """Boot, run each command at the prompt, and wait for `last`."""
    guest = R.Guest(image, 120)

    try:
        guest.wait_for("kosmos>", "a prompt")

        for c in commands:
            guest.type(c)
            time.sleep(0.3)

        guest.wait_for(last, "the last answer")
        time.sleep(0.5)
        guest._read_available()
    finally:
        guest.close()

    return guest.seen


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("keyring")
    disk = os.path.join(work, "disk.img")
    fails, checks = [], 0

    def check(ok, what):
        nonlocal checks
        checks += 1
        if not ok:
            fails.append(what)

    # The forgery: the image's own program, in /Home.
    forged = os.path.join(work, "forged.lua")
    shutil.copy(PROGRAM, forged)

    kfs("create", disk, "32", forged + ":/Home/forged.lua")
    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                              # noqa: E402

    # 1. No /Keyring yet.
    said = boot(R, image, ["keyring", "keyring get smb://10.0.2.2 diego",
                           "run /Home/forged.lua", "echo DONE-ONE"], "DONE-ONE")
    check("keyring: a key made for this machine" in said,
          "the first boot did not say a key was made")
    check(re.search(r"^0 entries, the file new", said, re.M) is not None,
          "a fresh keyring was not '0 entries, the file new':\n" + said[-600:])
    check("keyring: not this door's to ask" in said,
          "the manage door was not refused what only smbfs's may ask")
    check("keyring: this program was not handed the keyring" in said,
          "the program copied into /Home was handed the keyring")
    listing = kfs("ls", disk, "/Keyring")
    check("machine-key" in listing and "keyring" in listing,
          "/Keyring does not hold the key and the file:\n" + listing)

    key = os.path.join(work, "key.out")
    kfs("get", disk, "/Keyring/machine-key", key)
    check(os.path.getsize(key) == 32, "the key is %d bytes" % os.path.getsize(key))

    # 2. The same disk again.
    said = boot(R, image, ["keyring", "echo DONE-TWO"], "DONE-TWO")
    check(re.search(r"^0 entries, the file opened", said, re.M) is not None,
          "the second boot did not open the first's file:\n" + said[-600:])
    check("a key made" not in said, "the second boot made another key")

    # 3. A keyring that does not open, beside a key.
    garbage = os.path.join(work, "garbage")
    with open(garbage, "wb") as f:
        f.write(GARBAGE)

    os.remove(disk)
    kfs("create", disk, "32", key + ":/Keyring/machine-key",
        garbage + ":/Keyring/keyring")
    said = boot(R, image, ["keyring", "echo DONE-THREE"], "DONE-THREE")
    check(re.search(r"^0 entries, the file set aside", said, re.M) is not None,
          "a keyring that does not open was not set aside:\n" + said[-600:])
    check("kept aside as /Keyring/keyring.unopened-" in said,
          "the log did not say where it was kept")

    aside = re.findall(r"(keyring\.unopened-\d+)", kfs("ls", disk, "/Keyring"))
    check(len(aside) == 1, "the set-aside file is not in /Keyring once: %r" % aside)

    if aside:
        back = os.path.join(work, "aside.out")
        kfs("get", disk, "/Keyring/" + aside[0], back)
        with open(back, "rb") as f:
            check(f.read() == GARBAGE, "the set-aside file is not the bytes put there")

    if fails:
        print("FAIL: %d of %d checks on the keyring:\n  %s"
              % (len(fails), checks, "\n  ".join(fails)))
        return 1

    print("PASS: %d checks on the keyring (a disk without /Keyring given one, "
          "a key and an empty keyring; the manage door refused smbfs's "
          "question; the program copied into /Home handed nothing; the file "
          "opened again on the next boot; a keyring that did not open kept "
          "aside byte for byte and an empty one begun)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
