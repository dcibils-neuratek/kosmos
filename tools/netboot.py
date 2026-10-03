#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""What a boot server hands the M700, laid out in a folder (`make netboot`).

Diego, 2 October 2026: "is there any way we can simulate the e1000 driver in
qemu so i dont have to build sticks all the time?", then "lets try network
boot" (`roadmap.md`, the M700 booted over the network). The machine's
firmware fetches `bootx64.efi` - Kosmos's own loader - by TFTP, and the
loader fetches the rest beside it (`boot/efi/loader.c`, `source_read`):

    bootx64.efi           the loader, as on a stick
    boot/kosmos.bin       the kernel and the userland image
    boot/kosmos.head      its first 36 KB, so the loader can claim the
                          kernel's place before fetching the rest
    boot/kosmos.sums      the build's sums, which the loader holds it to
    boot/kosmos.cmdline   the words the kernel is told - taken from the stick
                          in the machine, so that `/Home` is that stick's

**`/Home` stays on the stick**, written once and left plugged in: its
command line names its own partition, and a boot over the network has to
name the same one, or the machine starts with an empty `/Home` in memory.
So the words come out of the stick's image - the newest development one
unless `--stick` says otherwise - read off its FAT the way `run_uefi.py`
reads them.

Usage: netboot.py KERNEL --loader BOOTX64.EFI [--stick IMAGE] [--out DIR]
"""

import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

from mkusb_image import page_sums                           # noqa: E402

ESP_AT = 34 * 512           # the ESP starts at the GPT's first usable sector
HEAD_BYTES = 36 * 1024      # the loader's `head`, in `boot/efi/loader.c`


def stick_words(image):
    """What the stick tells the kernel, off its ESP, or None."""
    got = subprocess.run(["mtype", "-i", "%s@@%d" % (image, ESP_AT),
                          "::/boot/kosmos.cmdline"], capture_output=True)

    if got.returncode != 0:
        return None

    return got.stdout.decode("utf-8", "replace").strip()


def newest_stick():
    """The newest development stick this tree built, which is the one most
    likely to be in the machine."""
    images = glob.glob(os.path.join(ROOT, "build", "x86_64",
                                    "kosmos-usb-*-development.img"))

    return max(images, key=os.path.getmtime) if images else None


def main():
    args = sys.argv[1:]
    kernel = args.pop(0) if args and not args[0].startswith("--") else \
        os.path.join(ROOT, "build", "x86_64", "kosmos.bin")
    loader, stick, out, words = None, None, os.path.join(ROOT, "build", "netboot"), None

    while len(args) >= 2 and args[0] in ("--loader", "--stick", "--out", "--words"):
        if args[0] == "--loader":
            loader = args[1]
        elif args[0] == "--stick":
            stick = args[1]
        elif args[0] == "--out":
            out = args[1]
        else:
            words = args[1]

        args = args[2:]

    if loader is None or not os.path.isfile(loader):
        sys.exit("netboot: no loader at %s - `make x86-build` and the loader "
                 "build it" % loader)

    if not os.path.isfile(kernel):
        sys.exit("netboot: no kernel at %s - `make x86-build`" % kernel)

    if words is None:
        stick = stick or newest_stick()

        if stick is None or not os.path.isfile(stick):
            sys.exit("netboot: no stick image to take the command line from; "
                     "give --stick, or --words")

        words = stick_words(stick)

        if words is None:
            sys.exit("netboot: %s has no boot/kosmos.cmdline to take" % stick)

    boot = os.path.join(out, "boot")
    os.makedirs(boot, exist_ok=True)

    shutil.copyfile(loader, os.path.join(out, "bootx64.efi"))
    shutil.copyfile(kernel, os.path.join(boot, "kosmos.bin"))

    with open(kernel, "rb") as f:
        head = f.read(HEAD_BYTES)

    with open(os.path.join(boot, "kosmos.head"), "wb") as f:
        f.write(head)

    with open(os.path.join(boot, "kosmos.sums"), "wb") as f:
        f.write(page_sums(kernel))

    with open(os.path.join(boot, "kosmos.cmdline"), "w") as f:
        f.write(words + "\n")

    print("netboot: %s holds the loader, a %.1f MB kernel, its sums and its "
          "command line" % (out, os.path.getsize(kernel) / 1048576.0))
    print("netboot: the kernel is told: %s" % words)

    if stick:
        print("netboot: /Home is the stick from %s, which stays in the "
              "machine" % os.path.basename(stick))


if __name__ == "__main__":
    main()
