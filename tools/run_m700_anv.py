#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The 3D Kit's scene on the M700's GPU, through Mesa's Intel driver.

    python3 tools/run_m700_anv.py [ADDRESS]      (192.168.1.35 by default)

The check `testing.md` 18.955 left on the machine, which the gate cannot
reach: the sky and one box drawn by `anv` in the 3D Kit, the frame that
hung the HD 530's render engine until `intelgpu` gave it i915's
whitelist. It passes when the scene answers `scene=64x64 boxes=1` and the
driver reported no hang while it ran; it fails, and says which, otherwise.

`anvtest.elf` goes over under a name of its own each time, the build's
modification time in it: init keeps an image it made under its file's
name, size and mtime, and a stripped build of the same size put over the
old one would run the old one until the machine restarted. The machine
needs no packages for it; the files go to /Home/Temporary/anv.
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
IMAGE = os.path.join(ROOT, "build/user-x86_64/apps/anvtest.elf")
FOLDER = "/Home/Temporary/anv"

SCENE = """-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- kosmos: image {image}
local kit = use("anvtest.elf")
print("k3d scene: " .. tostring(kit.scene({{eye = {{0.0, 3.0, 8.0}},
  target = {{0.0, 1.0, 0.0}}, focal = 1.2, near = 0.1, far = 200.0,
  sun = {{0.4, 1.0, 0.3}}, ambient = 0.3, zenith = {{0.2, 0.4, 0.8}},
  horizon = {{0.8, 0.85, 0.9}}, haze = {{0.7, 0.75, 0.8}}, haze_start = 6.0,
  haze_end = 40.0, boxes = {{{{-1.0, 0.0, -1.0, 1.0, 2.0, 1.0, 220, 60, 40}}}},
  size = 64}})))
"""


def telnet(address, *args):
    done = subprocess.run([sys.executable, os.path.join(HERE, "kosmos_telnet.py"), address, *args],
                          capture_output=True, text=True, timeout=600)
    return done.returncode, done.stdout + done.stderr


def hangs(address):
    _, out = telnet(address, "run", "log stood at acthd")
    return re.findall(r"^\[[\d.]+\] intelgpu: the render engine stood at .*$", out, re.M)


def main(argv):
    address = argv[1] if len(argv) > 1 else "192.168.1.35"

    if not os.path.exists(IMAGE):
        print(f"m700-anv: {IMAGE} is not built - make ARCH=x86_64 apps")
        return 1

    stamp = int(os.path.getmtime(IMAGE))
    image = f"anvtest-{stamp}.elf"

    with tempfile.TemporaryDirectory() as scratch:
        stripped = os.path.join(scratch, image)
        script = os.path.join(scratch, "anvscene.lua")
        subprocess.run(["x86_64-elf-strip", "-o", stripped, IMAGE], check=True)
        with open(script, "w") as f:
            f.write(SCENE.format(image=image))

        before = hangs(address)
        for local, remote in ((stripped, f"{FOLDER}/{image}"), (script, f"{FOLDER}/anvscene.lua")):
            code, out = telnet(address, "put", local, remote)
            if code != 0:
                print(f"m700-anv: could not put {remote}: {out.strip()}")
                return 1

    _, out = telnet(address, "run", f"{FOLDER}/anvscene.lua")
    said = re.search(r"k3d scene: .*", out)
    after = hangs(address)
    new = [line for line in after if line not in before]   # each carries its time

    print(f"m700-anv: {said.group(0) if said else out.strip()}")
    for line in new:
        print(f"m700-anv: {line}")

    if not said or "scene=64x64 boxes=1" not in said.group(0):
        print("m700-anv: FAIL - the scene did not finish on the GPU")
        return 1
    if new:
        print("m700-anv: FAIL - the render engine hung while it ran")
        return 1

    print("m700-anv: pass - the sky and a box drawn by anv on the HD 530")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
