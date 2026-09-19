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

Given a second image, whose kernel is zeros, it boots that one too, and the
loader's refusal has to be on the screen while it waits for a key - drawn by
the loader itself, because the ThinkPad's firmware console showed none of it.

**And it boots the first stick once more with the screen where the ThinkPad's
firmware puts it**: 0x4000000000, above the four gigabytes the boot page
tables map, at 1920x1080. Under OVMF the screen is at 0x80000000, and the
early screen was dark on that machine for months while this harness passed.
"""

import os
import re
import shutil
import socket
import struct
import subprocess
import sys
import scratch
import threading
import time
import uuid

QEMU = "qemu-system-x86_64"

# The colours Kosmos draws with, from `user/lib/ui.lua`'s palette and the
# kernel's own boot screen. Sampled from a good boot rather than guessed.
GROUND = (13, 17, 23)           # the ground the whole desktop sits on
GREEN  = (63, 185, 80)          # the boot log's stage headings
RED    = (204, 34, 51)          # the wordmark

# The mode OVMF's GOP offers, which is *not* the 1920x1080 ramfb is asked
# for - so the size alone says which of the two answered.
LOADER_MODE = (1280, 800)

# The loader's own lines, in the colour `draw_line` in `boot/efi/loader.c`
# gives them, on the kernel's GROUND.
LOADER_INK = (0xE6, 0xED, 0xF3)

# Long enough for OVMF and the loader to reach a refusal under TCG, which
# then waits for a key for ever.
REFUSAL_AT = 15.0

#
# **Where the ThinkPad's firmware puts its screen, which OVMF never does**:
# 256 GB up, past the four gigabytes `start.S` maps, at the panel's own size.
# Every other boot here has its screen at 0x80000000, so the early screen was
# checked for months on the one layout the machine it was written for does
# not have.
#
THINKPAD_SCREEN = 0x4000000000
THINKPAD_MODE = (1920, 1080, 7680)          # width, height, bytes a row
MB2_BOOT_MAGIC = 0x36D76289
MB2_TAG_FRAMEBUFFER = 8


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


def damaged_copy(iso):
    """A copy of the stick `iso` with one byte of `\\boot\\kosmos.bin` changed
    - the thirteenth byte of its fourth page - and its sums as the build wrote
    them. None when it cannot be made.

    The filesystem's first sector is read out of the GPT: the first entry's
    starting LBA, at byte 32 of the entry array that begins at LBA 2.
    """
    work = scratch.directory("damaged")
    copy = os.path.join(work, "damaged.img")
    kernel = os.path.join(work, "kosmos.bin")

    try:
        shutil.copy(iso, copy)

        with open(copy, "rb") as handle:
            head = handle.read(4096)

        at = struct.unpack_from("<Q", head, 2 * 512 + 32)[0] * 512
        image = "%s@@%d" % (copy, at)

        subprocess.run(["mcopy", "-n", "-i", image, "::/boot/kosmos.bin",
                        kernel], check=True, capture_output=True)

        with open(kernel, "r+b") as handle:
            handle.seek(3 * 4096 + 12)
            byte = handle.read(1)
            handle.seek(3 * 4096 + 12)
            handle.write(bytes([byte[0] ^ 0x01]))

        subprocess.run(["mcopy", "-o", "-i", image, kernel,
                        "::/boot/kosmos.bin"], check=True, capture_output=True)
        return copy
    except (OSError, subprocess.CalledProcessError, struct.error):
        return None


def capture(iso, moments):
    """Boots once and screendumps at each moment; returns frames and serial.

    **One boot, several pictures.** The early screen and the finished
    desktop are two claims about the same machine, and booting twice to make
    them would double what this costs and still not prove they were the same
    boot.
    """
    fw, why = firmware()

    if fw is None:
        return None, why

    code, varsfd = fw
    work = scratch.directory()
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
           "-smp", "4",
           # SMBIOS 3.0, which OVMF then publishes under its own GUID in
           # the EFI Configuration Table. QEMU's default is the 2.1 entry
           # point, and a 2021 laptop's firmware is likely to hand over
           # the newer one - so this is the path the ThinkPad's name takes.
           "-machine", "smbios-entry-point-type=64"]

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


class Gdb:
    """Just enough of GDB's remote protocol to stop a guest, read and write."""

    def __init__(self, port):
        self.buf = b""
        self.s = None

        for _ in range(100):
            try:
                self.s = socket.create_connection(("127.0.0.1", port), 2)
                return
            except OSError:
                time.sleep(0.1)

    def _byte(self):
        while not self.buf:
            chunk = self.s.recv(65536)

            if not chunk:
                raise EOFError("QEMU's gdbstub closed")

            self.buf += chunk

        b, self.buf = self.buf[:1], self.buf[1:]
        return b

    def ask(self, data, timeout=30.0):
        body = data.encode()
        self.s.settimeout(timeout)
        self.s.sendall(b"$" + body + b"#%02x" % (sum(body) & 0xff))

        while self._byte() != b"+":
            pass

        while self._byte() != b"$":
            pass

        reply = b""

        while True:
            c = self._byte()

            if c == b"#":
                break

            reply += c

        self._byte()
        self._byte()
        self.s.sendall(b"+")
        return reply.decode()

    def read(self, at, n):
        return bytes.fromhex(self.ask("m%x,%x" % (at, n)))

    def write(self, at, data):
        return self.ask("M%x,%x:%s" % (at, len(data), data.hex())) == "OK"

    def run_to(self, address, timeout):
        """Continues to a hardware breakpoint there, and removes it.

        False when the guest does not get there in time - a stick the loader
        refuses never does - and then the guest is still running, breakpoint
        and all, until QEMU is killed.
        """
        placed = self.ask("Z1,%x,1" % address) == "OK"

        try:
            stop = self.ask("c", timeout=timeout) if placed else ""
        except socket.timeout:
            return False

        self.ask("z1,%x,1" % address)
        return placed and stop.startswith("T")


def pmemsave(mon, address, size, path):
    """Guest memory into a file, through the monitor; the bytes or None."""
    s = socket.socket(socket.AF_UNIX)
    s.connect(mon)
    time.sleep(0.3)
    s.recv(65536)
    s.sendall(('pmemsave %#x %d "%s"\n' % (address, size, path)).encode())
    time.sleep(1.0)
    s.close()

    for _ in range(40):
        if os.path.exists(path) and os.path.getsize(path) == size:
            with open(path, "rb") as f:
                return f.read()

        time.sleep(0.25)

    return None


def rgb(raw, width, height, pitch):
    """The kernel's 32-bit XRGB rows as the 24-bit pixels `share` counts."""
    out = bytearray()

    for y in range(height):
        row = raw[y * pitch:y * pitch + width * 4]
        line = bytearray(width * 3)
        line[0::3] = row[2::4]
        line[1::3] = row[1::4]
        line[2::3] = row[0::4]
        out += line

    return bytes(out)


def thinkpad_screen(image, elf):
    """Boots once with the screen where the ThinkPad's firmware puts it.

    **Nothing in the loader or the kernel changes for this.** QEMU is started
    paused with its gdbstub, run to the kernel's first instruction, and the
    framebuffer tag in the structure the loader built is rewritten to name
    0x4000000000 at 1920x1080. The memory there is a DIMM that is in no map
    the firmware hands over, which is what a graphics aperture is: the
    kernel draws into it, and the monitor reads the pixels back - once when
    the page allocator is about to start, which is before `hal_fb_init`
    could have mapped anything, and once at the prompt.

    Returns (serial, tagged, early, late), or (None, why, None, None).
    """
    fw, why = firmware()

    if fw is None:
        return None, why, None, None

    names = subprocess.run(["x86_64-elf-nm", elf], capture_output=True,
                           text=True).stdout
    symbols = {}

    for line in names.splitlines():
        parts = line.split()

        if len(parts) == 3:
            symbols[parts[2]] = int(parts[0], 16)

    if "_start" not in symbols or "pmm_init" not in symbols:
        return None, "no _start or pmm_init in %s" % elf, None, None

    code, varsfd = fw
    work = scratch.directory()
    mon = os.path.join(work, "mon")
    writable = os.path.join(work, "vars.fd")
    width, height, pitch = THINKPAD_MODE

    shutil.copy(varsfd, writable)

    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    cmd = [QEMU, "-M", "q35", "-m", "4G,slots=2,maxmem=300G",
           "-object", "memory-backend-ram,id=aperture,size=16M",
           "-device", "pc-dimm,memdev=aperture,addr=%#x" % THINKPAD_SCREEN,
           "-no-reboot", "-vga", "std", "-display", "none",
           "-serial", "stdio",
           "-monitor", "unix:%s,server,nowait" % mon,
           "-gdb", "tcp:127.0.0.1:%d" % port, "-S",
           "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + code,
           "-drive", "if=pflash,format=raw,unit=1,file=" + writable,
           "-device", "qemu-xhci,id=xhci",
           "-drive", "if=none,id=stick,format=raw,snapshot=on,file=" + image,
           "-device", "usb-storage,bus=xhci.0,drive=stick",
           "-smp", "4"]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    heard = bytearray()

    def drain():
        while True:
            chunk = os.read(p.stdout.fileno(), 65536)

            if not chunk:
                return

            heard.extend(chunk)

    threading.Thread(target=drain, daemon=True).start()

    tagged = False
    early = late = None

    try:
        g = Gdb(port)

        if g.s is None:
            return None, "QEMU's gdbstub did not answer", None, None

        g.ask("?")

        #
        # The structure's address is in ebx at the kernel's entry. The stub
        # sends each register as eight bytes here; four are tried as well,
        # and whichever points at something shaped like a Multiboot 2
        # structure - a sane total size, a zero reserved word - is it.
        #
        started = g.run_to(symbols["_start"], 180.0)

        if started:
            regs = g.ask("g")
            info = None

            if int.from_bytes(bytes.fromhex(regs[0:8]), "little") \
                    == MB2_BOOT_MAGIC:
                for offset in (16, 8):
                    at = int.from_bytes(bytes.fromhex(regs[offset:offset + 8]),
                                        "little")
                    total, reserved = struct.unpack("<II", g.read(at, 8))

                    if 16 <= total <= 1 << 20 and reserved == 0:
                        info = at
                        break

            if info is not None:
                blob = g.read(info, total)
                at = 8

                while at + 8 <= total:
                    kind, size = struct.unpack_from("<II", blob, at)

                    if kind == 0 or size < 8:
                        break

                    if kind == MB2_TAG_FRAMEBUFFER:
                        wanted = (THINKPAD_SCREEN, pitch, width, height)

                        g.write(info + at + 8,
                                struct.pack("<Q", THINKPAD_SCREEN))
                        g.write(info + at + 16,
                                struct.pack("<III", pitch, width, height))
                        tagged = struct.unpack(
                            "<QIII", g.read(info + at + 8, 20)) == wanted
                        break

                    at += (size + 7) & ~7

        #
        # A kernel that never started draws nothing and reaches no prompt, so
        # there is nothing to wait for: what the machine said goes back, and
        # the checks name each thing that did not happen. Until 14 September
        # the socket's timeout ended the whole run in a traceback here.
        #
        if not started:
            return heard.decode("utf-8", "replace"), False, None, None

        if tagged and g.run_to(symbols["pmm_init"], 60.0):
            raw = pmemsave(mon, THINKPAD_SCREEN, pitch * height,
                           os.path.join(work, "early.raw"))
            early = rgb(raw, width, height, pitch) if raw else None

        g.s.sendall(b"$D#44")

        until = time.time() + 90.0

        while time.time() < until and b"kosmos>" not in heard:
            time.sleep(0.5)

        time.sleep(2.0)
        raw = pmemsave(mon, THINKPAD_SCREEN, pitch * height,
                       os.path.join(work, "late.raw"))
        late = rgb(raw, width, height, pitch) if raw else None
    finally:
        p.kill()
        p.wait()

    return heard.decode("utf-8", "replace"), tagged, early, late


def home_boot(image, check):
    """**USB step 5f: `/home` on a partition of the stick the machine started
    from.**

    A stick `mkusb_image.py --home` made carries no disk for the loader to
    read. Its kfs image is a second partition, of Kosmos's type, and its
    command line names that partition's unique GUID. So nothing here is
    QEMU's but the machine: the firmware finds the stick over xHCI, the loader
    passes the stick's words on as it always has, and Kosmos's own USB driver
    has to find the partition named on that same stick for `diskinfo` to say
    it is `/home`, and for a file saved there to have extents on a disk.

    **With no screen**, which the loader allows - Kosmos starts without one -
    so the prompt stays on the serial line to be typed at. **And on a snapshot
    of the image**, so a save leaves the file as the build wrote it for
    `test_stickcheck.py`, which reads it after.
    """
    fw, why = firmware()

    if fw is None:
        check(False, "no firmware to boot the home stick with: %s" % why)
        return

    code, varsfd = fw
    work = scratch.directory()
    writable = os.path.join(work, "vars.fd")
    shutil.copy(varsfd, writable)

    # The partition the image names, read off it as the disk server reads it
    # off the stick: the header at block 1, and the entry after the ESP's.
    with open(image, "rb") as f:
        f.seek(512)
        header = f.read(92)
        f.seek(struct.unpack_from("<Q", header, 72)[0] * 512 + 128)
        second = f.read(128)

    first, last = struct.unpack_from("<QQ", second, 32)
    own = str(uuid.UUID(bytes_le=second[16:32])).upper()

    cmd = [QEMU, "-M", "q35", "-m", "4G", "-no-reboot",
           "-vga", "none", "-display", "none", "-serial", "stdio",
           "-drive", "if=pflash,format=raw,unit=0,readonly=on,file=" + code,
           "-drive", "if=pflash,format=raw,unit=1,file=" + writable,
           "-device", "qemu-xhci,id=xhci",
           "-drive", "if=none,id=stick,format=raw,snapshot=on,file=" + image,
           "-device", "usb-storage,bus=xhci.0,drive=stick"]

    typed = ('print("named:", sys.boot("opt/kosmos/home"))', "diskinfo",
             "save home.txt kept where it started")

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.PIPE)
    os.set_blocking(p.stdout.fileno(), False)

    out, sent = b"", 0
    start = quiet = time.time()

    try:
        while time.time() - start < 240.0:
            chunk = p.stdout.read()

            if chunk:
                out += chunk
                quiet = time.time()
            else:
                time.sleep(0.1)

            prompts = out.count(b"kosmos>")

            if (sent < len(typed) and prompts > sent
                    and time.time() - quiet > 0.5):
                p.stdin.write(typed[sent].encode() + b"\n")
                p.stdin.flush()
                sent += 1
                quiet = time.time()

            if sent == len(typed) and prompts > len(typed):
                time.sleep(0.8)
                out += p.stdout.read() or b""
                break
    finally:
        p.kill()
        p.wait()

    said = out.decode("utf-8", "replace").replace("\r", "")
    shown = "\n    ".join(l.strip() for l in said.splitlines()
                           if "named:" in l or "disk:" in l
                           or "Kosmos partition" in l or "filesystem:" in l
                           or "saved" in l or "the loader:" in l)

    check("the disk: none" in said
          and "the stick against the build: same" in said,
          "the home stick's boot did not say the loader handed over no disk "
          "and a kernel that was the build's:\n    " + shown)

    check(("named:\t" + own) in said,
          "the stick's command line did not name its Kosmos partition, %s, "
          "to sys.boot:\n    %s" % (own, shown))

    check(("disk: %d sectors of 512 bytes" % (last - first + 1)) in said
          and ("on the Kosmos partition on USB unit 0, blocks %d to %d"
               % (first, last)) in said,
          "/home was not the partition the stick names, blocks %d to %d:"
          "\n    %s" % (first, last, shown))

    check(re.search(r"saved home\.txt: \d+ bytes, [1-9]\d* extent", said)
          is not None,
          "a file saved to /home on the stick did not land on a disk:\n    "
          + shown)


#
# **A stick that starts the desktop by itself shows a desktop, not a boot
# screen**, and the colour checks below were written before any stick did.
# `mkusb_image.py` writes what the kernel is told into `\boot\kosmos.cmdline`
# on the ESP, and the ESP begins at the GPT's first usable sector - 34, which
# is 17408 bytes into the image - so this can read it out of the artifact
# rather than being told out of band.
#
ESP_AT = 34 * 512
DRAWN_ENOUGH = 200


def boot_args(iso):
    """What the stick tells the kernel, or "" when it tells it nothing."""
    try:
        got = subprocess.run(["mtype", "-i", "%s@@%d" % (iso, ESP_AT),
                              "::/boot/kosmos.cmdline"],
                             capture_output=True, timeout=30)
    except (OSError, subprocess.SubprocessError):
        return ""

    if got.returncode != 0:
        return ""

    return got.stdout.decode("utf-8", "replace")


def drawn(pixels):
    """How many distinct colours are on a frame.

    A firmware screen is a logo on a flat ground and counts in the dozens; a
    Kosmos desktop with its windows, text and icons counted 2347 the day this
    was written. The gap is orders of magnitude, so this separates them
    without naming a single colour - which matters, because the desktop's own
    ground is a colour the user picks and `theme.lua` says so.
    """
    seen = set()

    for i in range(0, len(pixels) - 2, 3):
        seen.add(pixels[i:i + 3])

    return len(seen)


def main():
    iso = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos-uefi.img"
    refusal = sys.argv[2] if len(sys.argv) > 2 else None
    home = sys.argv[3] if len(sys.argv) > 3 else None
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

    #
    # **A machine without OVMF is a skip, and nothing else is.** The `None`
    # a boot that started and gave no picture answers was printed as a skip
    # as well, so a gate whose screendump failed passed without one check -
    # and the skip itself had never run: `capture` answered four values
    # without the firmware, where this takes two. `tools/test_run_uefi.py`
    # asks both.
    #
    fw, why = firmware()

    if fw is None:
        print("SKIP: %s" % why)
        return 0

    frames, serial = capture(iso, (30.0,))

    if frames is None:
        print("FAIL: the first boot through Kosmos's loader gave no "
              "picture: %s" % serial)
        return 1

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
    # **And the other three started, through the path a laptop takes.** Under
    # `-kernel` the trampoline page is below a map SeaBIOS wrote; here the map
    # is the one Kosmos's loader passes on from UEFI, which is the map the
    # ThinkPad hands over - so this is the check that says a processor can be
    # started on the
    # machine the port is for, before that machine is asked.
    #
    check("3 of the others in the kernel too" in serial,
          "booted through the loader, the machine did not bring the other "
          "three processors into the kernel: "
          + next((l.strip() for l in serial.splitlines()
                  if "others" in l or "firmware" in l), "no line about them"))

    #
    # Anchored on the phrase the boot fact prints when the APIC is running,
    # not on "I/O APIC" alone: every sentence that explains a fallback names
    # the I/O APIC too - "because nothing answers at the I/O APIC's address" -
    # so the short match would pass on the very boot this exists to catch.
    #
    check("interrupts: an I/O APIC" in serial,
          "booted through the loader the machine fell back to the 8259 "
          "pair, which is what no ACPI looks like")

    #
    # **The machine's name, through the only place UEFI leaves it.**
    #
    # `neofetch` on the ThinkPad said `QEMU q35 x86-64`, which was compiled
    # in. The PC board reads SMBIOS now, and on a UEFI machine the entry
    # point is in the EFI Configuration Table rather than below 1 MB - the
    # same trap the RSDP was. Under `-kernel` SeaBIOS leaves it in the BIOS
    # area, so this boot is the only one that walks the EFI System Table,
    # and every offset in it: a wrong one finds no GUID and names nothing.
    #
    named = next((l.strip() for l in serial.splitlines() if "machine:" in l),
                 "")

    check("machine: QEMU Standard PC" in named
          and "from SMBIOS 3." in named
          and "in the EFI system table" in named,
          "booted through the loader, the machine was not named out of "
          "SMBIOS 3 in the EFI System Table: " + (named or "no machine line"))

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
    #
    # **Which screen this stick was built to be showing.** Every stick handed
    # over carries `USB_BOOT=wm`, so the desktop has replaced the boot screen
    # long before this capture, and asking such a stick for the boot log's
    # green fails on a machine that is working perfectly. It did:
    # 0.10.70-stable - which booted on the ThinkPad and played music through
    # it - failed these three while its serial line showed Tracker, the
    # Deskbar, Monitor, Log and Processes all up at 1280x800.
    #
    desktop_stick = "opt/kosmos/boot=" in boot_args(iso)

    if desktop_stick:
        colours = drawn(pixels)

        check(colours >= DRAWN_ENOUGH,
              "the stick starts the desktop and only %d colours are on the "
              "screen; a firmware screen counts in the dozens and a drawn "
              "desktop in the thousands" % colours)
    else:
        ground = share(pixels, GROUND)

        check(ground > 0.5,
              "only %.1f%% of the screen is Kosmos's ground colour; the "
              "picture is still the firmware's" % (100.0 * ground))

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

    #
    # **Kosmos's own loader, in its own words.** `boot/efi/loader.c` prints
    # through the firmware's console, which OVMF copies to the serial line.
    # The kernel's place has to be claimed or borrowed - never refused - and
    # both copies of the kernel have to match the file before it hands over.
    #
    loader = [l.strip() for l in serial.splitlines() if "kosmos-boot:" in l]

    check(any("the kernel's place: 0x01000000.." in l for l in loader),
          "the loader did not place the kernel at 16 MB: "
          + repr(loader[:4]))
    check(any("both copies of the kernel are the file, page for page" in l
              for l in loader),
          "the loader found the kernel's copies changed, or never checked: "
          + repr(loader[-3:]))
    check(any(l.startswith("kosmos-boot: handing over:") for l in loader),
          "the loader never handed over: " + repr(loader[-3:]))

    #
    # **And the kernel read off the stick is the one the build wrote.** Every
    # check above is about memory: the loader's fingerprints are of what it
    # read, so a stick handing back other bytes passed all of them, and
    # nothing the ThinkPad has shown rules that out. `mkusb_image.py` puts the
    # build's sums beside each file now, and the loader holds its read to
    # them.
    #
    check(any(l.startswith("kosmos-boot: the kernel is the build's, page for "
                           "page: ") for l in loader),
          "the loader did not say the kernel it read is the build's: "
          + repr([l for l in loader if "kernel" in l][:4]))

    #
    # **And the kernel's account of it**: what the loader repaired before and
    # after the firmware let go, written into the command line and read back
    # at boot. Nothing repaired and nothing lost is the only healthy answer
    # under emulation.
    #
    handed = next((l.strip() for l in serial.splitlines()
                   if "the loader: kosmos-boot," in l), "")

    check("0 pages repaired before the firmware let go, 0 after, 0 lost"
          in handed,
          "the kernel does not report a clean hand-over from its loader: "
          + (handed or "no loader line"))

    #
    # **And the disk, on the same terms.** The loader fingerprints it when it
    # reads it off the stick and again once the firmware has let go, and
    # leaves `same` or `diff` - or `none` when the stick carries no disk,
    # which the loader's own line says. Until this check, a disk that changed
    # in memory before Kosmos ran passed here: only the kernel's pages were
    # held to account.
    #
    carried = any(l.startswith("kosmos-boot: the disk: 0x") for l in loader)
    wanted = "the disk: same" if carried else "the disk: none"

    check((wanted + ";") in (handed + ";"),
          "the kernel does not say `%s` after the loader %s: %s"
          % (wanted, "read a disk" if carried else "found none",
             handed or "no loader line"))

    #
    # **And the disk the build wrote**, when there is one, and the kernel told
    # that everything the loader read was held to the build's sums.
    #
    if carried:
        check(any(l.startswith("kosmos-boot: the disk is the build's, page for "
                               "page: ") for l in loader),
              "the loader did not say the disk it read is the build's: "
              + repr([l for l in loader if "disk" in l][:4]))

    check(handed.endswith("; the stick against the build: same"),
          "the kernel does not say its loader held the stick to the build's "
          "sums: " + (handed or "no loader line"))

    #
    # **And the kernel off the firmware's memory**, which is what moving it to
    # 16 MB was for. Under GRUB this boot printed `UNDER THIS KERNEL` three
    # times for OVMF's ACPI NVS at 8 MB, and nobody had asked what it meant.
    #
    check("this kernel is 0x01000000.." in serial,
          "the kernel does not say it is at 16 MB: "
          + next((l.strip() for l in serial.splitlines()
                  if "this kernel is" in l), "no line"))
    check("UNDER THIS KERNEL" not in serial
          and "UNDER THE USERLAND IMAGE" not in serial,
          "memory the firmware keeps is under the kernel: "
          + next((l.strip() for l in serial.splitlines() if "UNDER" in l), "?"))

    #
    # **And the screen where the ThinkPad's firmware puts it, above 4 GB.**
    #
    # The early-screen check above passed for months while on the ThinkPad
    # the early screen had never once worked: its framebuffer is at
    # 0x4000000000, `hal_fb_early` refused anything past the four gigabytes
    # `start.S` maps, and every boot of that machine was dark from the
    # loader's last line to stage six. A stall anywhere in between was a
    # photograph of the loader's lines and nothing else, which is what two
    # sticks on 13 September were.
    #
    # So the same stick is booted with its screen moved there (see
    # `thinkpad_screen`), and the pixels are read **when the page allocator
    # is about to start**, at stage four - before `hal_fb_init` could have
    # mapped anything the late way. The ground and the log's green have to be
    # in them.
    #
    elf = os.path.join(os.path.dirname(iso) or ".", "kosmos.elf")

    if not os.path.exists(elf):
        fails.append("no %s beside the stick, so the ThinkPad's screen was "
                     "not tried" % elf)
    else:
        hserial, tagged, early, late = thinkpad_screen(iso, elf)

        if hserial is None:
            fails.append("the ThinkPad's screen could not be tried: %s"
                         % tagged)
        else:
            early_ground = share(early, GROUND) if early else 0.0
            early_green = share(early, GREEN) if early else 0.0
            said = next((l.strip() for l in hserial.splitlines()
                         if "attached here" in l or "since stage" in l),
                        "nothing about when the panel got the log")

            check(tagged,
                  "the harness did not stop at the kernel's entry and move "
                  "the loader's framebuffer to 0x4000000000")
            check(early_ground > 0.5 and early_green > 0.0005,
                  "with the screen at 0x4000000000 nothing was drawn by the "
                  "start of stage four (%s): the kernel is dark there until "
                  "stage six, as it was on the ThinkPad"
                  % ("no pixels read" if early is None else
                     "%.1f%% ground, %.2f%% green"
                     % (100.0 * early_ground, 100.0 * early_green)))
            check("the panel has had this log since stage two" in hserial,
                  "with the screen at 0x4000000000 the kernel says: " + said)
            check(("%dx%d, 32-bit XRGB" % THINKPAD_MODE[:2]) in hserial,
                  "the kernel did not take the 1920x1080 screen at "
                  "0x4000000000")
            if desktop_stick:
                check(late is not None and drawn(late) >= DRAWN_ENOUGH,
                      "with the screen at 0x4000000000 the desktop is not "
                      "drawn at the prompt")
            else:
                check(late is not None
                      and share(late, GREEN) > 0.001
                      and share(late, RED) > 0.0005,
                      "with the screen at 0x4000000000 the boot log and the "
                      "wordmark are not on it at the prompt")

    #
    # **A refusal, on the screen.** The ThinkPad's first boot through this
    # loader was a black panel that went back to the firmware's menu when a
    # key was pressed: the loader had refused and said why, through a text
    # console that machine's firmware did not show, and every check above
    # passed because they all read the serial line. So a stick whose kernel
    # is zeros is booted as well, and the refusal must be said and must be
    # drawn: the ground and the ink of the loader's own lines in the lower
    # half, where it draws them and where OVMF's console, with a refusal's
    # few lines, does not reach.
    #
    if refusal is not None:
        rframes, rserial = capture(refusal, (REFUSAL_AT,))

        if rframes is None:
            fails.append("the refusal stick gave no picture: %s" % rserial)
        else:
            rwidth, rheight, rpixels = rframes[0]
            lower = rpixels[(rheight // 2) * rwidth * 3:]
            ground_low = share(lower, GROUND)
            ink_low = share(lower, LOADER_INK)

            check("Press a key to return to the firmware" in rserial,
                  "a stick whose kernel is zeros was not refused on the serial "
                  "line: " + next((l.strip() for l in rserial.splitlines()
                                   if "kosmos-boot:" in l), "no loader line"))
            check(ground_low > 0.05 and ink_low > 0.005,
                  "the loader's refusal is not drawn in the lower half of the "
                  "screen: %.1f%% ground, %.2f%% ink"
                  % (100.0 * ground_low, 100.0 * ink_low))

    #
    # **A stick that does not hold what the build wrote is refused, with the
    # page.** The same image with one byte of `kosmos.bin` changed on it, and
    # the sums left as the build wrote them: what a stick returning wrong
    # bytes looks like to the loader. Made here from the image being tested,
    # through the partition table's own word for where the filesystem starts.
    #
    damaged = damaged_copy(iso)

    if damaged is None:
        fails.append("could not make a copy of the stick with one byte of its "
                     "kernel changed")
    else:
        dframes, dserial = capture(damaged, (REFUSAL_AT,))
        dloader = [l.strip() for l in (dserial or "").splitlines()
                   if "kosmos-boot:" in l]

        check("Press a key to return to the firmware" in (dserial or "")
              and "Welcome to Kosmos" not in (dserial or ""),
              "a stick with one byte of its kernel changed was not refused: "
              + repr(dloader[-4:]))
        check(any(l.startswith("kosmos-boot: the kernel: 1 of its ")
                  and "pages is not the build's, the first page 3, at byte "
                      "0x00003000" in l for l in dloader),
              "the refusal of a changed kernel did not name the one page, the "
              "fourth: " + repr(dloader[-4:]))

    if home is not None:
        home_boot(home, check)

    if fails:
        print("FAIL: %d of %d checks booting through Kosmos's loader under "
              "UEFI:"
              % (len(fails), len(fails) + checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks booting through Kosmos's loader under UEFI (the "
          "firmware's memory claimed and checked, the kernel at 16 MB, "
          "Kosmos drawing its own %dx%d screen, and the ThinkPad's 1920x1080 "
          "at 0x4000000000 from stage two%s)."
          % (checks, width, height,
             "; and /home on a partition of the stick it started from"
             if home is not None else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
