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
import struct
import subprocess
import sys
import tempfile
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

        check(lines >= periods,
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
    kern = re.search(r"(\d+) MB of RAM in (\d+) pages", out)
    user = re.search(r"(\d+) MB of RAM at 0x([0-9a-f]+), in (\d+) pages", out)

    check(kern is not None, "the boot log did not report the memory")
    check(user is not None, "`mem` at the prompt printed nothing usable")

    if kern and user:
        kmb, kn = int(kern.group(1)), int(kern.group(2))
        umb, un = int(user.group(1)), int(user.group(3))

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

    # And it does not claim to be *using* them. Counting is not starting:
    # `cpu_on.c` refuses until there is a local APIC, and there is not.
    check(smp is not None and "1 of them given new threads" in smp,
          "the machine claimed more than one processor is scheduling, and "
          "nothing can start a second one yet")

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

        used = re.search(r"(\d+) MB of RAM in \d+ pages", big)

        check(used is not None and 700 < int(used.group(1)) < 768,
              "the usable memory is not the region below the device window, "
              "so the board chose a block on the far side of the PCI hole")

    # And the sound, which is the one subsystem this board does not take
    # from virtio. `hal/pc/hda.c` says why an emulated Intel controller is
    # worth more here than an emulated virtio one: it is the same silicon
    # interface a ThinkPad has, so what passes here is what will run there.
    #
    sound(image, check)

    if fails:
        print("FAIL: %d of %d checks on x86-64:"
              % (len(fails), len(fails) + checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on x86-64 (it boots through twelve stages, names "
          "its processor out of CPUID, agrees with userland about the memory "
          "by two paths, answers what is typed at it, runs a program that "
          "reports what it was handed, and plays a tone an Intel HDA "
          "controller hands back at the right pitch)." % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
