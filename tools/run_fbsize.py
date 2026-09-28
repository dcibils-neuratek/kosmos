#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""One image at any screen size (`roadmap.md` 6zt).

The screen's size used to be compiled into the kernel, so each size was an
image of its own - and Diego, running the release at 3840x2160, was told
there was no image for it. Now QEMU hands the size over as
`opt/kosmos/fb=WxH` and `ramfb` allocates a screen that size when it
starts; without the option, or with one it cannot show, the image comes up
at the size it was built with and its boot log says why.

Three boots of the gate's own image, on whichever board it is for:

- **1280x720**: the boot log says the size and that `opt/kosmos/fb` asked
  for it, and the screen QEMU scans out is that size - which is the
  kernel, `ramfb` and QEMU agreeing, not only a number printed;
- **2560x1440**, the other way from the image's own 1920x1080, and the
  prompt drawn on it;
- **"banana"**: the size it was built with, and the log says the option
  asked for none it can show.

Usage: run_fbsize.py IMAGE
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

IMAGE = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"

ASKED = "at the size opt/kosmos/fb asked"
REFUSED = "opt/kosmos/fb asked for none it can show"


def boot(R, x86, base, value):
    """The image booted with `opt/kosmos/fb` set to `value`: what its boot
    log said about the display, and the size of the screen QEMU scanned
    out."""
    args = base + ["-fw_cfg", "name=opt/kosmos/fb,string=%s" % value]

    if x86:
        R.X86_ARGS = args
    else:
        R.QEMU_ARGS = args

    guest = R.Guest(IMAGE, 120)

    try:
        guest.wait_for("kosmos> ", "reached a prompt with opt/kosmos/fb=%s" % value)
        shot = guest.screendump()
    finally:
        guest.close()

    width, height, rgb = R.parse_ppm(shot)
    said = re.search(r"(\d+)x(\d+), 32-bit XRGB", guest.seen)
    source = next((line for line in guest.seen.splitlines() if "ramfb" in line), "")
    lit = sum(1 for i in range(0, len(rgb), 3 * 97) if rgb[i] or rgb[i + 1] or rgb[i + 2])

    return (said and (int(said.group(1)), int(said.group(2)))), source.strip(), \
        (width, height), lit


def main():
    import run_screenshot as R

    x86 = R.machine(IMAGE) == "x86_64"
    base = list(R.X86_ARGS if x86 else R.QEMU_ARGS)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    for value, want in (("1280x720", (1280, 720)), ("2560x1440", (2560, 1440))):
        said, source, shown, lit = boot(R, x86, base, value)
        check(said == want, "asked for %s, the boot log said %r" % (value, said))
        check(ASKED in source, "the log does not say %s was asked for: %r" % (value, source))
        check(shown == want, "asked for %s, QEMU scanned out %r" % (value, shown))
        check(lit > 0, "nothing was drawn on the %s screen" % value)

    said, source, shown, _ = boot(R, x86, base, "banana")
    check(said is not None and said == shown and said != (1280, 720),
          "a size it cannot show did not leave the image's own: log %r, screen %r"
          % (said, shown))
    check(REFUSED in source, "the log does not say the option was refused: %r" % source)

    if failed:
        print("FAIL: %d of %d checks on the screen's size as a boot option:"
              % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on the screen's size as a boot option, on %s (1280x720 and "
          "2560x1440 from one image, said and scanned out; \"banana\" left at the "
          "image's own %dx%d, and said)" % (checks, "x86-64" if x86 else "ARM",
                                           said[0], said[1]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
