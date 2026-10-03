#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Kosmos booted over the network, with /Home on a stick (`testing.md` 18.348).

Diego, 2 October 2026: "is there any way we can simulate the e1000 driver in
qemu so i dont have to build sticks all the time?", then "lets try network
boot" (`roadmap.md`, the M700 booted over the network). The M700's firmware
fetches Kosmos's loader by PXE, the loader fetches the kernel beside it by
TFTP through the firmware's own protocol (`boot/efi/loader.c`,
`source_read`), and `/Home` stays on the stick in the machine, named by the
command line `tools/netboot.py` took off that stick.

This is that, under OVMF: QEMU's own DHCP and TFTP serving the folder
`netboot.py` laid out, the network card first in the boot order, and the
`/Home` stick on USB as the M700's is - so the stick must *not* be what
boots. Checked, in the order it happens:

- the firmware fetched the loader over the network, not off the stick;
- the loader said where it came from and fetched the kernel from there;
- the kernel was the build's, page for page, and said so on its own line;
- `/Home` is the stick's Kosmos partition, where its command line points.

**OVMF's network stack starts only with a source of randomness** - EDK II
has required one since its TCP and DHCP were made unpredictable - and
QEMU's default processor has no RDRAND, so a virtio-rng device is given it.
The M700's processor has RDRAND; this is QEMU's need, not Kosmos's.

Usage: run_netboot.py FOLDER HOME_STICK
"""

import os
import re
import shutil
import struct
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402
from run_uefi import firmware                               # noqa: E402

QEMU = "qemu-system-x86_64"


def main():
    folder = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/netboot-test"
    stick = sys.argv[2] if len(sys.argv) > 2 else "build/x86_64/kosmos-uefi-home.img"
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    fw, why = firmware()

    if fw is None:
        print("SKIP: no firmware to boot over the network with: %s" % why)
        return 0

    for path in (os.path.join(folder, "bootx64.efi"),
                 os.path.join(folder, "boot", "kosmos.bin"), stick):
        if not os.path.isfile(path):
            print("FAIL: no %s - `make gate-images` lays the folder out" % path)
            return 1

    code, varsfd = fw
    work = scratch.directory()
    writable = os.path.join(work, "vars.fd")
    shutil.copy(varsfd, writable)

    # The partition the stick names, off its GPT as `run_uefi.py` reads it.
    with open(stick, "rb") as f:
        f.seek(512)
        header = f.read(92)
        f.seek(struct.unpack_from("<Q", header, 72)[0] * 512 + 128)
        second = f.read(128)

    first, last = struct.unpack_from("<QQ", second, 32)

    cmd = [QEMU, "-M", "q35", "-m", "4G", "-no-reboot",
           "-vga", "none", "-display", "none", "-serial", "stdio",
           "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + code,
           "-drive", "if=pflash,format=raw,unit=1,file=" + writable,
           "-netdev", "user,id=n0,tftp=%s,bootfile=bootx64.efi"
           % os.path.abspath(folder),
           "-device", "virtio-net-pci,netdev=n0,romfile=,bootindex=0",
           "-device", "virtio-rng-pci",
           "-device", "qemu-xhci,id=xhci",
           "-drive", "if=none,id=stick,format=raw,snapshot=on,file=" + stick,
           "-device", "usb-storage,bus=xhci.0,drive=stick,bootindex=1"]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    os.set_blocking(p.stdout.fileno(), False)

    out, typed = b"", False
    start = quiet = time.time()

    try:
        while time.time() - start < 180.0:
            chunk = p.stdout.read()

            if chunk:
                out += chunk
                quiet = time.time()
            else:
                time.sleep(0.1)

            if not typed and b"kosmos>" in out and time.time() - quiet > 0.5:
                p.stdin.write(b"diskinfo\n")
                p.stdin.flush()
                typed = True

            if typed and out.count(b"kosmos>") >= 2:
                time.sleep(0.5)
                out += p.stdout.read() or b""
                break
    finally:
        p.kill()
        p.wait()

    said = re.sub(r"\x1b\[[0-9;=?]*[A-Za-z]", "",
                  out.decode("utf-8", "replace")).replace("\r", "")
    shown = "\n    ".join(l.strip() for l in said.splitlines()
                           if "kosmos-boot" in l or "PXE" in l or "NBP" in l
                           or "the loader:" in l or "disk:" in l
                           or "Kosmos partition" in l)

    check("Start PXE over IPv4" in said
          and "UEFI PXEv4" in said and "no \\boot\\disk.img on this stick" not in said,
          "the firmware did not boot the loader over the network - or booted "
          "the stick instead:\n    " + shown)

    check(re.search(r"this loader came over the network: this machine at "
                    r"10\.0\.2\.15, its files from 10\.0\.2\.2", said) is not None,
          "the loader did not say it came over the network, and from where:"
          "\n    " + shown)

    check(re.search(r"the kernel: \d+ KB fetched from 10\.0\.2\.2", said)
          is not None
          and "the kernel is the build's, page for page" in said,
          "the kernel was not fetched over the network and held to the "
          "build's sums:\n    " + shown)

    check("fetched over the network, against the build: same" in said,
          "the kernel's own line did not say it was fetched over the network "
          "and was the build's:\n    " + shown)

    check(("on the Kosmos partition on USB unit 0, blocks %d to %d"
           % (first, last)) in said,
          "/Home was not the stick's Kosmos partition, blocks %d to %d:\n    %s"
          % (first, last, shown))

    elapsed = time.time() - start

    if failed:
        print("FAIL: %d of %d checks on booting over the network:"
              % (len(failed), checks))
        for f in failed:
            print("  " + f)
        return 1

    print("PASS: %d checks on booting over the network (the loader by PXE, "
          "the kernel by TFTP and held to the build's sums, /Home on the "
          "stick), %.0f s to the prompt." % (checks, elapsed))
    return 0


if __name__ == "__main__":
    sys.exit(main())
