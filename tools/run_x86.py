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

import datetime
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


def boot(image, option, timeout, typed=(), extra=(), until=None, after=None):
    """Boots, optionally types at the prompt, and returns everything printed.

    One line per prompt, and only after the machine has been quiet for a
    moment: a line written into the middle of the boot log is a line the
    console server has not been asked for yet.

    `until` is a line to wait for as well, for output that arrives on its
    own clock rather than a prompt's - a driver reporting after the shell
    is already up.

    `after` is a line to see before typing anything, for a command whose
    answer depends on something that starts on its own clock - the USB
    driver taking its controllers, which a report asked for sooner has, and
    truthfully, as undriven.
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
                    and time.time() - quiet > 0.4
                    and (after is None or after.encode() in out)):
                p.stdin.write(typed[sent].encode() + b"\n")
                p.stdin.flush()
                sent += 1
                quiet = time.time()

            done = (prompts > len(typed)) if typed else (prompts > 0)

            if until is not None and until.encode() not in out:
                done = False

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

    # QEMU's own record of each time the controller is started and stopped,
    # in a file of its own, for the check after this boot.
    started = os.path.join(tempfile.mkdtemp(prefix="kosmos-nvme-trace-"),
                           "starts")

    first = boot(image, None, 90.0,
                 extra=extra + ("-trace", "pci_nvme_mmio_start_success",
                                "-trace", "pci_nvme_mmio_stopped",
                                "-D", started),
                 typed=(
                     "diskinfo",
                     "mkfs --yes",
                     "save notes.txt written before the reboot",
                     "diskinfo",
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

    #
    # **And the controller started once by the kernel**, however many times
    # the disk was asked about. Every `sys.disk()` used to start it again -
    # `diskinfo` reads `/home/.super`, and the server answering asks the
    # kernel about the disk - and QEMU counted ten starts in a boot that ran
    # `diskinfo` three times. Nothing broke, because nothing was in flight
    # when it happened; the kernel now starts it at boot and keeps what it
    # found.
    #
    # **The firmware starts it first**, and the first run of this check
    # counted that as a second start: SeaBIOS brings the drive up to look for
    # something to boot, and leaves it running. The kernel's start begins by
    # stopping it - `nvme_init` clears the enable bit before it sets it - so
    # a start before the first stop is the firmware's, and every start after
    # it is the kernel's.
    #
    try:
        with open(started) as handle:
            traced = handle.read()
    except OSError:
        traced = ""

    stopped = traced.find("pci_nvme_mmio_stopped")
    starts = (traced[stopped:].count("pci_nvme_mmio_start_success")
              if stopped >= 0 else 0)

    check(stopped >= 0 and starts == 1,
          "QEMU saw the kernel start the NVMe controller %d times in one boot "
          "that asked about the disk four times%s; it is meant to start it "
          "once and keep what it found"
          % (starts, "" if stopped >= 0 else
             ", and never saw it stopped, so no start of the kernel's could "
             "be told from the firmware's"))

    second = boot(image, None, 90.0, extra=extra,
                  typed=("cat /home/notes.txt",))

    if second is None:
        check(False, "the machine would not boot the second time")
        return

    check("written before the reboot" in second,
          "the file written over NVMe was not there after a reboot, so the "
          "writes never reached the drive - which a driver that formats and "
          "reads back its own cache looks exactly like")


def qemu_usb(extra):
    """What QEMU itself says is on its USB buses, given these devices.

    A second QEMU with the same `-device` lines, `-S` so it never runs an
    instruction, asked `info usb` over its monitor and killed. `info usb`
    describes QEMU's device model, not the guest, so this is an answer that
    owes nothing to Kosmos - which is the point of asking it.

    Product and speed, and not the port: QEMU numbers its buses' ports
    itself, and the controller puts USB 3 ports first, so the keyboard QEMU
    calls port 1 is the controller's port 5.
    """
    work = tempfile.mkdtemp(prefix="kosmos-infousb-")
    path = os.path.join(work, "monitor")
    cmd = [QEMU, "-M", "q35", "-m", "512M", "-S", "-no-reboot",
           "-display", "none", "-serial", "none",
           "-monitor", "unix:%s,server,nowait" % path] + list(extra)

    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    said = ""

    try:
        monitor = Monitor(path)
        said = monitor.ask("info usb", quiet=0.5)
        monitor.close()
    except RuntimeError:
        pass
    finally:
        proc.kill()
        proc.wait()

    return re.findall(r"Device \S+, Port \S+, Speed ([\d.]+) Mb/s, "
                      r"Product ([^\r\n]+)", said)


def qemu_pci(extra):
    """Every PCI function QEMU itself has, given these devices, as
    `vendor:device` strings - from `info pci` on a second QEMU, as `qemu_usb`
    asks `info usb`.

    **Asked once its firmware has run, and not before.** The bus behind a
    bridge is numbered by the firmware, which writes the number into the
    bridge as it enumerates, and QEMU's `info pci` walks the buses by those
    numbers - so a machine stopped before its first instruction lists nothing
    behind a root port. The first run of `machine_report` asked that one and
    was told the drive did not exist. SeaBIOS numbers the buses within a
    second, and then looks for something to boot, of which there is none.
    """
    work = tempfile.mkdtemp(prefix="kosmos-infopci-")
    path = os.path.join(work, "monitor")
    cmd = [QEMU] + ARGS + ["-S", "-serial", "none",
                           "-monitor", "unix:%s,server,nowait" % path] \
        + list(extra)

    proc = subprocess.Popen(cmd, stdin=subprocess.DEVNULL,
                            stdout=subprocess.DEVNULL,
                            stderr=subprocess.DEVNULL)
    said = ""

    try:
        monitor = Monitor(path)
        monitor.ask("cont", quiet=0.3)
        time.sleep(3.0)
        said = monitor.ask("info pci", quiet=0.5)
        monitor.close()
    except RuntimeError:
        pass
    finally:
        proc.kill()
        proc.wait()

    return re.findall(r"PCI device ([0-9a-f]{4}:[0-9a-f]{4})", said)


def qemu_binary():
    """The bytes of the QEMU this runs, or None: strings a device model
    carries are in it, which makes it a source for them that owes nothing
    to Kosmos."""
    for folder in os.environ.get("PATH", "").split(os.pathsep):
        path = os.path.join(folder, QEMU)

        if os.path.isfile(path):
            with open(path, "rb") as handle:
                return handle.read()

    return None


# `info usb`'s rates as the controller names them (xHCI 1.2, Table 7-13).
QEMU_SPEEDS = {"1.5": "Low-speed", "12": "Full-speed", "480": "High-speed",
               "5000": "SuperSpeed", "10000": "SuperSpeedPlus"}


def stick_with_gpt(path):
    """A 16 MB stick laid out as `mkusb_image.py` lays out a real one: a
    protective MBR, a GPT at both ends, and one partition of zeros between
    them, so step 5a has a header to find at block 1 and its backup at the
    last block. Its size in blocks is returned."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkusb_image

    blocks = 16 * 1024 * 1024 // mkusb_image.SECTOR
    partition = path + ".partition"

    # write_gpt puts 34 blocks before the partition and 33 after it.
    with open(partition, "wb") as handle:
        handle.truncate((blocks - 67) * mkusb_image.SECTOR)

    try:
        size = mkusb_image.write_gpt(path, partition,
                                     (blocks - 67) * mkusb_image.SECTOR)
    finally:
        os.unlink(partition)

    return size // mkusb_image.SECTOR


def usb(image, check):
    """Two xHCI controllers, a USB stick on the second, a keyboard on the
    first, and what the driver in `user/servers/xhci.c` says it found and
    what the devices said they are.

    **Two controllers, because the index is the part one cannot test.** A
    driver that always asked for the first would find a controller, take it,
    reset it and read its ports perfectly - and on a laptop with two it would
    never see the other. With the stick on the second, that driver reports
    nothing plugged in anywhere, and the check says so by name.

    **Step one** is the controllers found by class, taken from the firmware
    where there is firmware, reset, and their ports read. **The firmware
    handoff is the one path QEMU cannot reach**: its controller has no
    legacy-support capability, and the driver says that rather than claiming
    a handoff it never made.

    **Step two** is each controller given its rings and its interrupter and
    started; a No-Op command answered on the event ring **by interrupt** -
    the first MSI to reach a process on x86, which is why "found by looking"
    fails here rather than passing quietly; the keyboard's USB 2 port reset,
    after which, and only after which, its speed is named; and both devices
    given a slot and an address and asked for their descriptors.

    **And every 64-bit register written low half first and high half
    second** (xHCI 1.2 5.1), read out of QEMU's own trace of the writes the
    controllers were given. QEMU acts on ERDP's low half, so nothing it does
    can show the order; the driver wrote ERDP high half first from 0.10.54,
    and on the ThinkPad that fits every naming step taking a second and a
    mouse read only when a deadline came round.

    **What the devices say is compared with what QEMU says**, from a second
    QEMU's monitor, as product and speed. The keyboard is high-speed - 480 Mb/s
    - which this docstring once had as full-speed.

    **Step four is bytes each way on the stick's bulk endpoints**: its
    configuration read as SCSI over Bulk-Only, both endpoints given to the
    controller, and one command carried through them - INQUIRY, 31 bytes out,
    36 in and a 13-byte status - which QEMU's stick answers from
    `hw/scsi/scsi-disk.c` as "QEMU", "QEMU HARDDISK", a direct-access device.
    It is at SuperSpeed here, on the second controller's USB 3 port, so its
    endpoints are 1024 bytes a packet in bursts of 16 (`hw/usb/dev-storage.c`).

    **Step 5a is the stick's size and its first blocks**: TEST UNIT READY until
    it is ready, READ CAPACITY (10), and READ (10) of block 1 and of the last
    block - where the stick, laid out by `mkusb_image.write_gpt` as a real one
    is, holds a GPT header and its backup. Each header is held to its CRC and
    to the block it says it is at, so a read of the wrong block fails.
    """
    stick = os.path.join(tempfile.gettempdir(), "kosmos-x86-usb-stick.img")
    stick_blocks = stick_with_gpt(stick)

    extra = ("-device", "qemu-xhci,id=usb0",
             "-device", "qemu-xhci,id=usb1",
             "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
             "-device", "usb-storage,bus=usb1.0,drive=stick",
             "-device", "usb-kbd,bus=usb0.0")

    # QEMU's trace of what the controllers were written, in a file of its
    # own: on the serial line its lines would land inside the driver's.
    traced = os.path.join(tempfile.mkdtemp(prefix="kosmos-xhci-trace-"),
                          "writes")

    # The closing line, whatever it counts: "devices named" is never printed
    # by a run that names one device, which then waited out the timeout.
    out = boot(image, None, 90.0,
               extra=extra + ("-trace", "usb_xhci_oper_write",
                              "-trace", "usb_xhci_runtime_write",
                              "-D", traced),
               until="plugged in, ")

    if out is None:
        check(False, "the machine would not boot with two xHCI controllers")
        return

    said = [l[l.index("xhci:"):].strip()
            for l in out.replace("\r", "").splitlines() if "xhci:" in l]
    shown = "\n    ".join(said) or "(the driver said nothing)"
    at = r"([0-9a-f]{2}:[0-9a-f]{2}\.[0-7])"

    found = re.findall(r"xhci: " + at + r", version (\d+\.\d+), "
                       r"(\d+) ports, (\d+) slots", out)

    check(len(found) == 2 and found[0][0] != found[1][0],
          "the driver did not report two different xHCI controllers:\n    "
          + shown)

    # Step two's start: both running, each with its interrupt claimed.
    running = re.findall(r"xhci: " + at + r" runs: contexts of (\d+) bytes, "
                         r"(\d+) scratchpad pages?, (\d+) slots enabled, "
                         r"interrupt (\d+)(?! not claimed)", out)

    check(len(running) == 2,
          "the driver did not start both controllers with an interrupt "
          "claimed:\n    " + shown)

    answered = re.findall(r"xhci: " + at + r" answered a No-Op command on "
                          r"its event ring, by interrupt", out)

    check(len(answered) == 2,
          "a No-Op command was not answered by interrupt on both controllers"
          " - \"found by looking\" means the rings work and no interrupt "
          "reached the driver:\n    " + shown)

    ports = re.findall(r"xhci: " + at + r" port (\d+), USB (\d): a (\S+) "
                       r"device \(speed ID (\d+)\)(, after its reset)?", out)

    check(len(ports) == 2,
          "the driver did not report exactly two devices plugged in:\n    "
          + shown)

    # A speed on a USB 2 port only once the port has been reset (Table 5-27).
    check(all(usb_major != "2" or reset for _, _, usb_major, _, _, reset
              in ports),
          "a speed was named on a USB 2 port that had not been reset, where "
          "the field is invalid:\n    " + shown)

    if len(found) == 2:
        check(re.search(r"xhci: %s port \d+, USB 2: a High-speed device "
                        r"\(speed ID 3\), after its reset"
                        % re.escape(found[0][0]), out) is not None,
              "the keyboard on the first controller, %s, was not reported as a "
              "high-speed device after its port's reset:\n    "
              % found[0][0] + shown)

    devices = re.findall(r"xhci: " + at + r" port (\d+): ([0-9a-f]{4}):"
                         r"([0-9a-f]{4}), USB (\d+\.\d), class (\d+), "
                         r"\"([^\"]*)\"", out)

    check(len(devices) == 2,
          "the driver did not read two devices' descriptors and product "
          "strings:\n    " + shown)

    # `info usb`'s "Product" is QEMU's name for the device model, and for the
    # stick that is not its string descriptor - "QEMU USB MSD" against the
    # "QEMU USB HARDDRIVE" the stick itself says, which the first run of this
    # check found. So the speeds are compared with `info usb`, and each
    # product string must be one the emulator itself carries.
    speed_of = {(a, p): s for a, p, _, s, _, _ in ports}
    from_driver = sorted(speed_of.get((a, p), "?")
                         for a, p, _, _, _, _, _ in devices)
    from_qemu = sorted(QEMU_SPEEDS.get(rate, rate + " Mb/s")
                       for rate, _ in qemu_usb(extra))

    check(len(from_qemu) == 2,
          "QEMU's own monitor did not list the two devices to compare "
          "with: %r" % (from_qemu,))

    check(from_driver == from_qemu,
          "the devices' speeds are not the ones QEMU says it attached "
          "them at:\n    driver: %r\n    QEMU:   %r" % (from_driver, from_qemu))

    carried = qemu_binary()
    named = [name for _, _, _, _, _, _, name in devices]

    check(carried is not None and named
          and all(name.encode() in carried for name in named),
          "a product string the driver read is not one the QEMU binary "
          "carries: %r" % (named,))

    # Step four: the stick's bulk endpoints, and INQUIRY through them.
    bulk = re.search(r"xhci: " + at + r" port \d+: a stick: SCSI over "
                     r"Bulk-Only, bulk IN endpoint (\d+) and OUT endpoint "
                     r"(\d+), up to (\d+) bytes a packet in bursts of (\d+)",
                     out)
    inquiry = re.search(r"xhci: " + at + r" port \d+: the stick says it is "
                        r"\"([^\"]*)\" \"([^\"]*)\", revision "
                        r"\"([^\"]*)\", device type (\d+)", out)

    check(bulk is not None and bulk.group(2, 3, 4, 5) == ("1", "2", "1024",
                                                          "16"),
          "the stick's bulk endpoints were not given to the controller as QEMU "
          "declares them - IN 1 and OUT 2, 1024 bytes a packet in bursts of "
          "16:\n    " + shown)

    check(inquiry is not None
          and inquiry.group(2, 3, 5) == ("QEMU", "QEMU HARDDISK", "0"),
          "the stick did not answer INQUIRY through its bulk endpoints as "
          "QEMU's disk does - \"QEMU\", \"QEMU HARDDISK\", device type "
          "0:\n    " + shown)

    # Step 5a: the stick's size, then a GPT header at block 1 and its backup
    # at the last block, through READ CAPACITY (10) and READ (10).
    capacity = re.search(r"xhci: " + at + r" port \d+: the stick holds (\d+) "
                         r"blocks of (\d+) bytes", out)
    table = re.search(r"xhci: " + at + r" port \d+: block 1 holds (a|no) GUID "
                      r"partition table's header, and block (\d+) (its|no) "
                      r"backup", out)

    check(capacity is not None
          and capacity.group(2, 3) == (str(stick_blocks), "512"),
          "the stick did not say it holds %d blocks of 512 bytes, which READ "
          "CAPACITY (10) should find in its 16 MB:\n    " % stick_blocks
          + shown)

    check(table is not None
          and table.group(2, 3, 4) == ("a", str(stick_blocks - 1), "its"),
          "READ (10) did not find the stick's GPT header at block 1 and its "
          "backup at block %d, where `mkusb_image.write_gpt` put them:\n    "
          % (stick_blocks - 1) + shown)

    if len(found) == 2:
        stick_on = [a for a, _, usb_major, _, _, _ in ports
                    if usb_major == "3"]

        check(stick_on == [found[1][0]],
              "the stick is on the second controller, %s, and the driver "
              "named it on %r - which is what asking for the first "
              "controller twice looks like" % (found[1][0], stick_on))

    check(re.search(r"xhci: 2 controllers \([0-9a-f]{2}:[0-9a-f]{2}\.[0-7], "
                    r"[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]\), 2 ports with something "
                    r"plugged in, 2 devices named", out) is not None,
          "the driver's closing line is not what two controllers, a stick "
          "and a keyboard should give:\n    " + shown)

    #
    # **The 64-bit registers, low half first** (5.1): CRCR and DCBAAP among
    # the operational registers, ERSTBA and ERDP among interrupter 0's.
    #
    # QEMU's firmware drives the controllers before Kosmos does and writes
    # the same registers, so the writes looked at start from the driver's
    # own CONFIG - the slots its line says it enabled, where SeaBIOS enables
    # every slot there is. Everything after that is the driver's: it is one
    # thread, and nothing else in the machine writes a controller.
    #
    wrote = []

    try:
        with open(traced) as handle:
            wrote = re.findall(r"usb_xhci_(oper|runtime)_write off "
                               r"0x([0-9a-f]+), val 0x([0-9a-f]+)",
                               handle.read())
    except OSError:
        pass

    wrote = [(kind, int(off, 16), int(val, 16)) for kind, off, val in wrote]
    halves = {("oper", 0x18): "CRCR", ("oper", 0x30): "DCBAAP",
              ("runtime", 0x30): "ERSTBA", ("runtime", 0x38): "ERDP"}
    slots = int(running[0][3]) if running else -1
    first = next((i for i, (kind, off, val) in enumerate(wrote)
                  if (kind, off) == ("oper", 0x38) and val == slots), None)
    counted = {}
    wrong = []

    for i in range(len(wrote) if first is None else first, len(wrote)):
        kind, off, _ = wrote[i]

        if (kind, off) in halves:
            name = halves[(kind, off)]
            counted[name] = counted.get(name, 0) + 1

            if i + 1 >= len(wrote) or wrote[i + 1][:2] != (kind, off + 4):
                wrong.append("%s's low half with no high half after it"
                             % name)
        elif (kind, off - 4) in halves and wrote[i - 1][:2] != (kind, off - 4):
            wrong.append("%s's high half before its low half"
                         % halves[(kind, off - 4)])

    if first is None:
        why = "no CONFIG write of %d slots to start from" % slots
    elif wrong:
        why = "the order broken in %d places, the first %s" % (len(wrong),
                                                                wrong[0])
    else:
        why = "only %s written" % ", ".join(sorted(counted))

    check(first is not None and len(counted) == 4 and not wrong,
          "the driver did not write every 64-bit register low half first and "
          "high half second (xHCI 1.2 5.1), in QEMU's trace of %d writes: %s"
          % (len(wrote), why))


def usb_hotplug(image, check):
    """A keyboard pulled out and put back, once for every slot it could take.

    **Through QEMU's monitor, the way a person pulls a cable.** `device_del`
    detaches the keyboard and `device_add` puts another on the same port -
    `port=1` on its bus, which the driver calls port 5. The driver has to say
    the port is unplugged and name what left, then name the new keyboard on
    that port exactly as it named the one at boot.

    **Every round must bring exactly one of each.** A port raises no further
    change events until every change bit is cleared (xHCI 1.2 4.19.2), and a
    driver that never clears them sees the same change on every pass.

    **The rounds outnumber the slots, because a slot is what an unplug has to
    give back.** QEMU forgets which port a slot was for as the device leaves
    (`xhci_detach_slot` in `hcd-xhci.c`), so a replug is addressed whether or
    not the driver disabled the slot before it: two rounds passed with that
    code taken out, which is how this was found. What QEMU will not do is
    hand out a slot that is still enabled. So the keyboard goes back once for
    every slot its controller says it enabled - with the keyboard found at
    boot, one more than there are slots - and a driver that keeps them runs
    out before the last.
    """
    binary = os.path.join(os.path.dirname(image), "kosmos.bin")
    work = tempfile.mkdtemp(prefix="kosmos-x86-hotplug-")
    path = os.path.join(work, "monitor")
    stick = os.path.join(tempfile.gettempdir(), "kosmos-x86-usb-stick.img")

    with open(stick, "wb") as handle:
        handle.truncate(16 * 1024 * 1024)

    cmd = [QEMU, "-M", "q35", "-m", "512M", "-no-reboot",
           "-display", "none", "-serial", "stdio",
           "-monitor", "unix:%s,server,nowait" % path,
           "-device", "qemu-xhci,id=usb0",
           "-device", "qemu-xhci,id=usb1",
           "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
           "-device", "usb-storage,bus=usb1.0,drive=stick",
           "-device", "usb-kbd,bus=usb0.0,id=kbd0",
           "-kernel", binary]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL)
    heard = bytearray()

    # On a thread, for the pointer check's reason: a guest whose serial line
    # is not read stops inside `kputc` once the pipe is full.
    def drain():
        while True:
            chunk = os.read(proc.stdout.fileno(), 65536)

            if not chunk:
                return

            heard.extend(chunk)

    threading.Thread(target=drain, daemon=True).start()

    def since(mark):
        return heard[mark:].decode("utf-8", "replace").replace("\r", "")

    def wait_for(mark, pattern, seconds):
        until = time.time() + seconds

        while time.time() < until:
            if re.search(pattern, since(mark)):
                return True

            time.sleep(0.25)

        return False

    unplugged = (r'xhci: [0-9a-f]{2}:[0-9a-f]{2}\.[0-7] port (\d+): unplugged, '
                 r'0627:0001 "QEMU USB Keyboard"')
    named = (r'xhci: [0-9a-f]{2}:[0-9a-f]{2}\.[0-7] port (\d+): 0627:0001, '
             r'USB 2\.0, class 0, "QEMU USB Keyboard"')

    try:
        monitor = Monitor(path)

        if not wait_for(0, r"xhci: watching for devices plugged in and out",
                        90.0):
            check(False, "the USB driver never said it was watching for "
                         "devices:\n    " + repr(since(0)[-400:]))
            return

        boot = since(0)
        keyboard = re.search(r"xhci: ([0-9a-f]{2}:[0-9a-f]{2}\.[0-7]) port "
                             r"\d+: 0627:0001", boot)
        enabled = keyboard and re.search(
            r"xhci: %s runs: [^\n]* (\d+) slots enabled"
            % re.escape(keyboard.group(1)), boot)
        rounds = int(enabled.group(1)) if enabled else 0

        check(rounds >= 2,
              "the keyboard's controller did not say it enabled two slots or "
              "more, so there is nothing to run out of:\n    "
              + repr(boot[-600:]))

        # A failed round ends it: the next would start from a keyboard that
        # is not where the check thinks, and wait out both timeouts to say so.
        for round_ in range(1, rounds + 1):
            gone, new = "kbd%d" % (round_ - 1), "kbd%d" % round_

            mark = len(heard)
            monitor.ask("device_del " + gone)
            left = wait_for(mark, unplugged, 20.0)
            time.sleep(1.0)
            ports_left = re.findall(unplugged, since(mark))
            ok = left and len(ports_left) == 1

            check(ok, "round %d of %d: pulling the keyboard out did not bring "
                      "exactly one unplug line naming it:\n    %r"
                      % (round_, rounds, since(mark)[-400:]))

            if not ok:
                break

            mark = len(heard)
            monitor.ask("device_add usb-kbd,bus=usb0.0,port=1,id=" + new)
            came = wait_for(mark, named, 30.0)
            time.sleep(1.0)
            after = since(mark)
            ok = (came and re.findall(named, after) == ports_left
                  and not re.search(unplugged, after))

            check(ok, "round %d of %d: putting a keyboard back on port %s did "
                      "not name it there exactly once, with nothing "
                      "unplugged:\n    %r"
                      % (round_, rounds, ports_left[0], after[-600:]))

            if not ok:
                break

        monitor.close()
    finally:
        proc.kill()
        proc.wait()


def memdisk(image, check):
    """Boots with a disk the loader handed over, and reads a file off it.

    **The disk a ThinkPad gets its game data from**, and the path it takes:
    GRUB loads a kfs image from the USB stick into memory as a module, and
    `hal/pc/memdisk.c` presents that memory as the board's block device.
    QEMU's `-kernel` does the loader's half with `-initrd`, which puts the
    image in memory as a Multiboot 1 module - so this is everything from the
    kernel finding the module to the filesystem mounting it, without the
    firmware.

    **An empty NVMe drive is attached as well.** The loader's disk is meant
    to win over a real drive, and the drive holds nothing, so a file that
    comes back could only have come out of the module. It also catches the
    module's pages being handed to the allocator: the first processes would
    be built on top of the disk, and the file would not survive to be read.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    lua = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(image))),
                       "host", "lua")
    work = tempfile.mkdtemp()
    marker = os.path.join(work, "marker.txt")
    disk = os.path.join(work, "loader-disk.img")
    drive = os.path.join(work, "empty-nvme.img")

    with open(marker, "w") as handle:
        handle.write("carried in by the loader, not the drive\n")

    made = subprocess.run([lua, os.path.join(here, "kfs.lua"), "create", disk,
                           "8", marker + ":/home/marker.txt"],
                          capture_output=True, text=True)

    if made.returncode != 0:
        check(False, "kfs.lua could not make the loader's disk: "
              + (made.stderr or made.stdout).strip())
        return

    with open(drive, "wb") as handle:
        handle.truncate(64 * 1024 * 1024)

    extra = ("-initrd", disk,
             "-drive", "file=%s,format=raw,if=none,id=nvme0" % drive,
             "-device", "nvme,drive=nvme0,serial=kosmos")

    out = boot(image, None, 90.0, extra=extra,
               typed=("diskinfo", "cat /home/marker.txt"))

    if out is None:
        check(False, "the machine would not boot with a disk from the loader")
        return

    check("a disk from the loader: 8192 KB" in out,
          "the boot log did not name the loader's disk, so the module was "
          "never captured: "
          + next((l.strip() for l in out.splitlines() if "loader" in l),
                 "nothing was said about a loader at all"))

    check("disk: 16384 sectors" in out,
          "`diskinfo` did not report the loader's 8 MB disk as 16384 sectors, "
          "so the module was not the disk that was bound: "
          + next((l.strip() for l in out.splitlines() if "disk:" in l),
                 "nothing was said about a disk at all"))

    check("carried in by the loader, not the drive" in out,
          "the file on the loader's disk did not come back - either the "
          "module was never read, or the allocator was given its pages and "
          "something was built on top of it")



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

        #
        # The Kosmos end of the bar, not the middle of it.
        #
        # This was the centre of the Deskbar's window plus 24, which was the
        # button when the Deskbar was a small window in the top-right
        # corner. It is the strip across the whole top now - `0,0 1920x36` -
        # so the centre is somewhere among the buttons for running windows,
        # and a click there raises an application instead of opening the
        # menu. The menu is at the left, where the Kosmos button is.
        #
        at_x, at_y = deskbar[0] + 40, deskbar[1] + 18

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


# A ring's requests before its Link back to the first: `RING_TRBS - 1` in
# `user/servers/xhci.c`.
RING_REQUESTS = 255

# Movements before the click - past two rounds of that, with room - and the
# quiet asked for after each. QEMU folds a movement into the one before while
# that one is unread, so a driver that reads late reads fewer reports; and a
# gap has to be well under the 50 ms such a driver waits for that to show.
MOVEMENTS_BEFORE_THE_CLICK = 640
MOVEMENT_GAP = 0.01
MOVEMENT_GAP_MOST_MS = 30.0

# A keyboard plugged in while the mouse moves, once on each controller: the
# movements around it and the one it comes before, how long after it the
# mouse's requests are read, and the longest gap between two of them that
# still counts as a mouse being read. A plug that held the mouse made it 104
# and 107 ms in QEMU's trace - USB 2.0's debounce - where movements are 12
# apart.
PLUG_MOVEMENTS = 240
PLUG_AT = 80
PLUG_WINDOW = 1.5
PLUG_GAP_MOST_MS = 50.0


def usb_mouse(image, check):
    """A USB mouse on the second of two controllers moves the pointer and
    clicks the Deskbar's button - after enough reports to go round its ring
    twice - lets go of a button held as it is pulled out, and is read again
    when it is plugged back in.

    **The mouse is the first thing to go round a ring.** A ring is 255
    requests and a Link back to its start (`ring_push` in `xhci.c`), and
    nothing before the mouse sent one ring that many: a keyboard replugged
    for every slot is a few dozen commands. A mouse sends a request for each
    report, 125 a second, so the Link and the cycle bit it turns over are
    reached within two seconds of moving it - on the ThinkPad, where a mistake
    there is a mouse that stops. So the monitor moves it to and fro past two
    rounds first, and the click comes after.

    **The second controller, with nothing on the first**, because a driver
    that waits on one controller at a time still clicks - its reports only
    arrive late. What it cannot do is keep up: QEMU folds a movement into the
    one before while that one is unread, so a mouse read late sends fewer
    reports, and their count against the movements is the check. The
    movements are 20 ms apart, and a driver asleep on the other controller for
    50 ms reads fewer than one report in two. `mouse_set` routes the monitor's
    movement to the USB mouse rather than the PS/2 mouse every q35 machine
    also has, so the menu opens through USB or not at all.

    **On the machine's own interrupt controller, as the ThinkPad boots**, and
    not `opt/kosmos/irq=pic` as `pointer` does. Under the 8259s QEMU's two
    controllers share line 11: the second's claim is refused, it is polled,
    and the shared line wakes the driver for both - so the wait on two lines
    would never run, and a driver waiting on one would pass. The first run of
    this check was on the 8259s, and that is how it was found.

    **A button held as the mouse is pulled out has to come up.** The pointer
    holds each source's buttons, and a driver that forgot a mouse's as it left
    would leave the desktop dragging for good. QEMU sends no release for a
    device it deletes, which is exactly that case.

    **A keyboard plugged in while the mouse moves does not stop it being
    read**, on the other controller or on its own. Naming a device waits -
    USB 2.0's debounce, a reset, commands and transfers - and every one of
    those waits reads mice (`wait_serving` in `xhci.c`). When they did not,
    QEMU's trace had 104 and 107 ms between two of the mouse's requests
    during a plug, where the movements are 12 ms apart; the trace is read
    once QEMU has gone, because it writes as it pleases.

    **And the mouse that goes back in is full-speed**, like the ThinkPad's,
    whose interval is milliseconds rather than a power of two.
    """
    binary = os.path.join(os.path.dirname(image), "kosmos.bin")
    work = tempfile.mkdtemp(prefix="kosmos-x86-usbmouse-")
    path = os.path.join(work, "monitor")
    traced = os.path.join(work, "trace")
    plugs = []
    endpoint = 0
    cmd = [QEMU, "-M", "q35,vmport=off", "-m", "512M", "-no-reboot",
           "-display", "none", "-vga", "none", "-device", "ramfb",
           "-monitor", "unix:%s,server,nowait" % path,
           "-serial", "stdio",
           "-device", "qemu-xhci,id=usb0",
           "-device", "qemu-xhci,id=usb1",
           "-device", "usb-mouse,bus=usb1.0,port=1,id=mouse0",
           "-fw_cfg", "name=opt/kosmos/boot,string=wm",
           # Every doorbell the driver rings for an endpoint, with its time.
           "-msg", "timestamp=on", "-trace", "usb_xhci_ep_kick",
           "-D", traced,
           "-kernel", binary]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL)
    heard = bytearray()

    # On a thread, for the pointer check's reason: a guest whose serial line
    # is not read stops inside `kputc` once the pipe is full.
    def drain():
        while True:
            chunk = os.read(proc.stdout.fileno(), 65536)

            if not chunk:
                return

            heard.extend(chunk)

    threading.Thread(target=drain, daemon=True).start()

    def since(mark):
        return heard[mark:].decode("utf-8", "replace").replace("\r", "")

    def wait_for(mark, pattern, seconds):
        until = time.time() + seconds

        while time.time() < until:
            found = re.search(pattern, since(mark))

            if found:
                return found

            time.sleep(0.25)

        return None

    def said():
        return "\n    ".join(l.strip() for l in since(0).splitlines()
                             if "xhci:" in l or "wm: button" in l)[-1500:]

    def route_to_usb(monitor):
        """The monitor's mouse, made QEMU's USB one. True if it took."""
        listed = re.search(r"Mouse #(\d+): QEMU HID Mouse",
                           monitor.ask("info mice", quiet=0.3))

        if listed is None:
            return False

        monitor.ask("mouse_set " + listed.group(1), quiet=0.3)
        return re.search(r"\* Mouse #\d+: QEMU HID Mouse",
                         monitor.ask("info mice", quiet=0.3)) is not None

    at = r"[0-9a-f]{2}:[0-9a-f]{2}\.[0-7]"
    reading = (r"xhci: (" + at + r") port (\d+): a mouse, read from "
               r"endpoint (\d+), up to (\d+) bytes every (\d+) (ms|us)")
    by_descriptor = (r"xhci: " + at + r" port \d+: its reports, by its "
                     r"descriptor: ([^\n]*)")
    first_report = r"xhci: " + at + r" port \d+: the mouse's first report"

    try:
        monitor = Monitor(path)

        mouse = wait_for(0, reading, 120.0)
        closing = wait_for(0, r"xhci: 2 controllers \((" + at + r"), ("
                           + at + r")\)", 30.0)
        deskbar = wait_for(0, r"wm: window Deskbar at (\d+),(\d+) (\d+)x(\d+)",
                           120.0)

        check(mouse is not None,
              "the USB driver did not say it was reading QEMU's mouse:\n    "
              + said())

        #
        # **And read by what its Report descriptor says**, which is QEMU's
        # boot layout exactly - five buttons, padding, X, Y, a wheel - so the
        # clicks and movements below pass either way. What they cannot tell
        # is whether the driver read the descriptor, and a mouse that ignores
        # the boot protocol, as the ThinkPad's does, is where that decides
        # everything. `test_usbdecode.c` has the layouts QEMU cannot send.
        #
        layout = wait_for(0, by_descriptor, 10.0)

        check(layout is not None and layout.group(1).strip()
              == "5 buttons from bit 0, X from bit 8 in 8, Y from bit 16 in "
                 "8, no report ID",
              "the driver did not read QEMU's mouse by its Report descriptor "
              "- five buttons, then X and Y a byte each:\n    " + said())

        check(mouse is not None and closing is not None
              and mouse.group(1) == closing.group(2),
              "the mouse is on the second controller and was not read "
              "there:\n    " + said())

        # Two lines, or the wait on every controller at once waits on one.
        lines = re.findall(r"xhci: " + at + r" runs: [^\n]*, interrupt (\d+)\n",
                           since(0))

        check(len(lines) == 2 and lines[0] != lines[1],
              "the two controllers did not each run on an interrupt of their "
              "own, so the driver's wait is not on two lines:\n    " + said())

        check(deskbar is not None,
              "booted into the desktop with a USB mouse, the Deskbar never "
              "opened a window")

        if mouse is None or deskbar is None:
            return

        # The rest of the login windows placed first, as in `pointer`.
        time.sleep(6.0)

        screen = monitor.screendump(os.path.join(work, "geometry.ppm"))

        check(screen is not None, "QEMU would not screendump the desktop")

        if screen is None:
            return

        width, height = screen[0], screen[1]
        at_x, at_y = int(deskbar.group(1)) + 40, int(deskbar.group(2)) + 18

        routed = route_to_usb(monitor)

        check(routed, "QEMU would not route the monitor's mouse to its USB "
                      "mouse: " + repr(monitor.ask("info mice", quiet=0.3)))

        if not routed:
            return

        # To and fro, so the pointer ends where it started - on a socket that
        # gives up after 2 ms rather than `Monitor`'s 50. `ask` returns once
        # the monitor has been quiet as long as it was asked, and it cannot
        # see a quiet shorter than its socket's timeout: at 50 ms every
        # movement came 50 ms after the last, and a driver waiting 50 ms on
        # the wrong controller kept up with them and passed.
        monitor.sock.settimeout(0.002)
        began = time.time()

        for i in range(MOVEMENTS_BEFORE_THE_CLICK):
            monitor.ask("mouse_move %d 0" % (1 if i % 2 == 0 else -1),
                        quiet=MOVEMENT_GAP)

        gap_ms = (time.time() - began) * 1000.0 / MOVEMENTS_BEFORE_THE_CLICK
        monitor.sock.settimeout(0.05)

        check(gap_ms <= MOVEMENT_GAP_MOST_MS,
              "the movements were %.1f ms apart, too far apart to tell a "
              "driver that reads late from one that does not" % gap_ms)

        mark = len(heard)
        click(monitor, at_x, at_y, width, height)
        menu = wait_for(mark, r"wm: menu of Deskbar at (\d+),(\d+) (\d+)x(\d+)",
                        20.0)

        check(menu is not None,
              "after %d movements a click on the Deskbar's button through "
              "QEMU's USB mouse opened no menu; the driver and the window "
              "manager said:\n    %s" % (MOVEMENTS_BEFORE_THE_CLICK, said()))

        check(re.search(first_report, since(0)) is not None,
              "the driver never said it read the mouse's first report:\n    "
              + said())

        #
        # A keyboard plugged in while the mouse moves - into the other
        # controller, then into the mouse's own - and named. The times of the
        # plugs are kept, and the mouse's requests around them are read out of
        # the trace below, once QEMU has gone.
        #
        endpoint = int(mouse.group(3))

        for n, (bus, where) in enumerate(
                (("usb0.0,port=1", "the other controller"),
                 ("usb1.0,port=2", "the mouse's own controller"))):
            mark = len(heard)
            monitor.sock.settimeout(0.002)

            for i in range(PLUG_MOVEMENTS):
                if i == PLUG_AT:
                    plugs.append((where, time.time()))
                    monitor.ask("device_add usb-kbd,bus=%s,id=plugged%d"
                                % (bus, n), quiet=MOVEMENT_GAP)

                monitor.ask("mouse_move %d 0" % (1 if i % 2 == 0 else -1),
                            quiet=MOVEMENT_GAP)

            monitor.sock.settimeout(0.05)
            named = wait_for(mark, r'xhci: [^\n]*: 0627:0001, [^\n]*'
                             r'"QEMU USB Keyboard"', 20.0)

            check(named is not None,
                  "a keyboard plugged into %s while the USB mouse moved was "
                  "never named:\n    %s" % (where, said()))

        # A button held, and the mouse pulled out while it is.
        mark = len(heard)
        monitor.ask("mouse_button 1", quiet=0.1)
        down = wait_for(mark, r"wm: button down", 10.0)

        check(down is not None,
              "a button pressed on the USB mouse never reached the window "
              "manager:\n    " + said())

        mark = len(heard)
        monitor.ask("device_del mouse0")
        gone = wait_for(mark, r'xhci: ' + at + r' port \d+: unplugged, '
                        r'0627:0001 "QEMU USB Mouse", after (\d+) reports?, '
                        r'(\d+) found by looking', 20.0)
        up = wait_for(mark, r"wm: button up", 10.0)

        check(gone is not None,
              "pulling the USB mouse out brought no unplug line naming it and "
              "counting its reports:\n    " + said())

        reports = int(gone.group(1)) if gone else 0

        check(reports >= 2 * RING_REQUESTS,
              "the driver read %d reports before the mouse was pulled out, "
              "fewer than two rounds of a ring (%d), so its Link was not "
              "tested" % (reports, 2 * RING_REQUESTS))

        check(reports * 5 >= MOVEMENTS_BEFORE_THE_CLICK * 4,
              "the driver read %d reports for %d movements %.1f ms apart - "
              "fewer than four in five, which is a mouse read late: QEMU "
              "folds a movement into the one before while that is unread"
              % (reports, MOVEMENTS_BEFORE_THE_CLICK, gap_ms))

        # **And brought by the controller's interrupt**, which a count of
        # reports cannot tell from a driver that looks often enough. The line
        # when the mouse leaves says how many were found by looking instead,
        # which is also how a photograph of the ThinkPad says whether its
        # reports come by interrupt.
        looked = int(gone.group(2)) if gone else reports

        check(gone is not None and looked * 10 <= reports,
              "%d of the %d reports were found by looking rather than brought "
              "by the controller's interrupt, more than one in ten"
              % (looked, reports))

        check(up is not None,
              "a button held on the USB mouse as it was pulled out never came "
              "up:\n    " + said())

        # And plugged back in, where it is read again from its first report -
        # a full-speed mouse this time, as the ThinkPad's is. `usb_version=1`
        # gives QEMU's mouse only its full-speed descriptors, whose bInterval
        # is 10 milliseconds rather than a power of two: the driver has to
        # read it as 8 ms (xHCI 6.2.3.6), and taking it for a power would
        # make it 64.
        mark = len(heard)
        monitor.ask("device_add usb-mouse,usb_version=1,bus=usb1.0,port=1,"
                    "id=mouse1")
        again = wait_for(mark, reading, 30.0)
        full = re.search(r"xhci: " + at + r" port \d+, USB 2: a Full-speed "
                         r"device \(speed ID 1\)", since(mark))
        first = None

        if again is not None and route_to_usb(monitor):
            monitor.ask("mouse_move 5 5", quiet=0.1)
            first = wait_for(mark, first_report, 10.0)

        check(again is not None and first is not None,
              "a USB mouse plugged back in was not read again from its first "
              "report:\n    " + said())

        check(full is not None and again is not None
              and (again.group(5), again.group(6)) == ("8", "ms"),
              "a full-speed mouse asking for a report every 10 ms was not "
              "read every 8 ms:\n    " + said())

        monitor.close()
    finally:
        proc.kill()
        proc.wait()

    #
    # **The mouse's requests after each plug**, from QEMU's trace now that it
    # has written all of it: each doorbell for the mouse's endpoint is a
    # `usb_xhci_ep_kick` with the time it was rung, and a mouse being read
    # has one for every movement.
    #
    if not plugs:
        return

    dci = 2 * endpoint + 1
    kicks = []

    try:
        with open(traced) as handle:
            for text in handle:
                found = re.match(r"(\S+)Z usb_xhci_ep_kick slotid \d+, "
                                 r"epid (\d+)", text)

                if found and int(found.group(2)) == dci:
                    stamp = datetime.datetime.strptime(
                        found.group(1), "%Y-%m-%dT%H:%M:%S.%f")
                    kicks.append(stamp.replace(
                        tzinfo=datetime.timezone.utc).timestamp())
    except OSError:
        pass

    for where, at in plugs:
        after = [t for t in kicks if at <= t < at + PLUG_WINDOW]
        gaps = [b - a for a, b in zip(after, after[1:])]
        longest = 1000.0 * max(gaps) if gaps else float("inf")

        check(len(after) >= 20 and longest <= PLUG_GAP_MOST_MS,
              "with a keyboard plugged into %s, the USB mouse's requests in "
              "the %.1f s after were %d, and the longest gap between two was "
              "%.1f ms, more than %.0f: the plug held the mouse (QEMU's trace, "
              "%d of the mouse's requests in all)"
              % (where, PLUG_WINDOW, len(after), longest, PLUG_GAP_MOST_MS,
                 len(kicks)))


def machine_report(image, check):
    """`machine` at the prompt, on a q35 with a drive behind a bridge.

    **This Machine described a q35 on the ThinkPad.** It listed twenty-two
    devices on bus 0, every one "NO DRIVER" - the two xHCI controllers the
    USB driver runs among them - and closed on a paragraph about QEMU's
    bridges and SATA controller. The board's scan walked bus 0 alone, so
    nothing behind a bridge was listed, and it called a device driven only if
    `claimed_here` had been taught its kind: virtio, and one class of sound
    controller.

    **So this machine has the ThinkPad's shape where it matters**: an NVMe
    drive behind a PCI Express root port, on the bus behind it, and two xHCI
    controllers. `machine` is typed once the USB driver has said it runs
    both, because the driver is a process that takes its controllers one
    after the other as it starts, and a report asked for sooner has the
    second undriven - which is what the first run of this check did. Without
    a window manager `machine` prints its report, which is the text its
    window shows.

    **What QEMU has is asked of QEMU**: the vendor and device of every PCI
    function, from `info pci` on a second QEMU, must be what the listing
    holds - which a scan that stops at bus 0 fails by exactly the drive. Then
    the drive driven and off bus 0, both controllers driven, the count
    agreeing with the lines, the screen's source in the words of the boot
    log, and neither of the two things the report said that were never true
    of a ThinkPad.
    """
    disk = os.path.join(tempfile.gettempdir(), "kosmos-x86-machine-nvme.img")

    with open(disk, "wb") as handle:
        handle.truncate(16 * 1024 * 1024)

    extra = ("-device", "ramfb",
             "-device", "pcie-root-port,id=rp0,bus=pcie.0,chassis=1,slot=1",
             "-drive", "file=%s,format=raw,if=none,id=nvme0" % disk,
             "-device", "nvme,drive=nvme0,serial=kosmos,bus=rp0",
             "-device", "qemu-xhci,id=usb0",
             "-device", "qemu-xhci,id=usb1")

    out = boot(image, None, 120.0, typed=("machine",), extra=extra,
               after="xhci: 2 controllers")

    if out is None:
        check(False, "the machine would not boot with a drive behind a bridge")
        return

    parts = out.replace("\r", "").split("kosmos>")
    report = next((p for p in reversed(parts) if "On the bus" in p), "")
    shown = report[report.find("On the bus"):][:3000] or "(no report)"

    listed = re.findall(r"^([0-9a-f]{2}):([0-9a-f]{2})\.([0-7]) +"
                        r"(driven|no driver) +(.*) ([0-9a-f]{6}) "
                        r"\(([0-9a-f]{4}:[0-9a-f]{4})\)$",
                        report, re.MULTILINE)
    driven = sum(1 for l in listed if l[3] == "driven")
    from_qemu = sorted(qemu_pci(extra))

    check(len(from_qemu) > 0 and sorted(l[6] for l in listed) == from_qemu,
          "`machine` did not list every PCI function QEMU has, behind the "
          "root port as well as on bus 0:\n    machine: %r\n    QEMU:    %r"
          % (sorted(l[6] for l in listed), from_qemu))

    drives = [l for l in listed if l[5].startswith("0108")]

    check(len(drives) == 1 and drives[0][0] != "00"
          and drives[0][3] == "driven",
          "the NVMe drive behind the root port was not listed once, off bus 0 "
          "and driven:\n    " + shown)

    controllers = [l for l in listed if l[5] == "0c0330"]

    check(len(controllers) == 2
          and all(l[3] == "driven" for l in controllers),
          "the two xHCI controllers the USB driver took were not both listed "
          "as driven:\n    " + shown)

    counted = re.search(r"(\d+) devices? found, (\d+) driven, (\d+) without "
                        r"a driver\.", report)

    check(counted is not None and int(counted.group(1)) == len(listed)
          and int(counted.group(2)) == driven,
          "the report's count does not agree with its own lines, %d listed "
          "and %d driven: %s" % (len(listed), driven,
                                 counted.group(0) if counted else "no count"))

    frame = re.search(r"^Framebuffer +(.+?) *$", report, re.MULTILINE)

    check(frame is not None and frame.group(1).startswith("ramfb")
          and frame.group(1) in parts[0],
          "the report's framebuffer is %r, not the board's own description of "
          "its screen in the boot log" % (frame.group(1) if frame else None))

    # The sentences themselves, and not "q35": that word is in the machine's
    # own name here, which SMBIOS gives as QEMU's q35.
    stale = [l.strip() for l in report.splitlines()
             if "on a q35" in l or "SMP is being built" in l
             or "no host controller driver" in l]

    check(report != "" and not stale,
          "the report still says what was never true of a ThinkPad:\n    "
          + ("\n    ".join(stale) or "(no report at all)"))


def row(out, label):
    """The value of one of `neofetch`'s rows, or None."""
    found = re.search(r"^%s +(.+?)\r?$" % label, out, re.MULTILINE)

    return found.group(1) if found else None


def identity(image, check):
    """Boots as QEMU and as a ThinkPad, and asks the machine what it is.

    **`neofetch` on a ThinkPad T14 printed two rows about the machine and
    neither had read it.** `Host  QEMU q35 x86-64` was the Makefile's platform
    string, compiled into every PC build. `Network  virtio-net at 0.0.0.0` was
    any answer from the network stack, which has an address of four zero
    bytes and no card on a machine with nothing it can drive.

    Both are reproducible here without the laptop, and that is what makes
    this worth two boots:

      - q35 carries an Intel e1000e unless it is told otherwise, and nothing
        in Kosmos drives one. That is the ThinkPad's shape exactly - an
        Ethernet controller on the bus and no virtio-net.
      - `-smbios type=1` puts any strings QEMU is given into System
        Information, so the second boot *is* a ThinkPad as far as the table
        is concerned. And `smbios-entry-point-type=64` gives it the 3.0 entry
        point a 2021 firmware is likely to use; QEMU's default is 2.1, so
        without it only the older half of `smbios_entry` would ever run.

    The first boot is QEMU saying it is QEMU, which is the half that stops a
    fix from being "print something else".
    """
    out = boot(image, None, 120.0, typed=("neofetch",))

    if out is None:
        check(False, "the machine would not boot to be asked what it is")
        return

    named = next((l.strip() for l in out.splitlines() if "machine:" in l), "")

    check("machine: QEMU Standard PC" in named and "in the BIOS area" in named,
          "the boot log did not name QEMU out of SMBIOS in the BIOS area: "
          + (named or "no machine line"))

    host = row(out, "Host") or ""

    check(host.startswith("QEMU Standard PC") and host.endswith(", x86-64"),
          "neofetch's Host is %r, not the name QEMU's firmware gives" % host)

    network = row(out, "Network") or ""

    check(re.match(r"not driven: Intel 8086:10d3 at 00:[0-9a-f]{2}\.\d$",
                   network) is not None,
          "neofetch's Network is %r on a machine with an undriven e1000e and "
          "no virtio-net" % network)

    extra = ("-machine", "smbios-entry-point-type=64",
             "-smbios", "type=1,manufacturer=LENOVO,product=20W000T9US,"
                        "version=ThinkPad T14 Gen 2i")

    out = boot(image, None, 120.0, typed=("neofetch",), extra=extra)

    if out is None:
        check(False, "the machine would not boot with a ThinkPad's SMBIOS")
        return

    named = next((l.strip() for l in out.splitlines() if "machine:" in l), "")

    check("machine: LENOVO 20W000T9US ThinkPad T14 Gen 2i, from SMBIOS 3."
          in named,
          "given a 3.0 entry point and a ThinkPad's System Information, the "
          "boot log said: " + (named or "no machine line"))

    host = row(out, "Host") or ""

    check(host == "LENOVO 20W000T9US ThinkPad T14 Gen 2i, x86-64",
          "neofetch's Host on the ThinkPad's table is %r" % host)


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

    # 2a. And the USB driver said nothing, because this machine has no USB
    #     controller: it asks, is told there is none, and exits.
    check("xhci:" not in out,
          "a machine with no xHCI controller heard from the USB driver: "
          + next((l.strip() for l in out.splitlines() if "xhci:" in l), ""))

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

    # And a disk that is no drive at all: the image GRUB loads from a USB
    # stick into memory, which is how a machine with nothing it can read
    # carries its own data. `memdisk` says why a drive is attached anyway.
    #
    memdisk(image, check)

    # And USB: two controllers, one stick, and which of them it is on.
    #
    usb(image, check)
    usb_hotplug(image, check)

    # And a USB mouse moving the pointer a TrackPoint moves. `usb_mouse` says
    # why it goes round its ring twice before it clicks.
    #
    usb_mouse(image, check)

    # And what the machine says it is, which it used to read out of the
    # Makefile. `identity` says why QEMU can stand in for the ThinkPad here.
    #
    identity(image, check)

    # And the machine's own report of what is on its bus, which on the
    # ThinkPad listed bus 0 and nothing driven. `machine_report` says why the
    # drive is behind a bridge.
    #
    machine_report(image, check)

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
          "NVMe drive across a reboot, reads one off a disk the loader "
          "handed over in memory, names itself out of SMBIOS as QEMU and "
          "as a ThinkPad, finds a USB stick and a keyboard on two xHCI "
          "controllers and asks the stick what it is through its bulk "
          "endpoints, moves the pointer and clicks with a USB mouse, reads "
          "it through a plug on either controller, and "
          "opens a menu with a click through a PS/2 mouse whether or not "
          "the machine has a serial port)."
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
