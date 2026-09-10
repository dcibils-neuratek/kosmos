#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Boots Kosmos the way a laptop will: firmware, a loader, and an image.

**Every other x86 test in this tree boots through QEMU's `-kernel`, and
`-kernel` is not a loader.** It reads the multiboot header, copies the image
in and jumps. It does not answer the video request - measured, not assumed -
so the one path a machine with no ramfb depends on entirely had never run
once, while `tools/test_loaderfb.c` asked the decision fourteen careful
questions on the host and every one of them passed.

What that hid was three faults, none of them a driver and all of them fatal
and silent on a machine with no serial port:

  - the boot page tables mapped one gigabyte, and a multiboot loader may
    leave its information structure anywhere below four;
  - the largest usable low region under UEFI begins *above* the kernel
    image, so a subtraction that had always been positive wrapped;
  - the framebuffer was never mapped, because ramfb's pixels are in RAM and
    a firmware's are not. The first pixel faulted, the handler tried to
    report it, and the console lock was already held by the write that
    faulted.

So this boots the real artifact under OVMF - the same EDK II a ThinkPad's
firmware is built from - and **checks the screen rather than the serial
line**, because a machine whose framebuffer works stops talking to the
serial line at stage six. That is the point: the picture is the result.
"""

import os
import shutil
import socket
import subprocess
import sys
import tempfile
import time

QEMU = "qemu-system-x86_64"

# The colours Kosmos draws with, from `user/lib/ui.lua`'s palette and the
# kernel's own boot screen. Sampled from a good boot rather than guessed.
GROUND = (13, 17, 23)           # the ground the whole desktop sits on
GREEN  = (63, 185, 80)          # the boot log's stage headings
RED    = (204, 34, 51)          # the wordmark

# The mode OVMF's GOP offers, which is *not* the 1920x1080 ramfb is asked
# for - so the size alone says which of the two answered.
LOADER_MODE = (1280, 800)


def firmware():
    """OVMF's two halves, or None with a reason."""
    prefix = subprocess.run(["brew", "--prefix", "qemu"], capture_output=True,
                            text=True).stdout.strip()

    if not prefix:
        return None, "homebrew has no qemu prefix to find OVMF under"

    code = os.path.join(prefix, "share/qemu/edk2-x86_64-code.fd")
    varsfd = os.path.join(prefix, "share/qemu/edk2-i386-vars.fd")

    if not os.path.exists(code) or not os.path.exists(varsfd):
        return None, "OVMF is not installed beside qemu"

    return (code, varsfd), None


def capture(iso, moments):
    """Boots once and screendumps at each moment; returns frames and serial.

    **One boot, several pictures.** The early screen and the finished
    desktop are two claims about the same machine, and booting twice to make
    them would double what this costs and still not prove they were the same
    boot.
    """
    fw, why = firmware()

    if fw is None:
        return None, None, None, why

    code, varsfd = fw
    work = tempfile.mkdtemp()
    mon = os.path.join(work, "mon")
    writable = os.path.join(work, "vars.fd")

    shutil.copy(varsfd, writable)

    #
    # **As a USB stick, not as a CD**, and the difference is the whole
    # point of this harness.
    #
    # `-cdrom` puts the image on the SATA controller and the firmware boots
    # it as an optical device. Nothing on a ThinkPad does that: the image
    # goes on a stick, the firmware finds it over xHCI with its own USB
    # stack, and boots it as `UEFI QEMU USB HARDDRIVE`.
    #
    # **And it is the image `make usb` writes, byte for byte.** It used to
    # be a `grub-mkrescue` ISO, which passed every check here and then
    # dropped to `grub rescue>` on the machine, unable to find its own
    # modules. A harness that boots something other than what is shipped
    # tests the harness.
    #
    cmd = [QEMU, "-M", "q35", "-m", "4G", "-no-reboot",
           "-vga", "std", "-display", "none", "-serial", "stdio",
           "-monitor", "unix:%s,server,nowait" % mon,
           "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + code,
           "-drive", "if=pflash,format=raw,unit=1,file=" + writable,
           "-device", "qemu-xhci,id=xhci",
           "-drive", "if=none,id=stick,format=raw,file=" + iso,
           "-device", "usb-storage,bus=xhci.0,drive=stick",
           # Four, so that "how many processors" has an answer worth
           # checking: ACPI's MADT is the only place that number is.
           "-smp", "4"]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    os.set_blocking(p.stdout.fileno(), False)

    out = b""
    frames = []
    start = time.time()

    try:
        for n, moment in enumerate(moments):
            while time.time() - start < moment:
                chunk = p.stdout.read()

                if chunk:
                    out += chunk

                time.sleep(0.2)

            ppm = os.path.join(work, "screen%d.ppm" % n)

            s = socket.socket(socket.AF_UNIX)
            s.connect(mon)
            time.sleep(0.4)
            s.recv(65536)
            s.sendall(("screendump %s\n" % ppm).encode())
            time.sleep(2.5)
            s.close()

            if not os.path.exists(ppm):
                return None, "the monitor wrote no screendump"

            with open(ppm, "rb") as f:
                raw = f.read()

            # P6\n<w> <h>\n255\n then three bytes a pixel.
            parts = raw.split(b"\n", 3)
            width, height = (int(x) for x in parts[1].split())
            frames.append((width, height, parts[3]))
    finally:
        p.kill()
        p.wait()

    return frames, out.decode("utf-8", "replace")


def share(pixels, colour):
    """What fraction of the screen is exactly this colour."""
    r, g, b = colour
    n = 0

    for i in range(0, len(pixels) - 2, 3):
        if pixels[i] == r and pixels[i + 1] == g and pixels[i + 2] == b:
            n += 1

    return n / (len(pixels) / 3.0)


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos-usb.img"
    checks = 0
    fails = []

    def check(ok, complaint):
        nonlocal checks

        if ok:
            checks += 1
        else:
            fails.append(complaint)

    if not os.path.exists(iso):
        print("SKIP: no %s. Run `make x86-usb-image`." % iso)
        return 0

    frames, serial = capture(iso, (30.0,))

    if frames is None:
        print("SKIP: %s" % serial)
        return 0

    width, height, pixels = frames[0]

    #
    # **The boot log reached the panel before the display stage.**
    #
    # `hal_fb_early` exists because a laptop has no serial port, and the
    # three faults that stood between this kernel and its first real machine
    # were at stages three, four and five - each of them, on that machine, a
    # black panel with nothing to read.
    #
    # **Asked of the machine rather than of a stopwatch**, and the first
    # version of this check was a stopwatch: a screendump seven seconds in,
    # on the theory that the display stage had not been reached yet. It
    # passed with the whole feature disabled, because OVMF, GRUB and twelve
    # boot stages take under two seconds together - so by any moment worth
    # sampling the screen is Kosmos's either way. Sampling every half second
    # from two seconds found no window at all.
    #
    # The machine knows which of the two ways its panel got the log, and now
    # says so. That is a line that changes rather than a picture nobody
    # compared.
    #
    #
    # **And the framebuffer is write-combining**, which is the half of that
    # question emulation can answer.
    #
    # How *fast* it is cannot be measured here: QEMU's framebuffer is host
    # memory and TCG models no cache, so uncached and write-combining run
    # identically. Whether the mapping is the right one is not a measurement
    # at all - it is the PAT programmed, a bit set in a page table entry,
    # and the architecture manual. The machine says which it got.
    #
    # The same line catches the fault underneath it: `start.S` maps the
    # first four gigabytes plain present-and-writable, which is write-back,
    # and write-back is the one memory type MMIO may not have.
    #
    #
    # **ACPI is visible, which is the whole reason there is a second boot
    # header.**
    #
    # `find_rsdp` looks where a BIOS leaves the pointer; UEFI passes it to
    # the loader and leaves nothing behind. Multiboot 1 has no tag to carry
    # one and Multiboot 2 has two - so before this the same image reported
    # one processor and fell back to a pair of 8259s here, while reporting
    # four and driving the local APIC under QEMU's `-kernel`. Every line of
    # the ACPI, APIC and MSI work was dead on the one path that matters.
    #
    # Four processors are asked for below, so a machine that says one is a
    # machine that did not read the MADT.
    #
    check("-> 4 processors" in serial,
          "the machine found "
          + next((l.strip() for l in serial.splitlines()
                  if "processor" in l and "->" in l), "no processor line")
          + "; ACPI is not reaching the kernel through the loader")

    #
    # Anchored on the phrase the boot fact prints when the APIC is running,
    # not on "I/O APIC" alone: every sentence that explains a fallback names
    # the I/O APIC too - "because nothing answers at the I/O APIC's address" -
    # so the short match would pass on the very boot this exists to catch.
    #
    check("interrupts: an I/O APIC" in serial,
          "booted through the loader the machine fell back to the 8259 "
          "pair, which is what no ACPI looks like")

    check("write-combining" in serial,
          "the framebuffer is not write-combining: "
          + next((l.strip() for l in serial.splitlines()
                  if "from the loader" in l), "and the loader did not answer"))

    check("the panel has had this log since stage two" in serial,
          "the screen was attached at the display stage rather than at the "
          "second one, so stages one to five reached nothing but a serial "
          "port that a laptop does not have")

    # 1. The loader answered the video request, and the mode says which one
    #    did: ramfb is asked for 1920x1080 and the firmware's GOP is its
    #    own size. A fallback to ramfb would be the wrong size *and* would
    #    mean the laptop path did not run.
    check((width, height) == LOADER_MODE,
          "the screen is %dx%d and OVMF's GOP is %dx%d; something other than "
          "the loader's framebuffer answered"
          % (width, height, LOADER_MODE[0], LOADER_MODE[1]))

    # 2. Kosmos owns the screen rather than the firmware. Before the
    #    framebuffer was mapped this capture was TianoCore's logo and GRUB's
    #    `Booting 'Kosmos'` - 1.8% of the screen lit, and every pixel of it
    #    somebody else's.
    ground = share(pixels, GROUND)

    check(ground > 0.5,
          "only %.1f%% of the screen is Kosmos's ground colour; the picture "
          "is still the firmware's" % (100.0 * ground))

    # 3. And it drew its own content into it: the boot log's headings and
    #    the wordmark. A cleared screen and a drawn one are the same
    #    fraction of ground.
    check(share(pixels, GREEN) > 0.001,
          "the boot log's green is not on the screen")
    check(share(pixels, RED) > 0.0005,
          "the wordmark is not on the screen")

    # 4. **The deadlock, which is the one that cannot report itself.** An
    #    unmapped framebuffer faults inside a console write, and the fault
    #    handler blocks on the lock that write is holding. The machine then
    #    says this, for ever, and a laptop says nothing at all.
    check("spinlock: console held" not in serial,
          "the console lock deadlocked, which is an unmapped framebuffer "
          "faulting inside a console write")

    check("PANIC" not in serial,
          "it panicked: " + next((l.strip() for l in serial.splitlines()
                                  if "PANIC" in l), "?"))

    # 5. It got far enough to hand over. The serial line goes quiet at stage
    #    six precisely because the screen took the console, so the last
    #    thing it should say is the virtual memory stage.
    check("[5/12]" in serial,
          "it did not reach the virtual memory stage on the serial line")

    # And that the firmware really did take the USB path rather than
    # falling back to something else that happened to work.
    check("USB" in serial,
          "the firmware did not boot this as a USB device, so the path a "
          "stick takes is not the path this checked")

    # 6. And the pitch fact is about this machine rather than a guess. It
    #    read `7680 bytes a row, not 7680: padded` for a while, which is a
    #    boot fact contradicting itself in one line.
    check("bytes a row, not " not in serial
          or "bytes a row, which is width" not in serial,
          "the boot log claims the pitch is both padded and not")

    if fails:
        print("FAIL: %d of %d checks booting through GRUB under UEFI:"
              % (len(fails), len(fails) + checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks booting through GRUB under UEFI (the firmware "
          "sets a mode, the loader passes it on, and Kosmos draws its own "
          "%dx%d screen into memory nothing else in this project has ever "
          "reached)." % (checks, width, height))
    return 0


if __name__ == "__main__":
    sys.exit(main())
