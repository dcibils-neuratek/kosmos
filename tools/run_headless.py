#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Boots a machine with no display and checks it still gets a shell.

Every other runner here attaches `-device ramfb`, because every other runner
is about pixels. That left one shape of machine untested: the one with no
display at all - which is `make serial`, and which is also a real board with
nothing plugged into it.

It was broken, and silently. init asked the kernel for the screen grant
whether or not there was a screen; the kernel refuses a grant it cannot
give; the refused spawn was the shell, so the machine booted through all
twelve stages, printed nothing wrong, and stopped at a prompt that never
appeared. The identical mistake had already been made and fixed for the
disk, four lines higher in the same function, with a comment explaining it.

So this is the test that would have caught it: no display, and the machine
still has to reach a prompt and still has to run a program.
"""

import os
import subprocess
import sys
import time

QEMU = "qemu-system-aarch64"

# The Makefile's serial line, which is the point: this is what a person gets
# from `make serial`, not a shape invented for the test.
ARGS = [
    "-M", "virt,gic-version=3",
    "-cpu", "cortex-a72",
    "-m", "512M",
    "-nographic",
    "-global", "virtio-mmio.force-legacy=false",
    # Deliberately no `-device ramfb`. That absence is the whole test.
]


def boot(image, boot_option, want, timeout):
    """Runs until `want` appears, and returns everything printed."""
    cmd = [QEMU] + ARGS

    if boot_option:
        cmd += ["-fw_cfg", "name=opt/kosmos/boot,string=" + boot_option]

    cmd += ["-kernel", image]

    p = subprocess.Popen(cmd, stdout=subprocess.PIPE,
                         stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL)
    os.set_blocking(p.stdout.fileno(), False)

    out = b""
    start = time.time()

    try:
        while time.time() - start < timeout:
            chunk = p.stdout.read()

            if chunk:
                out += chunk
            else:
                time.sleep(0.05)

            if want in out:
                # A moment more, so a line that arrives just after the one
                # being waited for is in the output rather than cut off.
                time.sleep(0.4)
                out += p.stdout.read() or b""
                break
    finally:
        p.kill()
        p.wait()

    return out.decode("utf-8", "replace")


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0

    # 1. It reaches a prompt.
    out = boot(image, None, b"kosmos>", 60.0)

    if "kosmos>" not in out:
        print("FAIL: a machine with no display never reached a prompt.")

        for line in out.splitlines():
            if "could not start" in line or "PANIC" in line:
                print("  the machine said: " + line.strip())

        return 1

    checks += 1

    # 2. Nothing refused to start on the way there. The prompt can appear
    #    with a server missing, and a shell talking to servers that are not
    #    there is not a working machine.
    for line in out.splitlines():
        if "could not start" in line:
            print("FAIL: reached a prompt, but: " + line.strip())
            return 1

    checks += 1

    # 3. And a program runs. Reaching a prompt only proves the shell was
    #    spawned; a program is spawned through a different path, which asks
    #    for the same grant and got it wrong in the same way.
    out = boot(image, "hello", b"kosmos>", 60.0)

    if "could not start a process for it" in out:
        print("FAIL: the prompt is there but no program will start:")
        print("  " + [l for l in out.splitlines()
                      if "could not start" in l][0].strip())
        return 1

    if "Hello from a process of my own." not in out:
        print("FAIL: `hello` started but printed nothing recognisable:")
        print("  " + repr(out[-200:]))
        return 1

    checks += 2

    # 4. And `/bin` reports every program in it.
    #
    #    Counted on this side from `user/bin/*.lua` and on that side from
    #    the namespace, which is what makes it a check rather than a
    #    restatement: two independent counts of the same thing.
    #
    #    It exists because they disagreed. A listing reply holds 74 names
    #    and the image had 82 programs, so `fs.list("/bin")` answered with
    #    the first 74 and said nothing - and the Deskbar, which builds its
    #    menu from that list, could not offer Tracker, the Terminal, the top
    #    bar or the web server. No error anywhere; the menu was just short,
    #    and had been since the seventy-fifth program was added.
    here = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(
        __file__))), "user", "bin")
    expected = len([f for f in os.listdir(here) if f.endswith(".lua")])

    #
    # `ls /bin`, because the boot option runs a *program* rather than
    # evaluating Lua - which is what `boot=wm` means and is the one way a
    # program starts here. `ls` prints one entry a line, so counting them is
    # counting lines that name a program.
    #
    out = boot(image, "ls /bin", b"kosmos>", 60.0)
    # The *first* field of each line: `ls` prints "  name  size  kind", so
    # a line ends in its kind and not in the name it is reporting.
    said = len([l for l in out.splitlines()
                if l.split() and l.split()[0].endswith(".lua")])

    if said == 0:
        print("FAIL: /bin would not list at all:")
        print("  " + repr(out[-300:]))
        return 1

    if said != expected:
        print("FAIL: /bin lists %d programs and there are %d."
              % (said, expected))
        print("  A listing that stops early stops silently, and the Deskbar")
        print("  builds its menu from it - so applications go missing with")
        print("  no error. See BIN_OP_LIST in user/servers/binfs.c.")
        return 1

    checks += 1

    print("PASS: %d checks on a machine with no display (it reaches a "
          "prompt, every server started, a program runs, and /bin lists "
          "all %d of them)." % (checks, expected))
    return 0


if __name__ == "__main__":
    sys.exit(main())
