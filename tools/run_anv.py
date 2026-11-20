#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`anv` on Kosmos (`docs/gpu3d.md` G7a).

`anvtest.elf` - Intel's Vulkan driver with Kosmos's kernel-driver type, and
Intel's compiler under it, in one image with `user/kits/anvtest/` - run
from a /Home of its own under QEMU, where there is no Intel GPU, and held
to two answers:

- **no device**: `anv` asks for `/Devices/gpu`, the driver says it has no
  Intel GPU, and an instance is made and enumerates none - without an
  error, since a machine with no GPU is not a broken one;
- **Gen9's instructions**: the compute shader in `store.comp`, SPIR-V
  compiled on the host, through Mesa's SPIR-V front end, NIR and `brw` for
  the M700's HD 530 (`8086:1912`, Mesa's own table): generation 9, a SIMD
  width Gen9 has, the store and the thread's end among the sends, no
  spills, and code whose size is a whole number of instructions.

`KOSMOS_ANV_DEVICE` names a PCI id for the compile other than the HD
530's; `KOSMOS_ANV_SERIAL` keeps the guest's console.

Usage: run_anv.py IMAGE [APPS]
"""

import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                               # noqa: E402

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
APPS = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, "build", "user-x86_64", "apps")
WORK = scratch.directory("anv")
HOME_DISK = os.path.join(WORK, "home.img")
LUA = os.path.join(ROOT, "build", "host", "lua")

DEVICE = int(os.environ.get("KOSMOS_ANV_DEVICE", "0x1912"), 16)

PROGRAM = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- anv's test program (`tools/run_anv.py`).
-- kosmos: image anvtest.elf
local kit = use("anvtest.elf")
print("anvtest devices: " .. kit.devices())
print("anvtest compile: " .. kit.compile(%d))
""" % DEVICE

COMPILED = re.compile(r"gen=(\d+) simd=(\d+) instrs=(\d+) sends=(\d+) "
                      r"spills=(\d+) fills=(\d+) bytes=(\d+)$")


def judge(devices, compiled):
    """What is wrong with the two answers, or nothing."""
    wrong = []
    if devices != "devices=0":
        wrong.append("devices: wanted none found and no error, said %r" % devices)
    m = COMPILED.match(compiled or "")
    if m is None:
        wrong.append("compile: no instructions, said %r" % compiled)
        return wrong
    gen, simd, instrs, sends, spills, fills, size = (int(g) for g in m.groups())
    if gen != 9:
        wrong.append("compile: generation %d, where 8086:%04x is Gen9" % (gen, DEVICE))
    if simd not in (8, 16, 32):
        wrong.append("compile: a SIMD width of %d, which Gen9 has not" % simd)
    if sends < 2:
        # The store, and the end of the thread - itself a send on Gen9.
        wrong.append("compile: %d sends, where the store and the thread's end are two" % sends)
    if spills or fills:
        wrong.append("compile: %d spills and %d fills in a shader of a few registers"
                     % (spills, fills))
    if instrs < sends or size == 0 or size % 16 or size // 16 < instrs // 2:
        # An instruction is 16 bytes, or 8 compacted.
        wrong.append("compile: %d bytes for %d instructions" % (size, instrs))
    return wrong


def main():
    built = os.path.join(APPS, "anvtest.elf")

    if not os.path.exists(built):
        print("FAIL: no %s - `make ARCH=x86_64 apps` builds it" % built)
        return 1

    image = os.path.join(WORK, "anvtest.elf")
    subprocess.run(["x86_64-elf-objcopy", "--strip-debug", built, image], check=True)

    with open(os.path.join(WORK, "anvtest.lua"), "w") as f:
        f.write(PROGRAM)

    megabytes = os.path.getsize(image) // (1 << 20)
    subprocess.run([LUA, os.path.join(HERE, "kfs.lua"), "create", HOME_DISK,
                    str(megabytes + 32)]
                   + ["%s:/Home/apps/anv/%s" % (os.path.join(WORK, n), n)
                      for n in ("anvtest.lua", "anvtest.elf")],
                   check=True, capture_output=True, cwd=ROOT)
    os.environ["KOSMOS_DISK"] = HOME_DISK

    import run_screenshot as R                               # noqa: E402

    guest = R.Guest(IMAGE, 180)
    said = {}

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        mark = len(guest.seen)
        started = time.monotonic()
        guest.type("run /Home/apps/anv/anvtest.lua")

        while time.monotonic() < started + 240 and "compile" not in said:
            for line in guest.seen[mark:].split("\n")[1:-1]:
                line = line.strip()
                for what in ("devices", "compile"):
                    head = "anvtest %s: " % what
                    if line.startswith(head) and what not in said:
                        said[what] = line[len(head):]
            if "compile" not in said:
                time.sleep(0.2)
                guest._read_available()
        took = time.monotonic() - started
    finally:
        guest.close()

    if os.environ.get("KOSMOS_ANV_SERIAL"):
        with open(os.environ["KOSMOS_ANV_SERIAL"], "w") as f:
            f.write(guest.seen)

    wrong = judge(said.get("devices"), said.get("compile"))
    if wrong:
        print("FAIL: anv on Kosmos did not say what it must")
        for w in wrong:
            print("  " + w)
        if "compile" not in said:
            print("--- the guest's last words ---\n" + guest.seen[mark:][-1500:])
        return 1

    print("PASS: anv on Kosmos, from a %.1f MB image, in %.1f s: %s; 8086:%04x %s"
          % (os.path.getsize(image) / 1e6, took, said["devices"], DEVICE, said["compile"]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
