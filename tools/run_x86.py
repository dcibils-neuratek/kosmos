#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Boots Kosmos on x86-64 and checks that it is the same system.

This replaced a harness that read a staging `kmain`'s printed proofs. That
one earned its keep while the port was being built a piece at a time - the
page tables, an address space, a context switch, a process at ring 3 - and
every one of those is now underneath a machine that boots to a shell, so
the questions worth asking are the ones you can ask a running system.

**It is not a translation of `run_headless.py`.** That one exists because a
machine with nothing plugged into it once booted to a prompt that never
appeared; this one exists because a second architecture can be wrong in ways
the first never was, and the checks are chosen for that: the same numbers
arrived at through the kernel's boot log and through userland's servers, an
answer typed at the prompt, and a program that runs and reports what it was
handed.

Typing is a check rather than a convenience. The machine reached a prompt
and ignored everything typed at it for a while, because `hlt` with
interrupts masked halts for ever where AArch64's `wfi` wakes - and a boot
test that only read the boot log would have called that a pass.
"""

import os
import re
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

QEMU = "qemu-system-x86_64"

# `make x86`'s own line. What a person runs, not a shape invented for a test.
ARGS = [
    "-M", "q35",
    "-m", "512M",
    "-nographic",
    "-no-reboot",
]

PAGE_SIZE = 4096


def boot(image, option, timeout, typed=(), extra=()):
    """Boots, optionally types at the prompt, and returns everything printed.

    One line per prompt, and only after the machine has been quiet for a
    moment: a line written into the middle of the boot log is a line the
    console server has not been asked for yet.
    """
    binary = os.path.join(os.path.dirname(image), "kosmos.bin")

    if not os.path.exists(binary):
        print("FAIL: no %s beside the ELF. Run `make x86-build`." % binary)
        return None

    cmd = [QEMU] + ARGS + list(extra)

    if option:
        # The same flag the ARM board takes, now that this one answers out
        # of fw_cfg too. It replaced `-append`, which could not carry a
        # value with a space in it - `boot=ls /bin` gave the machine `ls`.
        cmd += ["-fw_cfg", "name=opt/kosmos/boot,string=" + option]

    cmd += ["-kernel", binary]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         stdin=subprocess.PIPE)
    os.set_blocking(p.stdout.fileno(), False)

    out = b""
    start = time.time()
    quiet = time.time()
    sent = 0

    try:
        while time.time() - start < timeout:
            chunk = p.stdout.read()

            if chunk:
                out += chunk
                quiet = time.time()
            else:
                time.sleep(0.05)

            prompts = out.count(b"kosmos>")

            if (sent < len(typed) and prompts > sent
                    and time.time() - quiet > 0.4):
                p.stdin.write(typed[sent].encode() + b"\n")
                p.stdin.flush()
                sent += 1
                quiet = time.time()

            done = (prompts > len(typed)) if typed else (prompts > 0)

            if sent == len(typed) and done:
                # A moment more, so a line arriving just after the last
                # prompt is in the output rather than cut off.
                time.sleep(0.8)
                out += p.stdout.read() or b""
                break
    finally:
        p.kill()
        p.wait()

    return out.decode("utf-8", "replace")


#
# **The sound the machine made, read back off the wire.**
#
# Everything else in this file is a string the machine printed, which is the
# machine's own account of itself. Audio is the one subsystem where that is
# not enough: a driver that programs the controller wrongly prints exactly
# what a working one prints, and the whole of the difference is in bytes
# nobody in the guest ever sees again.
#
# So QEMU's `wav` backend writes what it was handed to a file, and this
# reads it. 440 Hz for a third of a second, and silence on either side -
# which is three separate claims about the driver:
#
#   - the samples arrived at all, and in the right format, since a wrong
#     sample rate or channel count reads as the wrong pitch rather than as
#     an error;
#   - they arrived *once*, so the cyclic buffer is not repeating a period
#     it has already played, which is what an HDA ring does when nobody
#     zeroes a period behind the read pointer;
#   - and the silence before and after is real silence, which is the same
#     claim from the other side.
#
# Killing QEMU leaves the header's length fields stale - it writes those on
# a clean exit - so the samples are read from the fixed 44-byte offset
# rather than from the header. Everything that matters is in the data.
#
WAV_HEADER = 44
TONE_HZ = 440
TONE_MS = 333


def tone(path):
    """(milliseconds, hertz, stray) of the loudest run in a captured WAV."""
    with open(path, "rb") as f:
        data = f.read()[WAV_HEADER:]

    frames = len(data) // 4
    left = [struct.unpack_from("<h", data, i * 4)[0] for i in range(frames)]

    live = [i for i, v in enumerate(left) if v != 0]

    if not live:
        return 0, 0, 0

    first, last = live[0], live[-1]
    segment = left[first:last + 1]

    crossings = sum(1 for i in range(1, len(segment))
                    if (segment[i - 1] < 0) != (segment[i] < 0))

    seconds = len(segment) / 44100.0

    # Samples outside the run: silence that is not silent, which is a period
    # played twice or a buffer that was never cleared.
    stray = len(live) - sum(1 for v in segment if v != 0)

    return int(seconds * 1000), int(crossings / 2 / seconds), stray


def storage(image, check):
    """Boots with an NVMe drive, writes a file, reboots, and reads it back.

    **The only disk on this QEMU line is the NVMe one**, which is what makes
    this a test of `hal/pc/nvme.c` rather than of the filesystem: there is no
    virtio-blk to fall back to, so a file that comes back after a reboot came
    back through an admin queue, an identify, a created I/O queue, a write
    command and a read command, and every field offset in all of them.

    `nvme.c` says in as many words that a driver which merely *initialises*
    proves almost nothing, and that the field offsets are the part to
    distrust because they were written from knowledge of the specification
    rather than from a copy of it. This is the test it names: bytes in, a
    reboot, and the same bytes out. A wrong offset anywhere on that path
    cannot produce them.

    The reboot is a kill rather than a shutdown, deliberately - it is what
    `run_disk.py` does on the other board, and the journal is supposed to
    survive exactly that.
    """
    disk = os.path.join(tempfile.gettempdir(), "kosmos-x86-nvme.img")

    with open(disk, "wb") as handle:
        handle.truncate(64 * 1024 * 1024)

    extra = ("-drive", "file=%s,format=raw,if=none,id=nvme0" % disk,
             "-device", "nvme,drive=nvme0,serial=kosmos")

    first = boot(image, None, 90.0, extra=extra, typed=(
        "diskinfo",
        "mkfs --yes",
        "save notes.txt written before the reboot",
    ))

    if first is None:
        check(False, "the machine would not boot with an NVMe drive")
        return

    check("sectors" in first,
          "`diskinfo` said nothing about sectors, so the NVMe namespace was "
          "never identified: "
          + next((l.strip() for l in first.splitlines() if "disk" in l),
                 "nothing was said about a disk at all"))

    check("Formatted" in first,
          "`mkfs` did not report formatting, so writing to the drive failed")

    check("saved notes.txt" in first,
          "`save` did not report writing the file")

    second = boot(image, None, 90.0, extra=extra,
                  typed=("cat /home/notes.txt",))

    if second is None:
        check(False, "the machine would not boot the second time")
        return

    check("written before the reboot" in second,
          "the file written over NVMe was not there after a reboot, so the "
          "writes never reached the drive - which a driver that formats and "
          "reads back its own cache looks exactly like")


def sound(image, check):
    """Boots with a real HDA controller, plays a tone, and listens."""
    wav = os.path.join(tempfile.gettempdir(), "kosmos-x86-hda.wav")

    if os.path.exists(wav):
        os.remove(wav)

    out = boot(image, "beep", 90.0, extra=(
        "-device", "ich9-intel-hda",
        "-device", "hda-output,audiodev=a0",
        "-audiodev", "wav,id=a0,path=" + wav,
    ))

    if out is None:
        check(False, "the machine would not boot with an HDA controller")
        return

    check("sound: Intel HDA" in out,
          "an ich9-intel-hda was on the bus and the driver did not take it: "
          + next((l.strip() for l in out.splitlines() if "sound" in l),
                 "nothing was said about sound at all"))

    check(re.search(r"beep: %d Hz, \d+ ms of sound" % TONE_HZ, out) is not None,
          "`beep` did not report that it had played anything")

    if not os.path.exists(wav):
        check(False, "nothing was written to the capture file at all")
        return

    ms, hz, stray = tone(wav)
    os.remove(wav)

    check(abs(hz - TONE_HZ) <= 4,
          "the captured tone is %d Hz and `beep` played %d" % (hz, TONE_HZ))
    check(abs(ms - TONE_MS) <= 25,
          "the captured tone is %d ms and `beep` played %d" % (ms, TONE_MS))
    check(stray == 0,
          "%d samples outside the tone are not silent, which is a period "
          "the ring played more than once" % stray)

    #
    # **And that the controller is raising its line**, which the capture
    # above cannot tell you.
    #
    # A cyclic buffer plays whether or not anybody is listening for the
    # interrupt, so a driver whose handler never runs produces a tone that
    # sounds correct - the ring is refilled by whoever polls the read
    # pointer. What it does *not* produce is a period retired on time, and
    # what nothing produces is the zeroing that keeps an underrun silent.
    #
    # `audiolag` asks the machine directly: one line raised per period, and
    # the queue floor above zero rather than the four-out-of-four a stream
    # that never filled would report.
    #
    lag = boot(image, "audiolag", 120.0, extra=(
        "-device", "ich9-intel-hda",
        "-device", "hda-output,audiodev=a0",
        "-audiodev", "none,id=a0",
    ))

    if lag is None:
        check(False, "the machine would not run `audiolag` with HDA")
        return

    raised = re.search(r"raised its line (\d+) times for (\d+) periods", lag)

    check(raised is not None, "`audiolag` did not report the interrupt count")

    if raised:
        lines, periods = int(raised.group(1)), int(raised.group(2))

        #
        # **About one per period, not exactly one.** The count is read
        # after the last period is queued and before the device has
        # necessarily finished with it, so the boundary moves by one or two
        # between runs - 401 and 399 have both been seen for 400 periods.
        # An exact comparison failed on the second of those and said the
        # handler was not running, which was not true of anything.
        #
        # The tolerance is wide enough for the boundary and nowhere near
        # wide enough to miss what this is for: virtio-sound raises 194
        # times for the same 400 periods, because that device services two
        # periods per interrupt. Half is the failure; a couple either way
        # is the measurement.
        #
        check(lines >= periods * 9 // 10,
              "the controller raised its line %d times for %d periods; the "
              "handler is not running once per period" % (lines, periods))

    check("UNDERRUNS 0" in lag,
          "the HDA ring underran: "
          + next((l.strip() for l in lag.splitlines() if "UNDERRUNS" in l),
                 "and said nothing about it"))

    floor = re.search(r"queue floor (\d+) of (\d+) periods", lag)

    check(floor is not None and 0 < int(floor.group(1)) <= int(floor.group(2)),
          "the queue floor is not a number between one and the device depth, "
          "which means the depth this driver reports is not the HAL's")


#
# **The pointer a laptop has, and the serial port it does not.**
#
# Every other boot in this file has a COM1, because QEMU gives a machine one
# unless told not to, and either a virtio tablet or no pointer at all. The
# first real machine had neither: its pointer is a TrackPoint on the i8042's
# auxiliary port, and it has no serial port - which mattered more than
# anything about the pointer. An I/O port nothing answers reads 0xFF, and
# 0xFF in a 16550's line status register says a byte has arrived. The
# console read a phantom byte for ever, never slept, and posted keys that
# pushed every mouse release out of the focused window's queue: a core at
# 100% and buttons that went down and never came up, on a machine that
# booted and typed perfectly.
#
# So two boots, both straight into the desktop on the 8259 the laptop uses
# and with QEMU's PS/2 mouse as the only pointer:
#
#   - with COM1, where the log can be read: the auxiliary port is bound, and
#     a click on the Deskbar's button - driven through the monitor as
#     relative counts, which is what a TrackPoint sends - opens its menu;
#   - without COM1, where nothing can be read: the machine halts between
#     events, sampled from QEMU's own `HLT=` rather than from anything the
#     guest says about itself, and the same click opens the same menu,
#     found in a screendump.
#
# **What each check is known to catch, because both were run against the
# build before the fix.** The idle check failed there - HLT=1 in 0 of 40
# samples - with either queue policy in `wm.lua`. The menu check did not:
# with the phantom port put back the click still opened the menu here,
# because whether a release is lost depends on where it lands in a queue the
# flood is filling, and this harness's timing does not land it there on
# demand. So the menu check is the end-to-end claim that a click works on a
# machine with no serial port, and the idle check is the test of the fault.
#
MOUSE_SPEED = 32        # `pointer_scale` in hal/pc/i8042.c: units per count
POINTER_RANGE = 32767   # the range it reports, which the wm maps to pixels


class Monitor:
    """QEMU's human monitor over a socket: a command in, whatever it said."""

    def __init__(self, path):
        for _ in range(400):
            self.sock = socket.socket(socket.AF_UNIX)

            try:
                self.sock.connect(path)
                break
            except OSError:
                self.sock.close()
                time.sleep(0.05)
        else:
            raise RuntimeError("QEMU's monitor never opened at " + path)

        self.sock.settimeout(0.05)
        self.ask("")

    def ask(self, line, quiet=0.15):
        """Sends a line and returns what came back before it went quiet."""
        self.sock.sendall((line + "\n").encode())
        out, heard = b"", time.time()

        while time.time() - heard < quiet:
            try:
                chunk = self.sock.recv(65536)
            except socket.timeout:
                continue

            if not chunk:
                break

            out += chunk
            heard = time.time()

        return out.decode("utf-8", "replace")

    def screendump(self, path):
        """Width, height and RGB bytes of the screen, or None."""
        self.ask("screendump " + path, quiet=0.5)

        for _ in range(100):
            if os.path.exists(path):
                picture = parse_ppm(open(path, "rb").read())

                if picture is not None:
                    return picture

            time.sleep(0.1)

        return None

    def close(self):
        self.sock.close()


def parse_ppm(data):
    """Width, height and pixels of a binary PPM, or None if it is short."""
    fields, at = [], 0

    while len(fields) < 4:
        while at < len(data) and data[at:at + 1].isspace():
            at += 1

        start = at

        while at < len(data) and not data[at:at + 1].isspace():
            at += 1

        if start == at:
            return None

        fields.append(data[start:at])

    width, height = int(fields[1]), int(fields[2])
    pixels = data[at + 1:]

    if fields[0] != b"P6" or len(pixels) < width * height * 3:
        return None

    return width, height, pixels


def click(monitor, x, y, width, height):
    """Moves the pointer to a screen pixel in relative counts, and clicks."""

    # To the corner first, so where it starts is known rather than assumed:
    # the driver clamps at its range, so enough movement up and to the left
    # is exactly the origin however far the pointer had wandered.
    for _ in range(24):
        monitor.ask("mouse_move -100 -100", quiet=0.03)

    # The window manager maps the driver's range onto the screen and the
    # driver multiplies each count by its speed, so this undoes both. Down
    # is positive here; QEMU turns it into PS/2's convention, where up is.
    need_x = -(-(x * POINTER_RANGE // (width - 1) + 1) // MOUSE_SPEED)
    need_y = -(-(y * POINTER_RANGE // (height - 1) + 1) // MOUSE_SPEED)

    while need_x > 0 or need_y > 0:
        step_x, step_y = min(need_x, 60), min(need_y, 60)
        monitor.ask("mouse_move %d %d" % (step_x, step_y), quiet=0.03)
        need_x, need_y = need_x - step_x, need_y - step_y

    time.sleep(1.5)
    monitor.ask("mouse_button 1", quiet=0.03)
    time.sleep(0.25)
    monitor.ask("mouse_button 0", quiet=0.03)


def pointer(image, check):
    """Clicks the Deskbar's button through a PS/2 mouse, with and without COM1."""
    binary = os.path.join(os.path.dirname(image), "kosmos.bin")
    work = tempfile.mkdtemp(prefix="kosmos-x86-pointer-")

    def start(serial):
        path = os.path.join(work, "monitor-" + serial)
        cmd = [QEMU, "-M", "q35,vmport=off", "-m", "512M", "-no-reboot",
               "-display", "none", "-vga", "none", "-device", "ramfb",
               "-monitor", "unix:%s,server,nowait" % path,
               "-serial", serial,
               "-fw_cfg", "name=opt/kosmos/boot,string=wm",
               "-fw_cfg", "name=opt/kosmos/irq,string=pic",
               "-kernel", binary]

        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL)
        heard = bytearray()

        # On a thread, because a guest whose serial line is not read stops
        # inside `kputc` once the pipe is full - the lesson the display
        # harness learned first.
        def drain():
            while True:
                chunk = os.read(proc.stdout.fileno(), 65536)

                if not chunk:
                    return

                heard.extend(chunk)

        threading.Thread(target=drain, daemon=True).start()
        return proc, Monitor(path), heard

    def said(heard):
        return heard.decode("utf-8", "replace")

    def wait_for(heard, pattern, seconds):
        until = time.time() + seconds

        while time.time() < until:
            found = re.search(pattern, said(heard))

            if found:
                return tuple(int(v) for v in found.groups())

            time.sleep(0.25)

        return None

    # 1. With COM1, where the machine can say what happened.
    proc, monitor, heard = start("stdio")
    began = time.time()

    try:
        deskbar = wait_for(heard, r"wm: window Deskbar at (\d+),(\d+) (\d+)x(\d+)",
                           120.0)
        to_desktop = time.time() - began

        check("pointer: the i8042 auxiliary port" in said(heard),
              "with no tablet on the machine the board did not bind the "
              "i8042's auxiliary port, so QEMU's PS/2 mouse reaches nothing")

        check(deskbar is not None,
              "booted into the desktop, the Deskbar never opened a window")

        if deskbar is None:
            return

        # The rest of the login windows placed first, so none of them lands
        # on the button after the pointer has got there.
        time.sleep(6.0)

        screen = monitor.screendump(os.path.join(work, "geometry.ppm"))

        check(screen is not None, "QEMU would not screendump the desktop")

        if screen is None:
            return

        width, height = screen[0], screen[1]
        at_x, at_y = deskbar[0] + deskbar[2] // 2, deskbar[1] + 24

        click(monitor, at_x, at_y, width, height)

        menu = wait_for(heard, r"wm: menu of Deskbar at (\d+),(\d+) (\d+)x(\d+)",
                        20.0)

        check(menu is not None,
              "a click on the Deskbar's button through QEMU's PS/2 mouse "
              "opened no menu; the driver and the window manager said: "
              + " | ".join(l.strip() for l in said(heard).splitlines()
                           if "i8042" in l or "wm: button" in l
                           or "not collecting" in l)[-400:])
    finally:
        monitor.close()
        proc.kill()
        proc.wait()

    if menu is None:
        return

    # 2. Without COM1, which is the machine the fault was on.
    proc, monitor, _ = start("none")

    try:
        # Nothing to wait on, so as long again as the first boot took and
        # then some. A machine slower than that fails the menu check below,
        # which says so.
        time.sleep(max(30.0, 2.5 * to_desktop + 10.0))

        halted = 0

        for _ in range(40):
            if "HLT=1" in monitor.ask("info registers"):
                halted += 1

            time.sleep(0.1)

        check(halted >= 10,
              "with no serial port the machine never idles - HLT=1 in %d of "
              "40 samples, where the same desktop with one halts in most - so "
              "something is reading a device that is not there as input"
              % halted)

        mx, my, mw, mh = menu
        before = monitor.screendump(os.path.join(work, "before.ppm"))

        click(monitor, at_x, at_y, width, height)
        time.sleep(5.0)

        after = monitor.screendump(os.path.join(work, "after.ppm"))
        changed = 0

        if before and after:
            for y in range(my, min(my + mh, height)):
                for x in range(mx, min(mx + mw, width)):
                    i = (y * width + x) * 3

                    if before[2][i:i + 3] != after[2][i:i + 3]:
                        changed += 1

        check(before is not None and after is not None,
              "QEMU would not screendump the desktop with no serial port")

        check(changed * 3 > mw * mh,
              "with no serial port a click on the Deskbar's button opened no "
              "menu: %d of the %d pixels where it opens changed"
              % (changed, mw * mh))
    finally:
        monitor.close()
        proc.kill()
        proc.wait()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    checks = 0
    fails = []

    def check(ok, complaint):
        nonlocal checks

        if ok:
            checks += 1
        else:
            fails.append(complaint)

    # 1. It boots, all the way, and takes what is typed at it.
    out = boot(image, None, 90.0, typed=("mem", "cpu"))

    if out is None:
        return 1

    if "kosmos>" not in out:
        print("FAIL: x86-64 never reached a prompt.")

        for line in out.splitlines():
            if ("PANIC" in line or "could not start" in line
                    or "process died" in line):
                print("  the machine said: " + line.strip())

        print("  last of what it did say: " + repr(out[-400:]))
        return 1

    checks += 1

    if "PANIC" in out:
        print("FAIL: it panicked: "
              + [l for l in out.splitlines() if "PANIC" in l][0].strip())
        return 1

    # 2. Nothing refused to start on the way there. A prompt can appear with
    #    a server missing, and a shell talking to servers that are not there
    #    is not a working machine.
    for line in out.splitlines():
        if "could not start" in line:
            print("FAIL: reached a prompt, but: " + line.strip())
            return 1

    checks += 1

    # 3. All twelve stages. The kernel prints one per subsystem it brings
    #    up, so a missing number is a subsystem that did not.
    for stage in range(1, 13):
        check("[%d/12]" % stage in out, "boot stage %d/12 is missing" % stage)

    # 4. The processor named itself out of CPUID rather than out of a
    #    constant in the image.
    check(re.search(r"x86-64 f\d+m\d+s\d+\s+\(CPUID\.1:EAX 0x[0-9a-f]+\)",
                    out) is not None,
          "the boot log does not name an x86-64 processor from CPUID")

    # 5. **The same memory, counted twice.** The boot log is the kernel's
    #    own; `mem` at the prompt is userland asking a server, which asks
    #    the kernel through a syscall. Two paths through the whole system to
    #    one number - and the megabytes are computed here rather than
    #    compared against a word that was printed.
    # **Anchored so that the two cannot be the same line.**
    #
    # The kernel's boot fact and userland's `mem` now print the same three
    # numbers in the same words, because the base was added to the boot log
    # where it was already in `mem` - and at that moment a search for the
    # shorter pattern found the longer line and this check compared a line
    # with itself. The boot fact carries `-> `; `mem` starts at the margin.
    kern = re.search(r"-> (\d+) MB of RAM at 0x([0-9a-f]+), in (\d+) pages",
                     out)
    user = re.search(r"^(\d+) MB of RAM at 0x([0-9a-f]+), in (\d+) pages",
                     out, re.MULTILINE)

    check(kern is not None, "the boot log did not report the memory")
    check(user is not None, "`mem` at the prompt printed nothing usable")

    if kern and user:
        kmb, kn = int(kern.group(1)), int(kern.group(3))
        umb, un = int(user.group(1)), int(user.group(3))

        # And where it starts, which is the third number and the one a PC
        # can get wrong on its own: `virt` has RAM at a constant and a PC
        # has it wherever the firmware left room.
        check(int(kern.group(2), 16) == int(user.group(2), 16),
              "the kernel says RAM begins at 0x%s and userland was told 0x%s"
              % (kern.group(2), user.group(2)))

        check(kn == un,
              "the kernel manages %d pages and userland was told %d" % (kn, un))
        check(kmb == umb, "the two paths disagree about the size in MB")
        check(kn * PAGE_SIZE // (1024 * 1024) == kmb,
              "%d pages of %d bytes is not %d MB" % (kn, PAGE_SIZE, kmb))

    # 6. And that typing reached the shell at all, which is the check the
    #    idle loop failed silently.
    check("architecture  x86-64" in out,
          "`cpu` at the prompt did not answer, or did not say x86-64")

    # 7. A program, started from the command line the loader passed - which
    #    also exercises `hal_boot_option`, a different mechanism from ARM's
    #    fw_cfg answering to the same name.
    ran = boot(image, "hello", 90.0)

    if ran is None:
        return 1

    check("Hello from a process of my own." in ran,
          "the fw_cfg boot option did not run a program")

    # What it prints is its own capability list, asked of the namespace - so
    # this is IPC and the servers rather than a string in the image.
    for path in ("/bin", "/dev", "/home", "/lib"):
        check(path in ran, "a process could not see %s" % path)

    check("process died" not in ran, "the program faulted on its way out")

    if fails:
        print("FAIL: %d of %d checks on x86-64:"
              % (len(fails), len(fails) + checks))

        for f in fails:
            print("  " + f)

        return 1

    #
    # **The processor count comes out of the firmware's tables.**
    #
    # This board answered 1 for as long as it existed, and the comment in
    # `hal/pc/cpus.c` was right to: the count is in the ACPI MADT, one entry
    # per local APIC, and nothing parsed it. AArch64 gets it from PSCI,
    # which will say whether a processor exists without starting it; x86 has
    # no equivalent and has to read a table.
    #
    # Four, asked for on the command line, so what is checked is that the
    # machine found what QEMU was told to give it - not that it printed a
    # number. A hardcoded 1 would fail this, and so would a count that came
    # from CPUID's core count, which is a different question with a
    # different answer on a hybrid processor.
    #
    smp = boot(image, None, 90.0, extra=("-smp", "4"))

    check(smp is not None and "-> 4 processors" in smp,
          "the machine did not find the four processors it was given; "
          "the MADT is the only place that number is")

    #
    # **And the other three are running kernel code**, which for as long as
    # this board existed they were not: nothing could send INIT and STARTUP,
    # and there was no page under 1 MB for a core to land on. Each climbs out
    # of real mode on its own, claims its per-CPU block through GS, loads its
    # own TSS and ticks on its own local APIC timer.
    #
    check(smp is not None and "3 of the others in the kernel too" in smp,
          "the machine found four processors and did not bring the other "
          "three into the kernel: "
          + next((l.strip() for l in (smp or "").splitlines()
                  if "others" in l or "firmware" in l), "no line about them"))

    #
    # **And each of them said so, from the board's side.** `cpu_on.c` reads
    # back a word every processor writes into the trampoline page as it
    # climbs, and prints one line per processor whatever became of it: why
    # it was refused, the last stage it reached, or how long reaching the
    # kernel took. The first real machine this ran on started none of its
    # cores and said nothing about why, and on a machine with no serial port
    # these lines are all there is to go on - this keeps them printing.
    #
    started = [l.strip() for l in (smp or "").splitlines()
               if "cpu_on: processor" in l]

    check(len(started) == 3
          and all("reached the kernel" in l for l in started),
          "the board should print one line per processor it started, each "
          "saying it reached the kernel; it printed: "
          + ("; ".join(started) or "nothing"))

    # And all four are given new threads, with nobody asking: a plain boot
    # spreads work across every processor that arrived. This said "1" until
    # 0.10.22, when that was the default rather than a limit.
    check(smp is not None and "4 of them given new threads" in smp,
          "a plain boot should give every processor that arrived new "
          "threads; it said: "
          + next((l.strip() for l in (smp or "").splitlines()
                  if "given new threads" in l), "no placement line"))

    #
    # **And the boot option from the loader's command line**, which is how a
    # machine with no fw_cfg - the ThinkPad, booted by GRUB - is given one at
    # all. `-append` fills Multiboot's command line under QEMU, the field
    # GRUB's `multiboot2` line fills on the laptop, and no fw_cfg is passed.
    #
    # Two rather than four, because four is what a plain boot says now and an
    # option that never arrived would pass. The option narrows.
    #
    narrowed = boot(image, None, 90.0,
                    extra=("-smp", "4", "-append", "opt/kosmos/smp=2"))

    check(narrowed is not None
          and "2 of them given new threads; opt/kosmos/smp asked for fewer"
          in narrowed,
          "opt/kosmos/smp=2 on the command line did not reach the kernel: "
          + next((l.strip() for l in (narrowed or "").splitlines()
                  if "given new threads" in l), "no placement line"))

    #
    # **A machine with a real amount of memory in it.**
    #
    # Every x86 boot in this file until now asked for 512 MB, which is the
    # one size where a PC's memory looks simple: one usable block, the
    # kernel inside it, and nothing above the four-gigabyte line. No machine
    # built this decade has that shape, and the port would have met the real
    # one for the first time on a laptop with no serial port - which is a
    # black screen and nothing to ask.
    #
    # Two faults, both found here and neither a driver:
    #
    #   - the boot page tables mapped one gigabyte, and a multiboot loader
    #     may leave its information structure anywhere below four. QEMU put
    #     it at 0x7ffe2349 and reading it faulted at boot stage three;
    #   - the largest usable region on a machine with a PCI hole is the one
    #     *above* four gigabytes, so the board chose the block the kernel is
    #     not loaded into. `pmm_init: the kernel image does not fit in RAM`.
    #
    # Sixteen gigabytes rather than four, because that is a ThinkPad and
    # because both faults get worse rather than better with size.
    #
    big = boot(image, None, 120.0, extra=("-m", "16G"))

    if big is None:
        check(False, "the machine would not boot with 16 GB of memory")
    else:
        check("kosmos>" in big,
              "16 GB of memory did not reach a prompt: "
              + next((l.strip() for l in big.splitlines()
                      if "PANIC" in l or "fault" in l),
                     "and said nothing about why"))

        # It says what it gave up. A kernel that identity maps RAM below the
        # process region cannot describe a laptop's memory, and a number
        # that is quietly five per cent of the truth has to be printed
        # rather than discovered.
        told = re.search(r"of (\d+) MB this machine has", big)

        check(told is not None and int(told.group(1)) > 16000,
              "the machine did not report the memory it cannot map; the cap "
              "is silent, which is how it would be found on hardware")

        used = re.search(r"(\d+) MB of RAM at 0x[0-9a-f]+, in \d+ pages", big)

        check(used is not None and 700 < int(used.group(1)) < 768,
              "the usable memory is not the region below the device window, "
              "so the board chose a block on the far side of the PCI hole")

    #
    # **Both interrupt controllers, because this machine has two and a
    # laptop may have one.**
    #
    # Intel has been removing the legacy 8259 pair and the 8253 from
    # UEFI-only platforms, and a kernel that hard-wired them gets no
    # scheduler tick there - a boot that prints all twelve stages and then
    # stops, with nothing else visibly wrong. So the controller is chosen at
    # run time from what the firmware described, and `opt/kosmos/irq=pic`
    # forces the other one.
    #
    # Checked because a fallback nobody runs is a fallback that rots, and
    # the machine it would fail on is the one with least to debug with. q35
    # has both, so both are exercised on every gate.
    #
    legacy = boot(image, None, 90.0,
                  extra=("-fw_cfg", "name=opt/kosmos/irq,string=pic"))

    check(legacy is not None and "kosmos>" in legacy,
          "forced onto the 8259 pair, the machine did not reach a prompt")

    if legacy:
        check("8259" in legacy,
              "the machine was forced onto the legacy pair and did not say "
              "so; the boot log is the only instrument a laptop has")

        check("PANIC" not in legacy,
              "the legacy interrupt path panicked: "
              + next((l.strip() for l in legacy.splitlines() if "PANIC" in l),
                     "?"))

    # And that the default took the other one, so the two checks above are
    # about a path this machine is not otherwise using.
    #
    # Anchored on the phrase the boot fact prints when the APIC is running,
    # not on "I/O APIC" alone: every sentence that explains a fallback names
    # the I/O APIC too - "because nothing answers at the I/O APIC's address" -
    # so the short match would pass on the very boot this exists to catch.
    #
    check("interrupts: an I/O APIC" in out,
          "the default boot did not take the APIC, so this machine only "
          "ever tests one of the two controllers")

    # And the sound, which is the one subsystem this board does not take
    # from virtio. `hal/pc/hda.c` says why an emulated Intel controller is
    # worth more here than an emulated virtio one: it is the same silicon
    # interface a ThinkPad has, so what passes here is what will run there.
    #
    sound(image, check)

    # And the disk, which is the other thing this board does not take from
    # virtio. A ThinkPad's storage is NVMe or nothing - `docs/thinkpad.md`
    # has the table - so the driver that has to work there is the one
    # exercised here, and virtio-blk stays the ARM board's disk so that
    # neither is orphaned. The same argument as the sound controller above,
    # made a second time.
    #
    storage(image, check)

    # And the pointer a laptop has, with and without the serial port it does
    # not have. `pointer` says why the second half is the one that matters.
    #
    pointer(image, check)

    if fails:
        print("FAIL: %d of %d checks on x86-64:"
              % (len(fails), len(fails) + checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on x86-64 (it boots through twelve stages, names "
          "its processor out of CPUID, agrees with userland about the memory "
          "by two paths, answers what is typed at it, runs a program that "
          "reports what it was handed, plays a tone an Intel HDA "
          "controller hands back at the right pitch, keeps a file on an "
          "NVMe drive across a reboot, and opens a menu with a click through "
          "a PS/2 mouse whether or not the machine has a serial port)."
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
