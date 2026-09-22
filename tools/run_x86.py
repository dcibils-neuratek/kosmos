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

import argparse
import datetime
import os
import re
import socket
import struct
import subprocess
import sys
import scratch
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
    disk = scratch.path("x86-nvme.img")

    with open(disk, "wb") as handle:
        handle.truncate(64 * 1024 * 1024)

    extra = ("-drive", "file=%s,format=raw,if=none,id=nvme0" % disk,
             "-device", "nvme,drive=nvme0,serial=kosmos")

    # QEMU's own record of each time the controller is started and stopped,
    # in a file of its own, for the check after this boot.
    started = os.path.join(scratch.directory("nvme-trace"),
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
    work = scratch.directory("infousb")
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
    work = scratch.directory("infopci")
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

    **Step 5b is a stick recovered.** QEMU's stick stalls nothing a driver
    sends it well, so this machine is started with
    `opt/kosmos/stickfault=signature`: the driver sends the first wrapper with
    a wrong signature, the stick stalls it, and the driver has to say so, run
    Reset Recovery and send INQUIRY again - and everything after that, the
    size and the partition table, comes from a stick that was recovered.
    """
    stick = scratch.path("x86-usb-stick.img")
    stick_blocks = stick_with_gpt(stick)

    extra = ("-device", "qemu-xhci,id=usb0",
             "-device", "qemu-xhci,id=usb1",
             "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
             "-device", "usb-storage,bus=usb1.0,drive=stick",
             "-device", "usb-kbd,bus=usb0.0")

    # QEMU's trace of what the controllers were written, in a file of its
    # own: on the serial line its lines would land inside the driver's.
    traced = os.path.join(scratch.directory("xhci-trace"),
                          "writes")

    # The closing line, whatever it counts: "devices named" is never printed
    # by a run that names one device, which then waited out the timeout.
    out = boot(image, None, 90.0,
               extra=extra + ("-fw_cfg",
                              "name=opt/kosmos/stickfault,string=signature",
                              "-trace", "usb_xhci_oper_write",
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

    # Step 5b: the first wrapper spoiled, the stall it earns, Reset Recovery,
    # and INQUIRY sent again - after which every stick line above came from a
    # stick that was recovered.
    spoiled = re.search(r"xhci: " + at + r" port \d+: its first command goes "
                        r"out with a wrong signature", out)
    recovered = re.search(r"xhci: " + at + r" port \d+: the INQUIRY's command "
                          r"failed: Stall Error \(6\), so the stick is reset"
                          r".*?xhci: " + at + r" port \d+: the stick is "
                          r"reset, and the INQUIRY sent again", out, re.S)

    check(spoiled is not None,
          "the driver did not say it spoiled the stick's first command, so "
          "opt/kosmos/stickfault never reached it and nothing was "
          "recovered:\n    " + shown)

    check(recovered is not None,
          "the stick's stall on a wrong signature was not met with Reset "
          "Recovery and INQUIRY sent again:\n    " + shown)

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


def usb_blocks(image, check):
    """**USB step 5d: the block protocol, from a program at the prompt.**

    A stick laid out by `mkusb_image.write_gpt`, on the second controller.
    Once the driver has read that stick's partition table itself, `sticks`
    asks the driver for it through `/dev/blocks`: each unit's size and names,
    the header at block 1, and the entries it points to - which have to be
    the partition `write_gpt` wrote, read through a region the program handed
    over. Then a read of one block past the last, by a two-line program
    written to `/ramfs` and run by its file - the prompt's own Lua has no
    `use`, which is a program's - and the driver has to refuse it by name
    rather than send it to the stick.

    And a write and a flush sent on `/dev/blocks` by a program, the same way:
    both refused as read only (USB step 5e), because the endpoint that writes
    is the disk server's alone. A write that got through would land on this
    check's own stick, which is thrown away.
    """
    stick = scratch.path("x86-usb-blocks.img")
    blocks = stick_with_gpt(stick)

    extra = ("-device", "qemu-xhci,id=usb0",
             "-device", "qemu-xhci,id=usb1",
             "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
             "-device", "usb-storage,bus=usb1.0,drive=stick")

    # `fs.raw` answers more than the reply, and `string.unpack`'s third
    # argument is where to start reading - so the reply is in parentheses.
    out = boot(image, None, 150.0,
               typed=("sticks",
                      'fs.write("/ramfs/past.lua", [[local r = '
                      'use("/lib/blocks.lua").open() print("past:", '
                      'r:read(0, %d, 1)) r:close()]])' % blocks,
                      "/ramfs/past.lua",
                      'fs.write("/ramfs/refused.lua", [[local r = '
                      'use("/lib/blocks.lua").open() local function ask(op) '
                      'return (string.unpack("<I4", (fs.raw("/dev/blocks", '
                      'string.pack("<I4I4I8I4I4", op, 0, 0, 1, r.handle))))) '
                      'end print("refused:", ask(4), ask(6)) r:close()]])',
                      "/ramfs/refused.lua"),
               extra=extra, after="its backup")

    if out is None:
        check(False, "the machine would not boot with a USB stick")
        return

    shown = "\n    ".join(l.strip() for l in out.splitlines()
                           if "unit " in l or "partition" in l
                           or "past:" in l or "refused:" in l
                           or "sticks:" in l)

    unit = re.search(r"unit 0: (\d+) blocks of (\d+) bytes, \"([^\"]*)\" "
                     r"\"([^\"]*)\"", out)

    check(unit is not None
          and unit.group(1, 2, 3, 4) == (str(blocks), "512", "QEMU",
                                         "QEMU HARDDISK"),
          "`sticks` did not say, through /dev/blocks, that unit 0 is %d "
          "blocks of 512 bytes, \"QEMU\" \"QEMU HARDDISK\":\n    %s"
          % (blocks, shown))

    part = re.search(r"partition 1: \"KOSMOS\", blocks 34 to (\d+), type "
                     r"C12A7328-F81F-11D2-BA4B-00A0C93EC93B", out)

    check(part is not None and part.group(1) == str(blocks - 34),
          "`sticks` did not read, through the driver, the partition "
          "`write_gpt` wrote - \"KOSMOS\", blocks 34 to %d, an EFI system "
          "partition:\n    %s" % (blocks - 34, shown))

    check("past:\tnil\tthat block is past the last" in out,
          "a read of block %d, one past the last, was not refused by the "
          "driver as past the last:\n    %s" % (blocks, shown))

    # BLOCK_OP_WRITE and BLOCK_OP_FLUSH, each answered BLOCK_ERR_READ_ONLY.
    check("refused:\t7\t7" in out,
          "a write and a flush sent on /dev/blocks were not both refused as "
          "read only, error 7:\n    %s" % shown)


def usb_diskbench(image, check):
    """**Disk Benchmark on a USB stick's blocks** (storage at full speed).

    The stick `usb_blocks` reads, with `diskbench` pointed at it: listed as
    unit 0 by its names; its two read rows measured through `/dev/blocks` -
    sequential in the largest reads one USB transfer moves, random in 4 KB -
    and both write rows refused by the program itself, because a drive's raw
    blocks are never written. The numbers are QEMU's. What this holds is that
    they arrive, and that the stick is byte for byte what it was.
    """
    import hashlib

    stick = scratch.path("x86-usb-diskbench.img")
    stick_with_gpt(stick)

    with open(stick, "rb") as f:
        before = hashlib.sha256(f.read()).hexdigest()

    extra = ("-device", "qemu-xhci,id=usb0",
             "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
             "-device", "usb-storage,bus=usb0.0,drive=stick")

    out = boot(image, None, 180.0, typed=("diskbench", "diskbench usb 0 1 1"),
               extra=extra, after="its backup")

    if out is None:
        check(False, "the machine would not boot with a USB stick for diskbench")
        return

    lines = out.splitlines()
    shown = "\n    ".join(l.rstrip() for l in lines
                           if "usb 0" in l or " x1" in l or " x8" in l
                           or " x32" in l or "KB reads" in l
                           or "diskbench" in l)

    def row(start):
        return next((l for l in lines if l.startswith(start)), "")

    never = "never: a drive's blocks are not written"
    seq = row("sequential 1 MB x1")
    rnd = row("random 4 KB x1")

    check(re.search(r"usb 0: QEMU QEMU HARDDISK", out) is not None,
          "`diskbench` did not list the stick as usb 0, by the names it "
          "answers INQUIRY with:\n    " + shown)

    speeds = re.findall(r"(\d+\.\d) MB/s", seq)

    check(len(speeds) == 1 and float(speeds[0]) > 0 and never in seq,
          "sequential 1 MB x1 on the stick did not read at a speed above zero "
          "and refuse to write: %r" % seq)

    iops = re.search(r"(\d+) IOPS", rnd)

    check(iops is not None and int(iops.group(1)) > 0 and never in rnd,
          "random 4 KB x1 on the stick did not read a number of IOPS above "
          "zero and refuse to write: %r" % rnd)

    # **And no request waits for the driver's deadline.** `/dev/blocks` was not
    # on the USB driver's wait, so every read there waited out its 50 ms: 17
    # IOPS, the ThinkPad's own number, where `/home` on the same stick model
    # read 938. At 200 a request is answered on its own clock rather than the
    # driver's; past that, QEMU's numbers say nothing.
    check(iops is not None and int(iops.group(1)) >= 200,
          "random 4 KB reads on the stick's blocks came at %s IOPS - a request "
          "on /dev/blocks is waiting for the USB driver's 50 ms deadline: %r"
          % (iops.group(1) if iops else "no", rnd))

    check("in 124 KB reads, the most one USB read moves" in out,
          "`diskbench` did not say its sequential reads are 124 KB, the most "
          "one USB read moves:\n    " + shown)

    with open(stick, "rb") as f:
        after = hashlib.sha256(f.read()).hexdigest()

    check(before == after,
          "the stick changed while `diskbench` measured it, and a drive's "
          "blocks are never written")


# The Kosmos partition's type (USB step 5e), as `user/init/init.lua` has it.
# Three copies: this one, `tools/mkusb_image.py`'s, which `stick_with_home`
# holds to this, and `init.lua`'s, which every check that boots a stick holds
# to both. A stick laid out with a different one is a `/home` never found.
KOSMOS_PARTITION = "8A9DC8A8-83CF-4F7F-962B-43157A68F14A"


def stick_with_home(path, megabytes=16, unique=None):
    """A stick laid out as USB step 5 lays one out: a protective MBR, a GPT at
    both ends, an EFI system partition, and a Kosmos partition after it - left
    blank, because the disk server formats a blank disk the first time it is
    asked, and that is part of what is being checked. Answers the Kosmos
    partition's first and last block.

    `unique` is the Kosmos partition's own GUID, as text, for a check that
    names it as the loader will; otherwise every GUID is a random one."""
    import struct
    import uuid
    import zlib

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkusb_image

    assert mkusb_image.KOSMOS_TYPE_GUID == KOSMOS_PARTITION, \
        "tools/mkusb_image.py and tools/run_x86.py name different Kosmos types"

    sector = mkusb_image.SECTOR
    total = megabytes * 1024 * 1024 // sector
    esp = (2048, 4095)
    home = (4096, total - 34)

    entries = bytearray(128 * 128)

    def entry(i, type_guid, span, name, own=None):
        at = i * 128
        entries[at:at + 16] = mkusb_image.guid_bytes(type_guid)
        entries[at + 16:at + 32] = (uuid.UUID(own) if own
                                    else uuid.uuid4()).bytes_le
        entries[at + 32:at + 48] = struct.pack("<QQ", span[0], span[1])
        label = name.encode("utf-16-le")
        entries[at + 56:at + 128] = label + b"\0" * (72 - len(label))

    entry(0, mkusb_image.ESP_TYPE_GUID, esp, "EFI")
    entry(1, KOSMOS_PARTITION, home, "KOSMOS HOME", unique)

    entries_crc = zlib.crc32(bytes(entries)) & 0xFFFFFFFF
    disk_guid = uuid.uuid4().bytes_le

    def header(mine, other, entries_at):
        h = bytearray(92)
        h[0:8] = b"EFI PART"
        h[8:16] = struct.pack("<II", 0x00010000, 92)
        h[24:56] = struct.pack("<QQQQ", mine, other, 34, total - 34)
        h[56:72] = disk_guid
        h[72:92] = struct.pack("<QIII", entries_at, 128, 128, entries_crc)
        h[16:20] = struct.pack("<I", zlib.crc32(bytes(h)) & 0xFFFFFFFF)
        return bytes(h) + b"\0" * (sector - 92)

    with open(path, "wb") as f:
        f.truncate(total * sector)

        mbr = bytearray(sector)
        mbr[446:462] = struct.pack("<BBBBBBBBII", 0, 0, 2, 0, 0xEE, 0xFF,
                                   0xFF, 0xFF, 1, total - 1)
        mbr[510:512] = b"\x55\xAA"
        f.write(mbr)

        f.seek(1 * sector)
        f.write(header(1, total - 1, 2))
        f.seek(2 * sector)
        f.write(bytes(entries))
        f.seek((total - 33) * sector)
        f.write(bytes(entries))
        f.seek((total - 1) * sector)
        f.write(header(total - 1, 1, total - 33))

    return home


def usb_drives(image, check):
    """**USB step 6b: another machine's FAT volume, at `/drives`.**

    A stick mtools laid out - `tools/fatstick.py` - attached over xHCI, and
    the drive server asked what is on it. **Somebody else's reading of the
    format on both sides**: mtools wrote the volume and `user/servers/
    fat_decode.c` walks it, so agreement is two readings meeting rather than
    one reader agreeing with itself.

    What each check is for, because several of them look alike and are not:

      - **the name is `PHOTOS`**, which is the volume's label. `Untitled` is
        what an unlabelled volume is called (Diego, 16 September), so a
        reader that never found the label would pass a test whose volume had
        none;
      - **`hello.txt` reads back its exact bytes**, which is a file found by
        a short name and read from its one cluster;
      - **the long name appears in a listing**, gathered from its pieces with
        the checksum that ties them to the short entry;
      - **`Italy/roma.txt` resolves**, which is a path walked one directory
        down rather than a root directory scanned;
      - **3000 bytes come back**, and the volume is formatted with one sector
        a cluster, so that file is a chain of six. A reader that returned the
        first cluster and stopped gives 512 and passes everything above it.
    """
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import fatstick

    if not fatstick.available():
        print("SKIP: the /drives phase, because mtools is not installed.")
        return

    stick = scratch.path("x86-fat-stick.img")
    fatstick.build(stick)

    extra = ("-device", "qemu-xhci,id=usb0",
             "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
             "-device", "usb-storage,bus=usb0.0,drive=stick")

    # Written to a file and run, never typed at the prompt: `use` is a global
    # inside a program the loader runs and not in the chunk the shell reads
    # from stdin. And no Lua comments in it - `fs.write` puts the whole thing
    # on one line, so a `--` would comment out everything after it.
    program = (
        'local names, err = fs.list("/drives") '
        'print("drives" .. ": volumes " .. table.concat(names or {}, ",") '
        '.. " err=" .. tostring(err)) '
        'local top = fs.list("/drives/PHOTOS") '
        'print("drives" .. ": top " .. table.concat(top or {}, "|")) '
        'local hello = fs.read("/drives/PHOTOS/hello.txt") '
        'print("drives" .. ": hello " .. tostring(hello)) '
        'local roma = fs.read("/drives/PHOTOS/Italy/roma.txt") '
        'print("drives" .. ": roma " .. tostring(roma)) '
        'local big = fs.read("/drives/PHOTOS/A Long File Name.txt") '
        'print("drives" .. ": big " .. tostring(big and #big or -1)) '
        'local second = fs.list("/drives/BACKUP") '
        'print("drives" .. ": second " .. table.concat(second or {}, "|")) '
        'local notes = fs.read("/drives/BACKUP/notes.txt") '
        'print("drives" .. ": notes " .. tostring(notes)) '
        'local ids = {} '
        'for _, v in ipairs(fs.volumes("/drives") or {}) do '
        'ids[#ids + 1] = v.name .. "=" .. tostring(v.id) end '
        'print("drives" .. ": ids " .. table.concat(ids, ",")) '
        'print("drives" .. ": done")'
    )

    # The Drives app's model, as a program of its own: the one above is
    # typed at the prompt, and a line there is cut at about a kilobyte.
    model = (
        'for _, d in ipairs(use("/lib/drivelist.lua").drives()) do '
        'local v = {} for _, x in ipairs(d.volumes) do v[#v + 1] = x.name end '
        'print("drives" .. ": model " .. d.kind .. "|" .. d.bytes .. "|" '
        '.. table.concat(v, ",") .. "|" .. d.unclaimed) end'
    )

    # **Not `its backup`, which every other USB phase waits for.** That line
    # comes from a stick laid out with a GPT, and this one has an MBR - as a
    # camera or a Windows box writes - so the driver prints "no GUID
    # partition table's header ... and no backup" and the gate never opens.
    # Nothing was typed, and seven checks failed against a program that was
    # never sent. This is the driver's own last line, whatever the stick.
    out = boot(image, None, 180.0,
               typed=("fs.write('/ramfs/d.lua', %r)" % program,
                      "/ramfs/d.lua",
                      "fs.write('/ramfs/m.lua', %r)" % model,
                      "/ramfs/m.lua"),
               extra=extra, after="watching for devices")

    if out is None:
        check(False, "the machine would not boot with a FAT stick")
        return

    lines = [l.strip() for l in out.splitlines() if "drives:" in l]
    shown = "\n    ".join(lines) or "(the drive server said nothing)"

    def said(prefix):
        return next((l for l in lines if l.startswith("drives: " + prefix)), "")

    volumes = said("volumes")

    check("PHOTOS" in volumes,
          "/drives did not list the stick's volume under its label PHOTOS - "
          "a volume with no label is Untitled, so this is the label being "
          "read rather than a default:\n    " + shown)

    top = said("top")

    check("hello.txt" in top,
          "a listing of the volume did not hold hello.txt:\n    " + shown)

    check("A Long File Name.txt" in top,
          "a listing did not hold the long name, gathered from its pieces:"
          "\n    " + shown)

    check("Italy" in top,
          "a listing did not hold the Italy directory:\n    " + shown)

    check("Kosmos reads a drive." in said("hello"),
          "hello.txt did not read back the bytes mtools put in it:"
          "\n    " + shown)

    check("roma" in said("roma"),
          "Italy/roma.txt did not resolve one directory down:\n    " + shown)

    # **Each volume's own identity, as the whole set.** A shortcut in Places
    # is remembered by this, because a unit number is handed out afresh on
    # every replug (`drivesproto.h`). Compared as a dictionary against the
    # serials `fatstick.py` stamped, not by substring: a stride error or an
    # empty id is invisible to `in`, which is what 18.87 was about.
    ids = said("ids")[len("drives: ids "):]
    got = dict(p.split("=", 1) for p in ids.split(",") if "=" in p)

    check(got == fatstick.IDS,
          "/drives did not report each volume's own serial - wanted %r, "
          "got %r:\n    %s" % (fatstick.IDS, got, shown))

    # The chain, which is the part nothing else here can show.
    big = re.search(r"drives: big (-?\d+)", out)

    # **The second volume, which is what a one-volume fixture cannot show.**
    #
    # A listing of `/drives` was answered with 104-byte volume records and
    # decoded as 80-byte entries, so the first name was right - a name is the
    # first 64 bytes of both - and the second read 24 bytes into the middle
    # of the first record. One volume is exactly the case where that is
    # invisible, and this test was green over it.
    # **The exact set, not a substring, and the difference is the whole
    # point.** Decoding two 104-byte volume records at a stride of 80 gives
    # `PHOTOS` and then an *empty* name - the second read lands 24 bytes into
    # the first record, in the middle of its sizes, which trim to nothing. An
    # empty entry is invisible to `"BACKUP" in volumes`, so a substring test
    # passes on corrupted data. The set cannot: a missing volume, an extra
    # blank, or a garbage name all fail here and say what came back.
    found = re.search(r"drives: volumes (\S*) err=", out)
    names = sorted(n for n in (found.group(1).split(",") if found else [])
                   if n != "")

    check(names == ["BACKUP", "PHOTOS"],
          "/drives listed %r where both volumes should be there, exactly. "
          "A stride error decodes the second name as empty, which a "
          "substring test cannot see:\n    %s"
          % (names, shown))

    # And FAT16, whose root directory is a fixed run of sectors rather than a
    # cluster chain - a different walk from everything above.
    check("notes.txt" in said("second"),
          "the FAT16 volume did not list notes.txt from its fixed root "
          "directory:\n    " + shown)

    check("the second volume" in said("notes"),
          "notes.txt on the FAT16 volume did not read back its bytes:"
          "\n    " + shown)

    check(big is not None and int(big.group(1)) == 3000,
          "a 3000-byte file on a volume of one-sector clusters read back %s "
          "bytes - 512 is a reader that returned the first cluster and never "
          "followed the table:\n    %s"
          % (big.group(1) if big else "nothing", shown))

    # **The Drives app's model** (USB step 6e): the stick as one drive of
    # its own size, its two volumes in partition order, and what they do
    # not account for - the stick's 64 MB less their 48 and 12, which is
    # its partition table and the room after them.
    stick_model = [l for l in lines if l.startswith("drives: model USB stick|")]
    want = "drives: model USB stick|%d|PHOTOS,BACKUP|%d" % (
        fatstick.SECTORS * 512, (fatstick.SECTORS - 98304 - 24576) * 512)

    check(stick_model == [want],
          "the Drives app's model did not see one stick of %d bytes holding "
          "PHOTOS then BACKUP - wanted %r, got %r"
          % (fatstick.SECTORS * 512, want, stick_model))


def usb_home(image, check):
    """**USB step 5e: `/home` on a stick's Kosmos partition, across a reboot.**

    A stick with an EFI partition and a blank Kosmos partition, and a machine
    started with `opt/kosmos/home=usb`. On the first boot the disk server finds
    the partition through `/dev/blocks`, formats it because it is blank, and
    `save` writes a file to `/home` through the write endpoint only it holds;
    the second boot is a machine that has never seen the stick, and the file
    has to be there. What a file written and read back in one boot could not
    show: that the blocks went to the stick, and to the right blocks of it.

    And that the save's commit flushed the stick. A flush cannot be seen
    under QEMU, whose stick writes straight to a file, so what is checked is
    the driver's line for a stick's first flush that it kept.
    """
    stick = scratch.path("x86-usb-home.img")
    first, last = stick_with_home(stick)

    extra = ("-device", "qemu-xhci,id=usb0",
             "-device", "qemu-xhci,id=usb1",
             "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
             "-device", "usb-storage,bus=usb1.0,drive=stick",
             "-fw_cfg", "name=opt/kosmos/home,string=usb")

    one = boot(image, None, 150.0,
               typed=("diskinfo", "save notes.txt kept on a stick",
                      'print("origin", sys.info().log_origin > 0 and '
                      'sys.info().log_origin <= sys.ticks())',
                      "log save", "diagnose"),
               extra=extra, after="its backup")

    two = boot(image, None, 150.0,
               typed=("diskinfo", "cat /home/notes.txt"),
               extra=extra, after="its backup")

    if one is None or two is None:
        check(False, "the machine would not boot with a stick for /home")
        return

    def shown(out):
        return "\n    ".join(l.strip() for l in out.splitlines()
                              if "disk:" in l or "filesystem:" in l
                              or "Kosmos partition" in l or "cache" in l
                              or "saved" in l or "kept on a stick" in l
                              or "found at" in l or "look(s)" in l
                              or "log:" in l)

    # The disk server owns no console, so what it found comes back through
    # `/home/.super` and `diskinfo` says it: the partition's own size rather
    # than the stick's, and which partition it is.
    said = ("disk: %d sectors of 512 bytes" % (last - first + 1),
            "on the Kosmos partition on USB unit 0, blocks %d to %d"
            % (first, last))

    check(all(s in one and s in two for s in said),
          "diskinfo did not say /home is the Kosmos partition, blocks %d to "
          "%d - %d sectors - on both boots:\n    %s\n    %s"
          % (first, last, last - first + 1, shown(one), shown(two)))

    check("filesystem: version" in one and "saved notes.txt" in one,
          "the first boot did not format the blank partition and save a file "
          "to /home on it:\n    %s" % shown(one))

    check("the stick wrote out its cache when asked" in one,
          "the first boot's save did not flush the stick - the driver never "
          "said a SYNCHRONIZE CACHE (10) was kept:\n    %s" % shown(one))

    check("filesystem: version" in two
          and "kept on a stick" in two.split("cat /home/notes.txt")[-1],
          "the second boot did not find the file the first saved to /home on "
          "the stick:\n    %s" % shown(two))

    # **What finding the stick took**, said by `diskinfo` in the log's own
    # seconds: the look that found it, and a first look no later than that.
    found = re.search(r"found at (\d+\.\d+) s, by look (\d+); the first look "
                      r"was at (\d+\.\d+) s", one)

    # The counter's reading at the log's zero, which those seconds count from:
    # set, and not after now.
    check(re.search(r"^origin\s+true\s*$", one, re.M) is not None,
          "sys.info().log_origin was not the counter at the log's zero - set, "
          "and no later than sys.ticks()")

    check(found is not None
          and 0.0 < float(found.group(3)) <= float(found.group(1)) < 150.0,
          "diskinfo did not say, in the log's seconds, when the stick was "
          "found and when the first look for it was:\n    %s" % shown(one))

    # **`log save`, and the log off the stick as `make stick-log` takes it**:
    # the Kosmos partition copied out by `sticklog.py` and the file taken from
    # the copy by `kfs.lua` - from the stick the machine wrote it to, so what
    # is checked is the whole way a log reaches the Mac but the raw device.
    # The end of the file is the command that saved it, which a save of part
    # of the ring would not reach.
    tools = os.path.dirname(os.path.abspath(__file__))
    part, log, text = stick + ".home", stick + ".log.txt", ""
    report, diagnosis = stick + ".diagnose.txt", ""

    with open(part, "wb") as out:
        copied = subprocess.run([sys.executable,
                                 os.path.join(tools, "sticklog.py"), stick],
                                stdout=out, stderr=subprocess.PIPE)

    if copied.returncode == 0:
        subprocess.run([os.path.join("build", "host", "lua"),
                        os.path.join(tools, "kfs.lua"), "get", part,
                        "/home/log.txt", log], capture_output=True)

        if os.path.exists(log):
            with open(log, errors="replace") as f:
                text = f.read()

        subprocess.run([os.path.join("build", "host", "lua"),
                        os.path.join(tools, "kfs.lua"), "get", part,
                        "/home/diagnose.txt", report], capture_output=True)

        if os.path.exists(report):
            with open(report, errors="replace") as f:
                diagnosis = f.read()

    check(re.search(r"log: \d+ lines, \d+ KB, saved to /home/log\.txt", one)
          and "saved notes.txt" in text and "xhci:" in text
          and "log save" in text[-200:],
          "`log save` did not put the log on the stick in a form `make "
          "stick-log` reads back - the save said:\n    %s\n  and what came "
          "back was %d bytes ending %r (%s)"
          % (shown(one), len(text), text[-120:],
             copied.stderr.decode("utf-8", "replace").strip()))

    # **`diagnose`, off the same stick**: one file with what a diagnosis of
    # the ThinkPad asks for, instead of photographs - the build, the machine,
    # its devices, the disk as `/home/.super` has it, the sticks, the
    # processes, and the whole log last, so the file ends with the command
    # that wrote it.
    sections = ("== build", "== machine", "== devices", "== disk",
                "== /home", "== sticks", "== processes", "== log")
    missing = [s for s in sections if ("\n%s\n" % s) not in diagnosis]

    check(diagnosis.startswith("Kosmos diagnosis") and not missing
          and "Kosmos partition on USB unit 0" in diagnosis
          and "diagnose" in diagnosis[-200:],
          "`diagnose` did not put a whole diagnosis on the stick - %d bytes, "
          "missing %s, ending %r"
          % (len(diagnosis), ", ".join(missing) or "no section",
             diagnosis[-120:]))


def usb_flush_refused(image, check):
    """**USB step 5e: a stick that does not do SYNCHRONIZE CACHE is told once.**

    The ThinkPad's Kingston answers every SYNCHRONIZE CACHE (10) with ILLEGAL
    REQUEST, 20h/00h, and the driver asked it twice a commit, each time with a
    REQUEST SENSE and a line on the screen. QEMU's stick does every flush, so
    this one's are made to fail: blkdebug fails each flush of the image with
    EINVAL, which QEMU's SCSI disk answers as ILLEGAL REQUEST, 24h/00h.

    The format and three saves are several commits, each flushed twice. The
    driver has to say once that the stick does not do the command and never
    that the stick failed it; the saves have to land; and `diskinfo` has to
    say the cache is not written out, and why.
    """
    stick = scratch.path("x86-usb-no-flush.img")
    rules = stick + ".blkdebug"
    stick_with_home(stick)

    with open(rules, "w") as f:
        f.write('[inject-error]\nevent = "flush_to_disk"\niotype = "flush"\n'
                'errno = "22"\nonce = "off"\n')

    extra = ("-device", "qemu-xhci,id=usb0",
             "-device", "qemu-xhci,id=usb1",
             "-drive", "file=blkdebug:%s:%s,format=raw,if=none,id=stick"
             % (rules, stick),
             "-device", "usb-storage,bus=usb1.0,drive=stick",
             "-fw_cfg", "name=opt/kosmos/home,string=usb")

    out = boot(image, None, 150.0,
               typed=("save a.txt one", "save b.txt two", "save c.txt three",
                      "diskinfo"),
               extra=extra, after="its backup")

    if out is None:
        check(False, "the machine would not boot with a stick whose flushes "
                     "fail")
        return

    lines = [l.strip() for l in out.splitlines()]
    told = [l for l in lines if "does not do SYNCHRONIZE CACHE (10)" in l]
    failed = [l for l in lines
              if "SYNCHRONIZE CACHE (10), which the stick failed" in l]

    check(len(told) == 1 and not failed,
          "a stick that does not do SYNCHRONIZE CACHE was not told once and "
          "left alone - %d line(s) saying it does not, %d saying it failed:"
          "\n    %s" % (len(told), len(failed),
                        "\n    ".join((told + failed)[:6])))

    check(all("saved %s" % name in out for name in ("a.txt", "b.txt", "c.txt")),
          "saves to a stick that does not flush did not land:\n    %s"
          % "\n    ".join(l for l in lines if "save" in l))

    cache = [l for l in lines if "its cache:" in l]

    check(len(cache) == 1
          and "the stick does not do SYNCHRONIZE CACHE" in cache[0],
          "diskinfo did not say why the stick's cache is not written out:"
          "\n    %s" % "\n    ".join(cache or lines[-8:]))


class PluggedMachine:
    """A machine for a check that plugs something in while it runs.

    QEMU with its monitor on a socket, and its serial line read on a thread -
    for `usb_hotplug`'s reason: a guest whose line is not read stops inside
    `kputc` once the pipe is full - and typed at, a line at a time.
    """

    def __init__(self, image, extra):
        binary = os.path.join(os.path.dirname(image), "kosmos.bin")
        work = scratch.directory("x86-plugged")
        path = os.path.join(work, "monitor")
        cmd = ([QEMU] + ARGS + ["-monitor", "unix:%s,server,nowait" % path]
               + list(extra) + ["-kernel", binary])

        self.monitor = None
        self.heard = bytearray()
        self.proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT,
                                     stdin=subprocess.PIPE)
        threading.Thread(target=self._drain, daemon=True).start()

        try:
            self.monitor = Monitor(path)
        except Exception:
            self.close()
            raise

    def _drain(self):
        while True:
            chunk = os.read(self.proc.stdout.fileno(), 65536)

            if not chunk:
                return

            self.heard.extend(chunk)

    def mark(self):
        return len(self.heard)

    def since(self, mark):
        return self.heard[mark:].decode("utf-8", "replace").replace("\r", "")

    def wait_for(self, mark, pattern, seconds):
        until = time.time() + seconds

        while time.time() < until:
            if re.search(pattern, self.since(mark)):
                return True

            time.sleep(0.25)

        return False

    def typed(self, line, pattern, seconds=60.0):
        """A line at the prompt, and everything since, once `pattern` and the
        prompt after it have come."""
        mark = self.mark()
        self.proc.stdin.write(line.encode() + b"\n")
        self.proc.stdin.flush()
        came = self.wait_for(mark, r"(?:%s)[\s\S]*kosmos>" % pattern, seconds)
        time.sleep(0.5)
        return came, self.since(mark)

    def close(self):
        if self.monitor is not None:
            self.monitor.close()
            self.monitor = None

        self.proc.kill()
        self.proc.wait()


def usb_second_stick(image, check):
    """**USB step 5e: a stick plugged in while `/home` is on another.**

    The stick with `/home` on its Kosmos partition is on the second
    controller. A file is saved there; then a second stick, holding a
    partition of its own, is plugged into the first controller through QEMU's
    monitor, and another file is saved.

    **This is the check that found a unit was a position.** A unit was the
    Nth stick ready, counting controllers and then slots, so the stick just
    plugged in became unit 0 and moved `/home`'s stick to 1 - and the disk
    server, which keeps the unit it found its partition on, wrote the second
    file's blocks onto the new stick. So: not one block of the new stick may
    differ from what it held before it went in; the second file has to be in
    `/home` beside the first; and `sticks` has to show `/home`'s stick still
    unit 0 and the new one unit 1, the next number.
    """
    home = scratch.path("x86-usb-home-first.img")
    other = scratch.path("x86-usb-second.img")

    stick_with_home(home)
    stick_with_gpt(other)

    with open(other, "rb") as handle:
        before = handle.read()

    machine = PluggedMachine(image, (
        "-device", "qemu-xhci,id=usb0",
        "-device", "qemu-xhci,id=usb1",
        "-drive", "file=%s,format=raw,if=none,id=home" % home,
        "-device", "usb-storage,bus=usb1.0,drive=home",
        "-drive", "file=%s,format=raw,if=none,id=other" % other,
        "-fw_cfg", "name=opt/kosmos/home,string=usb"))

    try:
        if not (machine.wait_for(0, r"its backup[\s\S]*kosmos>", 150.0)
                or machine.wait_for(0, r"kosmos>[\s\S]*its backup", 1.0)):
            check(False, "the machine never had its prompt and `/home`'s stick "
                         "read:\n    " + repr(machine.since(0)[-600:]))
            return

        time.sleep(1.0)
        _, first = machine.typed("save first.txt kept before",
                                 r"saved first\.txt|save:")

        mark = machine.mark()
        machine.monitor.ask("device_add usb-storage,bus=usb0.0,drive=other,"
                            "id=other")
        plugged = machine.wait_for(mark, r"its backup", 90.0)
        time.sleep(1.0)

        check(plugged, "the stick plugged into the first controller was never "
                       "read by the driver:\n    "
                       + repr(machine.since(mark)[-600:]))

        if not plugged:
            return

        _, listing = machine.typed("sticks", r"unit|sticks:")
        _, second = machine.typed("save second.txt kept after",
                                  r"saved second\.txt|save:")
        _, cat = machine.typed("cat /home/first.txt", r"kept before|cat:")
    finally:
        machine.close()

    with open(other, "rb") as handle:
        after = handle.read()

    changed = [n for n in range(len(before) // 512)
               if before[n * 512:(n + 1) * 512] != after[n * 512:(n + 1) * 512]]

    check(not changed,
          "%d block(s) of the stick plugged in later were written, the first "
          "at block %s - `/home`'s writes followed a unit number to another "
          "stick:\n    %s"
          % (len(changed), changed[0] if changed else "-",
             "\n    ".join(l for l in second.splitlines() if l.strip())))

    check("saved first.txt" in first and "saved second.txt" in second
          and "read back: kept after" in second and "kept before" in cat,
          "the files saved to `/home` before and after the second stick went "
          "in are not both there:\n    %s\n    %s\n    %s"
          % (first.strip(), second.strip(), cat.strip()))

    zero, _, one = listing.partition("unit 1:")

    check("unit 0:" in zero and "\"KOSMOS HOME\"" in zero
          and "partition 1: \"KOSMOS\", blocks 34 to 32734" in one,
          "`sticks` did not show `/home`'s stick as unit 0 and the one "
          "plugged in later as unit 1:\n    %s"
          % "\n    ".join(l for l in listing.splitlines() if l.strip()))


def usb_home_late(image, check):
    """**USB step 5e: a stick named after the machine first asks for `/home`.**

    The shell decides where `/home` is once, as it builds its namespace: on the
    disk server when `/home/.super` answers a filesystem, and in memory when it
    does not. Under QEMU the driver names a stick before the shell starts; on
    the ThinkPad naming a stick takes seconds, and nothing makes init wait for
    the driver. So the disk server waits for the stick its option asked for,
    bounded, until the first time it has looked for as long as it may.

    Here the stick is not in at boot. The machine starts with
    `opt/kosmos/home=usb`, the driver says it is watching, and five seconds
    later the stick goes in through QEMU's monitor. `/home` has to be the
    stick's partition - `diskinfo` says so, and a file saved there has extents
    on a disk - rather than memory, where `diskinfo` finds no `/home/.super`.
    """
    stick = scratch.path("x86-usb-home-late.img")
    first, last = stick_with_home(stick)

    machine = PluggedMachine(image, (
        "-device", "qemu-xhci,id=usb0",
        "-device", "qemu-xhci,id=usb1",
        "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
        "-fw_cfg", "name=opt/kosmos/home,string=usb"))

    try:
        if not machine.wait_for(0, r"xhci: watching for devices plugged in "
                                   r"and out", 90.0):
            check(False, "the USB driver never said it was watching for "
                         "devices:\n    " + repr(machine.since(0)[-600:]))
            return

        time.sleep(5.0)
        mark = machine.mark()
        machine.monitor.ask("device_add usb-storage,bus=usb1.0,drive=stick,"
                            "id=stick")
        plugged = (machine.wait_for(mark, r"its backup", 60.0)
                   and machine.wait_for(0, r"kosmos>", 90.0))

        check(plugged, "the stick plugged in five seconds after the driver "
                       "started was not read, or no prompt came after it:\n    "
                       + repr(machine.since(mark)[-600:]))

        if not plugged:
            return

        time.sleep(1.0)
        _, info = machine.typed("diskinfo", r"filesystem:|diskinfo:|disk:")
        _, saved = machine.typed("save late.txt named late",
                                 r"saved late\.txt|save:")
    finally:
        machine.close()

    said = ("disk: %d sectors of 512 bytes" % (last - first + 1),
            "on the Kosmos partition on USB unit 0, blocks %d to %d"
            % (first, last))

    check(all(s in info for s in said),
          "`/home` was not the Kosmos partition, blocks %d to %d, when its "
          "stick came after the shell had asked - a shell that is told no "
          "filesystem keeps `/home` in memory:\n    %s"
          % (first, last,
             "\n    ".join(l for l in info.splitlines() if l.strip())))

    check(re.search(r"saved late\.txt: \d+ bytes, [1-9]\d* extent", saved)
          is not None,
          "a file saved to `/home` did not land on a disk, with extents:\n    %s"
          % "\n    ".join(l for l in saved.splitlines() if l.strip()))

    # **The wait, counted.** The stick went in five seconds after the driver
    # started watching, so the disk server looked many times, a tenth of a
    # second apart; the looks before it found no stick named; and the one that
    # found it came well after the first - which is what `diskinfo` has to be
    # able to say about the ThinkPad's twenty seconds.
    found = re.search(r"found at (\d+\.\d+) s, by look (\d+); the first look "
                      r"was at (\d+\.\d+) s", info)
    before = re.search(r"(\d+) look\(s\) before it found no stick named yet",
                       info)

    check(found is not None and before is not None
          and int(found.group(2)) >= 10 and int(before.group(1)) >= 9
          and float(found.group(1)) - float(found.group(3)) >= 1.0,
          "diskinfo did not count the disk server's wait for a stick plugged in "
          "late - many looks, the ones before finding no stick named, and the "
          "find well after the first:\n    %s"
          % "\n    ".join(l for l in info.splitlines() if l.strip()))


def usb_home_named(image, check):
    """**USB step 5f: `/home` on the partition the machine was told, and no
    other.**

    Two sticks, each with a Kosmos partition, and a machine started with
    `opt/kosmos/home` naming the second one's unique GUID - in small letters,
    to see that case does not matter - as the loader names the partition on
    the stick it started from. `usb` would take the first stick, unit 0. The
    name has to take unit 1, on the second controller, whose partition is a
    different size so the two cannot be mistaken; and the first stick's
    blocks have to be exactly what they were.
    """
    import uuid

    other = scratch.path("x86-usb-named-a.img")
    stick = scratch.path("x86-usb-named-b.img")
    named = str(uuid.uuid4()).upper()

    stick_with_home(other)
    first, last = stick_with_home(stick, megabytes=24, unique=named)

    with open(other, "rb") as handle:
        before = handle.read()

    extra = ("-device", "qemu-xhci,id=usb0",
             "-device", "qemu-xhci,id=usb1",
             "-drive", "file=%s,format=raw,if=none,id=other" % other,
             "-device", "usb-storage,bus=usb0.0,drive=other",
             "-drive", "file=%s,format=raw,if=none,id=named" % stick,
             "-device", "usb-storage,bus=usb1.0,drive=named",
             "-fw_cfg", "name=opt/kosmos/home,string=%s" % named.lower())

    out = boot(image, None, 150.0,
               typed=("diskinfo", "save named.txt on the named stick"),
               extra=extra, after="its backup")

    if out is None:
        check(False, "the machine would not boot with two Kosmos sticks")
        return

    shown = "\n    ".join(l.strip() for l in out.splitlines()
                           if "disk:" in l or "Kosmos partition" in l
                           or "filesystem:" in l or "saved" in l
                           or "diskinfo:" in l)

    said = ("disk: %d sectors of 512 bytes" % (last - first + 1),
            "on the Kosmos partition on USB unit 1, blocks %d to %d"
            % (first, last))

    check(all(s in out for s in said),
          "`/home` was not the partition named, on unit 1, blocks %d to %d:"
          "\n    %s" % (first, last, shown))

    check(re.search(r"saved named\.txt: \d+ bytes, [1-9]\d* extent", out)
          is not None,
          "a file saved to the named partition did not land on a disk:\n    %s"
          % shown)

    with open(other, "rb") as handle:
        after = handle.read()

    check(after == before,
          "the stick whose partition was not named was written:\n    %s"
          % shown)


def usb_home_large(image, check):
    """**A 512 MB `/home`, read past where the old one ended.**

    Diego, 19 September: "from now on we need to make the drive image at
    least 512mb". The stick's `/home` is a partition read by the USB driver,
    so its size should not matter to anything but the filesystem - and "should
    not" is what this checks. The image is made the way a stick's is,
    `homeimage.py` from a folder, at 512 MB: a 40 MB file first and a 256 KB
    one after it, so the second sits past the 32 MB the old disk ended at -
    found in the image's bytes here, rather than assumed from the order. The
    stick is `mkusb_image.write_gpt`'s, and the kernel is told the partition's
    GUID as the loader tells it.

    Then `/home` has to be all 512 MB - `diskinfo`'s sector count - with the
    free blocks the host's `kfs.lua df` counts in the same image, and the far
    file has to read back: its length, and bytes at its start, middle and
    end.
    """
    import uuid

    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import mkusb_image

    work = scratch.directory("x86-home512")
    folder = os.path.join(work, "home")
    home = os.path.join(work, "home.img")
    esp = os.path.join(work, "esp.img")
    stick = os.path.join(work, "stick.img")
    guid = str(uuid.uuid4()).upper()
    far = bytes(i % 251 for i in range(256 * 1024))
    here = os.path.dirname(os.path.abspath(__file__))
    lua = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(image))),
                       "host", "lua")

    os.makedirs(os.path.join(folder, "videos"))

    with open(os.path.join(folder, "videos", "a-filler.bin"), "wb") as out:
        out.write(b"\x5a" * (40 * 1024 * 1024))

    with open(os.path.join(folder, "videos", "far.bin"), "wb") as out:
        out.write(far)

    made = subprocess.run([sys.executable, os.path.join(here, "homeimage.py"),
                           folder, home, "512"], capture_output=True, text=True)

    if made.returncode != 0:
        check(False, "homeimage.py would not make a 512 MB /home: "
              + made.stdout + made.stderr)
        return

    with open(home, "rb") as handle:
        at = handle.read().find(far[:4096])

    check(at >= 32 * 1024 * 1024,
          "the far file begins at byte %d of the image, not past 32 MB - the "
          "test would not reach where the old disk ended" % at)

    free = subprocess.run([lua, os.path.join(here, "kfs.lua"), "df", home],
                          capture_output=True, text=True).stdout.strip()

    with open(esp, "wb") as handle:
        handle.truncate(1024 * 1024)

    mkusb_image.write_gpt(stick, esp, os.path.getsize(esp), home=home,
                          home_guid=guid)

    out = boot(image, None, 150.0,
               typed=("diskinfo", "df",
                      'local d = fs.read("/home/videos/far.bin") '
                      'print("FAR" .. "FILE", d and #d, d and d:byte(1), '
                      'd and d:byte(131073), d and d:byte(#d))'),
               extra=("-device", "qemu-xhci,id=usb0",
                      "-drive", "file=%s,format=raw,if=none,id=stick" % stick,
                      "-device", "usb-storage,bus=usb0.0,drive=stick",
                      "-fw_cfg", "name=opt/kosmos/home,string=%s" % guid),
               after="its backup")

    if out is None:
        check(False, "the machine would not boot with a 512 MB /home")
        return

    shown = "\n    ".join(l.strip() for l in out.splitlines()
                           if "disk:" in l or "FARFILE" in l
                           or "blocks free" in l or "Kosmos partition" in l)

    check("disk: %d sectors of 512 bytes" % (512 * 2048) in out,
          "/home was not the whole 512 MB partition:\n    " + shown)

    wanted = re.search(r"(\d+) blocks free of (\d+)", free)
    guest = re.findall(r"(\d+) blocks free of (\d+)", out)

    check(wanted is not None and guest
          and guest[-1] == (wanted.group(1), wanted.group(2)),
          "the machine's df and kfs.lua's disagree about the 512 MB /home: "
          "%r and %r" % (guest[-1] if guest else None, free))

    expect = "FARFILE\t%d\t%d\t%d\t%d" % (len(far), far[0], far[131072],
                                          far[-1])

    check(expect in out,
          "the file past 32 MB did not read back as written - wanted %r:"
          "\n    %s" % (expect, shown))


def cmdline_long(image, check):
    """**A command line longer than 256 characters keeps its last word.**

    The loader passes a stick's words and then 117 characters of its own, and
    the kernel kept 255 of them, so a stick with long `KOSMOS_ARGS` lost the
    loader's words from the end without a word said - and from USB step 5f
    those words carry `opt/kosmos/home`. QEMU's `-kernel` fills the same
    buffer through Multiboot 1's `-append`, so a line of 335 characters goes
    in with a word at its very end, and the machine is asked for that word.
    """
    pad = " ".join("opt/kosmos/pad%02d=%s" % (n, "x" * 8) for n in range(12))
    line = pad + " opt/kosmos/tail=reached"

    out = boot(image, None, 60.0,
               typed=('print("tail:", sys.boot("opt/kosmos/tail"))',),
               extra=("-append", line))

    if out is None:
        check(False, "the machine would not boot with a long command line")
        return

    check("tail:\treached" in out,
          "a word at the end of a %d-character command line did not reach "
          "sys.boot:\n    %s" % (len(line), "\n    ".join(
              l.strip() for l in out.splitlines() if "tail" in l)))


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
    work = scratch.directory("x86-hotplug")
    path = os.path.join(work, "monitor")
    stick = os.path.join(work, "stick.img")     # `usb` may be running too

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
    work = scratch.directory()
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



def sound_eapd(image, check):
    """**A pin that controls an amplifier's power has it switched on.**

    EAPD/BTL Enable, section 7.3.3.16 of the HDA specification: bit 1 of verb
    70Ch powers the amplifier a pin feeds. The ThinkPad's speaker and headphone
    pins are EAPD Capable, and nothing set it - the codec took the samples and
    made no sound. QEMU's codec has no such pin, so `opt/kosmos/hdaeapd=1` has
    the driver take its output pin for one.

    **And QEMU's codec keeps no EAPD/BTL register** - it reads back 0 whatever
    was written - so the write is seen where it arrives instead: with
    `debug=1` the codec names every verb it does not handle, and 70Ch is one.
    That has to carry bit 1; sound has to come up; and the pin's line has to
    say what EAPD/BTL reads back. What QEMU cannot say is whether that makes a
    speaker audible; only the ThinkPad can.
    """
    out = boot(image, None, 90.0, extra=(
        "-device", "ich9-intel-hda",
        "-device", "hda-output,audiodev=a0,debug=1",
        "-audiodev", "none,id=a0",
        "-fw_cfg", "name=opt/kosmos/hdaeapd,string=1",
    ))

    if out is None:
        check(False, "the machine would not boot with a pin taken for EAPD")
        return

    wrote = [int(m.group(1), 16) for m in re.finditer(
        r"nid \d+ \(\w+\), verb 0x70c, payload 0x([0-9a-f]+)", out)]

    check(any(w & 0x2 for w in wrote),
          "the pin taken for EAPD was not sent 70Ch with EAPD, bit 1, set: "
          + ("sent " + ", ".join("0x%x" % w for w in wrote) if wrote
             else "70Ch never arrived at the codec"))

    # QEMU's debug lines land in the middle of the kernel's, so the pin's line
    # is looked for in pieces rather than whole.
    check("sound: Intel HDA" in out
          and "the codec drives pin 0x" in out and "EAPD/BTL 0x" in out,
          "sound did not come up with a pin taken for EAPD, or the pin's line "
          "did not say what EAPD/BTL reads back: "
          + "; ".join(l.strip() for l in out.splitlines()
                      if "codec drives" in l or "sound:" in l)[:400])


def sound_slow_codec(image, check):
    """**A codec that is slow after reset is waited for, in milliseconds.**

    The HDA driver gave the codec half a million reads, and on the ThinkPad
    sound came up on one boot and not the next with nothing changed: a fast
    processor finishes those reads in a few milliseconds. QEMU's codec answers
    at once, so `opt/kosmos/hdaslow=150` holds back what it announces and
    answers until 150 ms after the controller leaves reset. The driver has to
    wait that out, bring sound up, and say how long the codec took.
    """
    out = boot(image, None, 90.0, extra=(
        "-device", "ich9-intel-hda",
        "-device", "hda-output,audiodev=a0",
        "-audiodev", "none,id=a0",
        "-fw_cfg", "name=opt/kosmos/hdaslow,string=150",
    ))

    if out is None:
        check(False, "the machine would not boot with a slow HDA codec")
        return

    said = [l.strip() for l in out.splitlines()
            if "codec" in l or "sound" in l]
    took = re.search(r"the codec announced itself (\d+) ms after reset, and "
                     r"answered its root node in (\d+) ms", out)

    check("sound: Intel HDA" in out and took is not None
          and int(took.group(1)) >= 150,
          "a codec held back for 150 ms after reset was not waited for: "
          + ("; ".join(said[:4]) or "nothing was said about sound"))


def sound(image, check):
    """Boots with a real HDA controller, plays a tone, and listens."""
    wav = scratch.path("x86-hda.wav")

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

    # **And where the sound goes, said on a machine that plays it**: the
    # converter and the pin, and the pins' configuration after it. The
    # ThinkPad played with its meter moving and was silent, and this line is
    # what says which pin the codec was playing into.
    check(re.search(r"the codec plays converter 0x[0-9a-f]{2} through pin "
                    r"0x[0-9a-f]{2}", out) is not None
          and re.search(r"codec node 0x[0-9a-f]{2} is a pin, output, pin caps "
                        r"0x[0-9a-f]{8}, config 0x[0-9a-f]{8}", out) is not None,
          "the HDA driver played without saying which converter and pin, and "
          "what the pin is: "
          + "; ".join(l.strip() for l in out.splitlines() if "codec" in l)[:400])

    # **And what the pin was set to, read back**: a pin made to play says what
    # the codec kept of its control, rather than what was written to it.
    check(re.search(r"the codec drives pin 0x[0-9a-f]{2}: control 0x[0-9a-f]{8}",
                    out) is not None,
          "the HDA driver did not say what its output pin was set to: "
          + "; ".join(l.strip() for l in out.splitlines() if "codec" in l)[:400])

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
    work = scratch.directory("x86-pointer")

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


def power_button(image, check):
    """The power button, pressed: a key, the desktop shut down, and S5.

    QEMU's `system_powerdown` is the button - it sets PWRBTN_STS in the
    ICH9's PM1 status, which it does only once the enable is set, as
    `ec.c` sets it after switching to ACPI mode. From there it is the path
    the ThinkPad's button takes: `ec_tick` hears it and queues `KEY_POWER`,
    the window manager takes that key and shuts down as the Deskbar's menu
    does, and `hal_power_off` writes the DSDT's sleep type to the FADT's
    PM1a control - which QEMU answers by exiting. A machine that is still
    running afterwards failed somewhere along that line, and the log says
    where.

    **And no press before the press.** The status is cleared at boot, so a
    button the firmware saw before Kosmos did is not a shutdown the moment
    the desktop comes up; the log is read for one first.
    """
    binary = os.path.join(os.path.dirname(image), "kosmos.bin")
    work = scratch.directory("x86-power")
    path = os.path.join(work, "monitor")
    cmd = [QEMU, "-M", "q35,vmport=off", "-m", "512M", "-no-reboot",
           "-display", "none", "-vga", "none", "-device", "ramfb",
           "-monitor", "unix:%s,server,nowait" % path,
           "-serial", "stdio",
           "-fw_cfg", "name=opt/kosmos/boot,string=wm",
           "-kernel", binary]

    proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT,
                            stdin=subprocess.DEVNULL)
    heard = bytearray()

    def drain():
        while True:
            chunk = os.read(proc.stdout.fileno(), 65536)

            if not chunk:
                return

            heard.extend(chunk)

    threading.Thread(target=drain, daemon=True).start()
    monitor = Monitor(path)

    def said():
        return heard.decode("utf-8", "replace")

    try:
        until = time.time() + 120.0

        while time.time() < until and "wm: window Deskbar at" not in said():
            time.sleep(0.25)

        check("wm: window Deskbar at" in said(),
              "booted into the desktop, the Deskbar never opened a window")

        if "wm: window Deskbar at" not in said():
            return

        check("acpi: switched to ACPI mode" in said(),
              "the machine never switched to ACPI mode, so its power button "
              "is the firmware's")

        # The rest of the desktop settled, and a moment for a stale press to
        # show itself if the status was not cleared.
        time.sleep(4.0)

        check("acpi: the power button\n" not in said().replace("\r", ""),
              "the power button was heard before anybody pressed it")

        monitor.ask("system_powerdown")

        try:
            proc.wait(timeout=30.0)
        except subprocess.TimeoutExpired:
            pass

        time.sleep(0.3)
        text = said().replace("\r", "")

        check("acpi: the power button\n" in text,
              "QEMU pressed the power button and the kernel never heard it")
        check("wm: the power button - shutting down" in text,
              "the kernel heard the power button and the window manager "
              "never took the key")
        check("deskbar: battery" not in text,
              "a machine with no battery to read showed one in the "
              "Deskbar: "
              + next((l for l in text.splitlines() if "deskbar: battery" in l),
                     ""))
        check(proc.poll() is not None,
              "the machine is still running after the power button - S5 "
              "was not entered:\n"
              + "\n".join(l for l in text.splitlines()
                          if "acpi:" in l or "power" in l)[-800:])
    finally:
        monitor.close()
        proc.kill()
        proc.wait()


def battery(image, check):
    """The battery, from the kernel's reading to the Deskbar's words.

    q35 has no embedded controller, so the reading is `opt/kosmos/battery`'s
    - the one input the kernel takes for a test instead of the ThinkPad's
    registers, and says so at boot - and everything above it is what runs on
    the ThinkPad: `hal_battery_read`, `sysinfo`, `/dev/battery` from the
    devices server, and the Deskbar drawing it. The registers themselves are
    `test_batterydecode`'s, on the host.

      at a prompt, 57 and charging: `/dev/battery` says 57, charging, on AC;
      the desktop, 57 and charging: the Deskbar says "57% charging";
      the desktop, 8: the Deskbar says "8%", and says it in red - which is
        the reddish pixels of that screen against the charging one's, the
        battery icon being orange and on both.

    And a machine with no battery shows none: `power_button`'s boot has no
    option, and its Deskbar must say nothing about one.
    """
    binary = os.path.join(os.path.dirname(image), "kosmos.bin")

    #
    # **Reddish, rather than one exact colour**, and the difference is the
    # face. The number is drawn in 0xe04848, and this counted pixels equal
    # to those three bytes - which is every pixel of a glyph only while the
    # glyph is a bitmap, one bit to a pixel. The desktop's face became IBM
    # Plex on 19 September, an outline rasterised with coverage, so a red
    # "8%" is a handful of solid pixels in a cloud of blends and the exact
    # count fell to zero. The check said the low battery was not shown; the
    # screen said it was, in red.
    #
    # What it means is "that number is red", so that is what it asks: a
    # pixel whose red clearly leads its green and blue. The background it
    # sits on is the Deskbar's tab, which is grey, and grey has no lead.
    #
    # **And it is asked as a difference between the two machines**, because
    # the battery *icon* is in the same band and is itself reddish - 79
    # pixels of it, which a fixed threshold read as a low battery on a
    # machine that was charging. The icon is the same on both screens and
    # the label is red on one of them, so what the red costs is exactly the
    # difference, and nothing has to know where the icon ends.
    #
    def reddish(r, g, b):
        return r > 0x90 and r - g > 0x40 and r - b > 0x40

    # QEMU splits an option's value at commas, so a comma inside one is two.
    def option(value):
        return ["-fw_cfg", "name=opt/kosmos/battery,string="
                + value.replace(",", ",,")]

    out = boot(image, None, 90.0,
               typed=('local b = fs.read("/dev/battery") print("BAT" .. "TERY", '
                      'b and b.percent, b and b.state, b and b.on_ac)',),
               extra=option("57,charging"))

    if out is None:
        check(False, "the machine would not boot with a battery option")
        return

    check("ec: the battery is opt/kosmos/battery's, for a test: 57%, charging"
          in out,
          "the kernel did not say the battery reading was the option's")
    check(re.search(r"BATTERY\s+57\s+charging\s+1", out) is not None,
          "/dev/battery did not say 57, charging, on AC: "
          + next((l.strip() for l in out.splitlines()
                  if l.startswith("BATTERY")), "nothing"))

    def desktop(value, label):
        work = scratch.directory("x86-battery")
        path = os.path.join(work, "monitor")
        cmd = [QEMU, "-M", "q35,vmport=off", "-m", "512M", "-no-reboot",
               "-display", "none", "-vga", "none", "-device", "ramfb",
               "-monitor", "unix:%s,server,nowait" % path,
               "-serial", "stdio",
               "-fw_cfg", "name=opt/kosmos/boot,string=wm"] + option(value) \
              + ["-kernel", binary]
        proc = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                                stderr=subprocess.STDOUT,
                                stdin=subprocess.DEVNULL)
        heard = bytearray()

        def drain():
            while True:
                chunk = os.read(proc.stdout.fileno(), 65536)

                if not chunk:
                    return

                heard.extend(chunk)

        threading.Thread(target=drain, daemon=True).start()
        monitor = Monitor(path)
        red = None

        try:
            until = time.time() + 120.0
            want = "deskbar: battery " + label

            while time.time() < until \
                    and want not in heard.decode("utf-8", "replace"):
                time.sleep(0.25)

            said = heard.decode("utf-8", "replace")
            check(want in said,
                  "with opt/kosmos/battery=%s the Deskbar never said %r: %s"
                  % (value, want,
                     next((l.strip() for l in said.splitlines()
                           if "deskbar: battery" in l), "nothing")))

            time.sleep(2.0)
            screen = monitor.screendump(os.path.join(work, "bar.ppm"))

            if screen is not None:
                width, height, px = screen
                red = 0

                # The Deskbar's rows: 32, fixed (`theme.metrics.deskbar`).
                for y in range(0, min(32, height)):
                    for x in range(width // 2, width):
                        o = (y * width + x) * 3

                        if reddish(px[o], px[o + 1], px[o + 2]):
                            red += 1
        finally:
            monitor.close()
            proc.kill()
            proc.wait()

        return red

    quiet = desktop("57,charging", "57% charging")
    low = desktop("8", "8%")

    check(quiet is not None and low is not None,
          "a battery desktop drew nothing to look at: 57%% charging gave "
          "%r and 8%% gave %r" % (quiet, low))

    check(quiet is not None and low is not None and low - quiet >= 12,
          "8%% and discharging drew %r reddish pixels in the bar against "
          "%r while charging - the low battery was not shown as low"
          % (low, quiet))


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
    work = scratch.directory("x86-usbmouse")
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
                    # QEMU writes no fraction at all on a whole second, and
                    # one strptime format raised on it and took the session
                    # down - rarely, since a kick has to land on the second.
                    when = found.group(1)
                    stamp = datetime.datetime.strptime(
                        when, "%Y-%m-%dT%H:%M:%S.%f" if "." in when
                        else "%Y-%m-%dT%H:%M:%S")
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
    disk = scratch.path("x86-machine-nvme.img")

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


def test_ssdt():
    """A table of AML made here, for QEMU to hand the machine with its own.

    `Name (KOSM, 0x2026)`, a string, and a buffer of six thousand bytes, so
    the table is longer than the page `sys.firmware` reads at a time and its
    bytes have to cross a seam to come back whole. `iasl -d` reads it as
    exactly that - checked once on the Mac rather than on every run, since
    nothing in `make test` needs ACPICA.
    """
    def pkglength(after):
        # AML's PkgLength counts its own bytes, so try each size in turn.
        for size in (1, 2, 3, 4):
            total = after + size

            if size == 1 and total < 0x40:
                return bytes([total])

            if size > 1 and total < (1 << (4 + 8 * (size - 1))):
                out, rest = [(size - 1) << 6 | (total & 0x0F)], total >> 4

                for _ in range(size - 1):
                    out.append(rest & 0xFF)
                    rest >>= 8

                return bytes(out)

        raise ValueError(after)

    fill = bytes((i * 7 + 3) & 0xFF for i in range(6000))
    size = b"\x0b" + struct.pack("<H", len(fill))
    body = (b"\x08KOSM\x0b" + struct.pack("<H", 0x2026)
            + b"\x08KSTR\x0dKosmos reads its firmware\x00"
            + b"\x08KBUF\x11" + pkglength(len(size) + len(fill)) + size + fill)
    head = struct.pack("<4sIBB6s8sI4sI", b"SSDT", 36 + len(body), 2, 0,
                       b"KOSMOS", b"KOSMOSTS", 1, b"KSMS", 1)
    table = bytearray(head + body)
    table[9] = (-sum(table)) & 0xFF
    return bytes(table)


def firmware(image, check):
    """The firmware's AML, off the machine and onto this one, byte for byte.

    **The ThinkPad's brightness is set somewhere its DSDT says**, and nobody
    here has read it: `acpi save` puts the DSDT and the SSDTs in `/home/acpi`
    for `make stick-log` to bring to the Mac. This is that path under QEMU,
    with one table whose every byte is known: `-acpitable` hands the machine
    an SSDT made by `test_ssdt`, beside QEMU's own DSDT and SSDTs, and the
    same bytes have to come off the disk. The DSDT, which QEMU makes and this
    does not know, has to be whole: its signature, the length it states and
    the size of the file agreeing, and its bytes summing to zero.

    So the kernel has to have followed the FADT to the DSDT - the one table
    the XSDT does not list - kept the SSDTs from the walk, mapped them once
    the MMU was on, and handed them up a page at a time; and `acpi` has to
    have written them whole.
    """
    here = os.path.dirname(os.path.abspath(__file__))
    lua = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(image))),
                       "host", "lua")
    work = scratch.directory("acpi")
    given = test_ssdt()
    table = os.path.join(work, "kosmos.aml")
    disk = os.path.join(work, "home.img")

    with open(table, "wb") as handle:
        handle.write(given)

    made = subprocess.run([lua, os.path.join(here, "kfs.lua"), "create", disk,
                           "16"], capture_output=True, text=True)

    if made.returncode != 0:
        check(False, "kfs.lua could not make the disk for the tables: "
              + (made.stderr or made.stdout).strip())
        return

    extra = ("-drive", "file=%s,format=raw,if=none,id=nvme0" % disk,
             "-device", "nvme,drive=nvme0,serial=kosmos",
             "-acpitable", "file=" + table)

    out = boot(image, None, 120.0, extra=extra, typed=("acpi", "acpi save"))

    if out is None:
        check(False, "the machine would not boot with a table of the test's")
        return

    fact = re.search(r"firmware tables: a DSDT of (\d+) bytes and (\d+) SSDTs?",
                     out)

    check(fact is not None,
          "the boot log did not say it had a DSDT to hand up: "
          + next((l.strip() for l in out.splitlines()
                  if "firmware tables" in l), "no firmware line at all"))

    listed = re.search(r"^DSDT +(\d+) bytes +\S+ +\S+ +sums to zero",
                       out, re.MULTILINE)
    ours = re.search(r"^(SSDT\d+) +(\d+) bytes +KOSMOS +KOSMOSTS +sums to "
                     r"zero", out, re.MULTILINE)

    check(listed is not None and fact is not None
          and listed.group(1) == fact.group(1),
          "`acpi` did not list a whole DSDT the size the boot log gave:\n"
          + "\n".join(l for l in out.splitlines()
                      if l.startswith(("DSDT", "SSDT", "acpi:"))))

    check(ours is not None and int(ours.group(2)) == len(given),
          "`acpi` did not list the test's own table, whole, at %d bytes:\n%s"
          % (len(given), "\n".join(l for l in out.splitlines()
                                   if l.startswith(("SSDT", "acpi:")))))

    check(re.search(r"acpi: \d+ tables? saved to /home/acpi", out) is not None,
          "`acpi save` did not say it saved the tables: "
          + next((l.strip() for l in out.splitlines()
                  if l.startswith("acpi:") and "saved" in l),
                 "nothing about saving"))

    # And off the disk, as `make stick-log FILE=/home/acpi/` takes them.
    folder = os.path.join(work, "acpi")
    os.makedirs(folder)
    subprocess.run([lua, os.path.join(here, "kfs.lua"), "getdir", disk,
                    "/home/acpi", folder], capture_output=True)

    def read(name):
        try:
            with open(os.path.join(folder, name), "rb") as handle:
                return handle.read()
        except OSError:
            return b""

    dsdt = read("DSDT.aml")
    stated = struct.unpack("<I", dsdt[4:8])[0] if len(dsdt) >= 8 else 0

    check(dsdt[:4] == b"DSDT" and stated == len(dsdt) > 36
          and sum(dsdt) & 0xFF == 0
          and (fact is None or len(dsdt) == int(fact.group(1))),
          "the DSDT off the disk is not whole: %d bytes, %r, stating %d, "
          "summing to %d" % (len(dsdt), dsdt[:4], stated, sum(dsdt) & 0xFF))

    back = read(ours.group(1) + ".aml") if ours else b""

    check(back == given,
          "the test's table off the disk is not the table QEMU was given: "
          "%d bytes of %d, the first difference at %s"
          % (len(back), len(given),
             next((i for i, (a, b) in enumerate(zip(back, given)) if a != b),
                  "the end of the shorter")))


def core(image, check, fails):
    """The machine itself: it boots through twelve stages, names its
    processor, agrees with userland about the memory, takes what is typed,
    runs a program, finds four processors and spreads work over them, and
    takes either interrupt controller.

    What main() did inline before the parts; an early exit is a failed check
    that stops the part, as it stopped the whole before.
    """
    # 1. It boots, all the way, and takes what is typed at it.
    out = boot(image, None, 90.0, typed=("mem", "cpu"))

    if out is None:
        check(False, "the machine would not boot")
        return

    if "kosmos>" not in out:
        print("FAIL: x86-64 never reached a prompt.")

        for line in out.splitlines():
            if ("PANIC" in line or "could not start" in line
                    or "process died" in line):
                print("  the machine said: " + line.strip())

        print("  last of what it did say: " + repr(out[-400:]))
        check(False, "x86-64 never reached a prompt")
        return

    check(True, "")

    if "PANIC" in out:
        check(False, "it panicked: "
              + [l for l in out.splitlines() if "PANIC" in l][0].strip())
        return

    # 2. Nothing refused to start on the way there. A prompt can appear with
    #    a server missing, and a shell talking to servers that are not there
    #    is not a working machine.
    for line in out.splitlines():
        if "could not start" in line:
            check(False, "reached a prompt, but: " + line.strip())
            return

    check(True, "")

    # 2a. And the USB driver said nothing, because this machine has no USB
    #     controller: it asks, is told there is none, and exits.
    check("xhci:" not in out,
          "a machine with no xHCI controller heard from the USB driver: "
          + next((l.strip() for l in out.splitlines() if "xhci:" in l), ""))

    # 2b. And the backlight driver said there was nothing to read - q35 has
    #     an Intel network card at 00:02.0, 8086:10d3, where Intel's graphics
    #     would be, and its class and its 32-bit BAR must each tell the two
    #     apart. Found as graphics, the driver would have read a network
    #     card's registers.
    check("backlight: no Intel graphics on this machine" in out,
          "the backlight driver did not say this machine has no Intel "
          "graphics: "
          + next((l.strip() for l in out.splitlines() if "backlight" in l),
                 "nothing from it at all"))

    # 2c. And ACPI mode (`hal/pc/ec.c`): q35's ICH9 puts its SCI on 9 and
    #     its SMI command port at B2h, SeaBIOS leaves SCI_EN clear because
    #     switching is an OS's to do, and writing the FADT's 02h to B2h is
    #     the switch - QEMU sets SCI_EN at once, as the ICH9 does. Then the
    #     power button is a key, nothing answers at 66h, where a laptop's
    #     embedded controller would be, and S5 is the DSDT's `\_S5`, sleep
    #     type 0 at PM1a control 604h - the two numbers `power.c` wrote as
    #     constants before it read them.
    said = "\n".join(l.strip() for l in out.splitlines()
                     if l.startswith(("ec: ", "acpi: ")))

    check("acpi: the FADT: SCI 9, SMI command port 0xb2 (0x02 enables ACPI)"
          in said
          and "acpi: switched to ACPI mode - 0x02 to port 0xb2" in said
          and "acpi: the 8253 still counts" in said
          and "acpi: the power button is a key now" in said
          and "at 0x66 - nothing answers there" in said
          and "acpi: S5 is sleep type 0, from the DSDT's \\_S5, written to "
              "PM1a control at 0x0604" in said,
          "the machine did not switch to ACPI mode, take the power button, "
          "look for an embedded controller and find S5, all as q35 has "
          "them:\n" + (said or "nothing from acpi.c or ec.c at all"))

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
        check(False, "the machine would not boot to run a program")
        return

    check("Hello from a process of my own." in ran,
          "the fw_cfg boot option did not run a program")

    # What it prints is its own capability list, asked of the namespace - so
    # this is IPC and the servers rather than a string in the image.
    for path in ("/bin", "/dev", "/home", "/lib"):
        check(path in ran, "a process could not see %s" % path)

    check("process died" not in ran, "the program faulted on its way out")

    # Nothing below is worth booting for when the machine cannot do that.
    if fails:
        return

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



def usb_ethernet(image, check):
    """A USB Ethernet adapter, named, configured, and moving frames.

    `usb.md` steps 7a, 7b and 7c.

    QEMU's `usb-net` is a CDC Ethernet device with two configurations,
    RNDIS first and CDC-ECM second. That is the shape of Diego's RTL8153,
    whose first configuration is Realtek's own interface and whose second
    is ECM (`roadmap.md` 5m) - so a driver that reads only a device's first
    configuration says nothing here, as it said nothing about the adapter
    on the ThinkPad on 21 September.

    **The MAC address is this machine's choice**, handed to QEMU, and has to
    come back out of the string descriptor that the adapter's Ethernet
    Networking descriptor names: the walk of every configuration, the ECM
    function's two interfaces, and a string read in the adapter's own
    language, end to end. The endpoints are the ones QEMU 11.1.1's `usb-net`
    offered when this was written, as the driver read them; the setting is 1
    because setting 0 of an ECM Data interface has none (ECM 1.2 3.3).

    **And then it is driven** (7b): the three endpoints handed to the
    controller, the configuration chosen, the Data interface taken off that
    empty setting 0, and the packet filter *accepted* - which is a request
    QEMU's adapter answers, so a driver that asked for the wrong thing is
    told so here rather than on the ThinkPad.

    **The setting in that line is the device's own answer.** Everything else
    in the sequence fails loudly when it fails; an interface left on setting
    0 behaves exactly like one that was set until a frame is expected, which
    is 7c. So the driver asks GET_INTERFACE and prints what comes back, and a
    driver that never sent SET_INTERFACE prints a 0 here.

    **And then frames move** (7c). `opt/kosmos/ethprobe` asks the driver for
    two ARP requests, and QEMU's user networking answers both from its
    gateway at 10.0.2.2, whose MAC is 52:55: and the address's four bytes.

    **Two requests, because the second one is the zero-length packet's
    test.** The first is 60 bytes, Ethernet's shortest, which no endpoint's
    packet size divides; the second is padded to a multiple of the bulk OUT
    endpoint's packet - 64 here - which is exactly the length ECM 1.2 3.3.1
    wants a zero-length packet after, since otherwise the adapter is still
    waiting for the rest of a frame that has already ended. Without that rule
    one answer comes back instead of two, which is a test rather than a
    specification quoted in a comment.

    **The link arrives with the traffic.** QEMU's adapter sent no
    notification at all through forty seconds of an idle link; with frames
    moving, NETWORK_CONNECTION comes. That is an observation and not a
    mechanism - what it means for the driver is only that the read is
    outstanding and nothing waits on it.

    **The comma in the option is doubled** on the command line: QEMU's own
    option parser takes a single one as the end of the value.
    """
    mac = "52:54:00:4b:4d:53"
    extra = ("-device", "qemu-xhci,id=usb0",
             "-netdev", "user,id=usbnet",
             "-device", "usb-net,bus=usb0.0,netdev=usbnet,mac=" + mac,
             "-fw_cfg",
             "name=opt/kosmos/ethprobe,string=10.0.2.15,,10.0.2.2")

    out = boot(image, None, 90.0, extra=extra, until="plugged in, ")

    if out is None:
        check(False, "the machine would not boot with a USB Ethernet adapter")
        return

    said = [l[l.index("xhci:"):].strip()
            for l in out.replace("\r", "").splitlines() if "xhci:" in l]
    shown = "\n    ".join(said) or "(the driver said nothing)"

    named = re.search(r"xhci: \S+ port \d+: USB Ethernet, CDC-ECM, in "
                      r"configuration (\d+): MAC ([0-9a-f:]{17}), frames up "
                      r"to (\d+) bytes", out)

    check(named is not None,
          "the driver did not name QEMU's USB Ethernet adapter as CDC-ECM:"
          "\n    " + shown)

    if named is not None:
        check(named.group(2) == mac,
              "the adapter's MAC came back as %s, not the %s QEMU was given"
              % (named.group(2), mac))
        check(named.group(1) == "1",
              "the adapter's ECM function was in configuration %s, not 1, "
              "which is QEMU's CDC one" % named.group(1))
        check(named.group(3) == "1514",
              "the adapter's largest frame was %s bytes, not 1514"
              % named.group(3))

    where = re.search(r"its frames on interface (\d+) setting (\d+), bulk "
                      r"IN (\d+) and OUT (\d+) of (\d+) bytes; its link on "
                      r"interrupt IN (\d+)", out)

    check(where is not None
          and where.groups() == ("1", "1", "2", "2", "64", "1"),
          "the adapter's frames were not on interface 1 setting 1, bulk 2 "
          "each way of 64 bytes, with its link on interrupt IN 1:\n    "
          + shown)

    #
    # And driven: 7b. The setting is named in the line because picking the
    # wrong one is the mistake ECM is known for, and the filter is named
    # because the adapter answered the request rather than stalling it.
    #
    driven = re.search(r"xhci: \S+ port \d+: configured, on setting (\d+) as it "
                       r"says itself, taking frames addressed to it, broadcast "
                       r"and multicast; listening for its link", out)

    check(driven is not None,
          "the adapter was named and not configured. 7b chooses the "
          "configuration, takes the Data interface off setting 0 - which has "
          "no endpoints, so no frame would ever arrive - and asks for the "
          "frames this machine wants:\n    " + shown)

    if driven is not None:
        check(driven.group(1) == "1",
              "the adapter says it is on setting %s. Setting 0 of an ECM Data "
              "interface has no endpoints at all (ECM 1.2 3.3), and leaving "
              "it there is why a correct-looking driver never receives a "
              "frame - which is why the number in that line is the device's "
              "own answer to GET_INTERFACE rather than what it was told."
              % driven.group(1))

    check("is not driven" not in out and "no frame would ever arrive" not in out,
          "a request the adapter is configured with was refused:\n    "
          + shown)

    #
    # And frames: 7c. Both requests out, and both answered by the gateway.
    #
    asked = re.search(r"xhci: \S+ port \d+: asking who has 10\.0\.2\.2, twice - "
                      r"(\d+) bytes and (\d+) -", out)

    check(asked is not None and asked.groups() == ("60", "64"),
          "the driver did not send two ARP requests of 60 and 64 bytes when "
          "opt/kosmos/ethprobe asked for them:\n    " + shown)

    replies = len(re.findall(r"xhci: \S+ port \d+: a frame of \d+ bytes from "
                             r"52:55:0a:00:02:02, type 0806 \(ARP\)", out))

    check(replies >= 1,
          "no ARP reply came back from QEMU's gateway at 10.0.2.2, whose MAC "
          "is 52:55: and the address's four bytes. A frame goes out on the "
          "bulk OUT endpoint and comes back on the bulk IN; if the Data "
          "interface is on setting 0 there are no endpoints to move it "
          "on:\n    " + shown)

    check(replies >= 2,
          "only %d ARP reply came back and two requests went out. The second "
          "is 64 bytes, an exact multiple of the endpoint's packet, and ECM "
          "1.2 3.3.1 wants a zero-length packet after one of those - without "
          "it the adapter is still waiting for the rest of a frame that has "
          "already ended, and never sends it:\n    %s" % (replies, shown))

    check("its link is up" in out,
          "the adapter never said its link was up. QEMU's `usb-net` sends no "
          "notification at all while nothing moves, and sends "
          "NETWORK_CONNECTION once frames do - so this is downstream of the "
          "probe above rather than a claim of its own:\n    " + shown)


PARTS = ["core"] + ['sound', 'sound_slow_codec', 'sound_eapd', 'storage', 'memdisk', 'usb', 'usb_blocks', 'usb_diskbench', 'usb_home', 'usb_second_stick', 'usb_home_late', 'usb_home_named', 'usb_home_large', 'usb_drives', 'usb_flush_refused', 'cmdline_long', 'usb_hotplug', 'usb_mouse', 'usb_ethernet', 'identity', 'firmware', 'machine_report', 'pointer', 'power_button', 'battery']


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("image", nargs="?", default="build/x86_64/kosmos.elf")
    parser.add_argument("--parts", default="",
                        help="parts to run, by name, separated by commas: "
                             + ", ".join(PARTS))
    args = parser.parse_args()
    image = args.image
    wanted = [w for w in args.parts.split(",") if w] or PARTS

    for w in wanted:
        if w not in PARTS:
            print("FAIL: no part called %s; there are %s"
                  % (w, ", ".join(PARTS)))
            return 1

    checks = 0
    fails = []

    def check(ok, complaint):
        nonlocal checks

        if ok:
            checks += 1
        else:
            fails.append(complaint)

    if "core" in wanted:
        core(image, check, fails)

    # And the sound, which is the one subsystem this board does not take
    # from virtio. `hal/pc/hda.c` says why an emulated Intel controller is
    # worth more here than an emulated virtio one: it is the same silicon
    # interface a ThinkPad has, so what passes here is what will run there.
    #
    if 'sound' in wanted:
        sound(image, check)
    if 'sound_slow_codec' in wanted:
        sound_slow_codec(image, check)
    if 'sound_eapd' in wanted:
        sound_eapd(image, check)

    # And the disk, which is the other thing this board does not take from
    # virtio. A ThinkPad's storage is NVMe or nothing - `docs/thinkpad.md`
    # has the table - so the driver that has to work there is the one
    # exercised here, and virtio-blk stays the ARM board's disk so that
    # neither is orphaned. The same argument as the sound controller above,
    # made a second time.
    #
    if 'storage' in wanted:
        storage(image, check)

    # And a disk that is no drive at all: the image GRUB loads from a USB
    # stick into memory, which is how a machine with nothing it can read
    # carries its own data. `memdisk` says why a drive is attached anyway.
    #
    if 'memdisk' in wanted:
        memdisk(image, check)

    # And USB: two controllers, one stick, and which of them it is on.
    #
    if 'usb' in wanted:
        usb(image, check)
    if 'usb_blocks' in wanted:
        usb_blocks(image, check)
    if 'usb_diskbench' in wanted:
        usb_diskbench(image, check)
    if 'usb_home' in wanted:
        usb_home(image, check)
    if 'usb_second_stick' in wanted:
        usb_second_stick(image, check)
    if 'usb_home_late' in wanted:
        usb_home_late(image, check)
    if 'usb_home_named' in wanted:
        usb_home_named(image, check)
    if 'usb_home_large' in wanted:
        usb_home_large(image, check)
    if 'usb_drives' in wanted:
        usb_drives(image, check)
    if 'usb_flush_refused' in wanted:
        usb_flush_refused(image, check)
    if 'cmdline_long' in wanted:
        cmdline_long(image, check)
    if 'usb_hotplug' in wanted:
        usb_hotplug(image, check)

    # And a USB mouse moving the pointer a TrackPoint moves. `usb_mouse` says
    # why it goes round its ring twice before it clicks.
    #
    if 'usb_mouse' in wanted:
        usb_mouse(image, check)
    if 'usb_ethernet' in wanted:
        usb_ethernet(image, check)

    # And what the machine says it is, which it used to read out of the
    # Makefile. `identity` says why QEMU can stand in for the ThinkPad here.
    #
    if 'identity' in wanted:
        identity(image, check)

    # And the firmware's AML, onto this Mac as the ThinkPad's will come -
    # which is where its brightness is set. `firmware` says how it is known
    # to be whole.
    #
    if 'firmware' in wanted:
        firmware(image, check)

    # And the machine's own report of what is on its bus, which on the
    # ThinkPad listed bus 0 and nothing driven. `machine_report` says why the
    # drive is behind a bridge.
    #
    if 'machine_report' in wanted:
        machine_report(image, check)

    # And the pointer a laptop has, with and without the serial port it does
    # not have. `pointer` says why the second half is the one that matters.
    #
    if 'pointer' in wanted:
        pointer(image, check)

    # And the power button, which ACPI mode makes the system's to answer.
    if 'power_button' in wanted:
        power_button(image, check)

    # And the battery, from the kernel's reading to the Deskbar.
    if 'battery' in wanted:
        battery(image, check)

    partial = "" if wanted == PARTS else " (%s)" % ", ".join(wanted)

    if fails:
        print("FAIL: %d of %d checks on x86-64%s:"
              % (len(fails), len(fails) + checks, partial))

        for f in fails:
            print("  " + f)

        return 1

    if partial:
        print("PASS: %d checks on x86-64%s." % (checks, partial))
        return 0

    print("PASS: %d checks on x86-64 (it boots through twelve stages, names "
          "its processor out of CPUID, agrees with userland about the memory "
          "by two paths, answers what is typed at it, runs a program that "
          "reports what it was handed, plays a tone an Intel HDA "
          "controller hands back at the right pitch, keeps a file on an "
          "NVMe drive across a reboot, reads one off a disk the loader "
          "handed over in memory, names itself out of SMBIOS as QEMU and "
          "as a ThinkPad, saves the firmware's AML to a disk byte for byte, "
          "finds a USB stick and a keyboard on two xHCI "
          "controllers and asks the stick what it is through its bulk "
          "endpoints, moves the pointer and clicks with a USB mouse, reads "
          "it through a plug on either controller, reads another machine's FAT32 volume at /drives - its label, its long names, a file one directory down and a chain of clusters - and "
          "opens a menu with a click through a PS/2 mouse whether or not "
          "the machine has a serial port)."
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
