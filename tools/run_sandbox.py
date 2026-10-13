#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Sandboxes: Alpine Linux in virtual machines on Kosmos, through `sandbox`.

`docs/virtualization.md` V3 and `docs/sandboxes.md` S1 - Diego, 12 October
2026: "the initial goal for sandboxes is to run a linux distro like alpine",
then "continue with the sandbox app", "as designed in the mockup". The ARM
machine is started at EL2 with the processor's virtualization on and a
`/Home` disk with Alpine installed as an image - `Sandboxes/Images/alpine`,
its kernel and an initramfs of its userland, made by `tools/alpine.py` from
Alpine's published files, held to their sums - and `sandbox` is typed at
the prompt. Held to:

  * **its images**: `sandbox images` names Alpine;
  * **a task**: `sandbox run alpine -- echo $((6*7)); uname -m; exit 3` -
    the command's output, and its exit code, 3, taken from the guest's end
    line (which never reaches the console) and made the program's own; the
    guest booted, ran and switched itself off through PSCI, which needs its
    timer, its interrupt controller and its serial port all working;
  * **a shell**: `sandbox run alpine` reaches a prompt named for the
    sandbox and answers what is typed at Kosmos's console - `cat
    /etc/alpine-release`, 3.24.2 - and `exit` ends it;
  * **a service**: `sandbox new web --service -- <a loop>`, then `start` -
    found in `/Running`, `list`ed as running, its console attached and
    printing, Control-W then D leaving it running, `stop`ped, and `list`ed
    as stopped;
  * **a folder of `/Home`**, by virtio-fs: read only - listed, read, and a
    write refused - and read and write, a file the guest made there read
    back by Kosmos at the prompt;
  * no terminal escape reaching Kosmos's console in any of it.

The screen at the end is kept in `build/gate/arm-sandbox-linux.ppm`.

Usage: run_sandbox.py <image>
"""

import os
import re
import shutil
import subprocess
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot as R                                   # noqa: E402
import scratch                                               # noqa: E402

HERE = os.path.dirname(os.path.abspath(__file__))


def main():
    image = sys.argv[1]
    folder = scratch.path("sandbox-home")
    disk = scratch.path("sandbox-home.img")
    alpine = os.path.join(folder, "Sandboxes", "Images", "alpine")

    shutil.rmtree(folder, ignore_errors=True)
    os.makedirs(alpine)

    # A project to share: a file and a folder in it (S3).
    os.makedirs(os.path.join(folder, "Projects", "site", "pages"))

    with open(os.path.join(folder, "Projects", "site", "hello.txt"), "w") as f:
        f.write("hello from Kosmos\n")

    with open(os.path.join(folder, "Projects", "site", "pages", "one.txt"), "w") as f:
        f.write("page one\n")

    # What `pkg install alpine` leaves beside an image: its manifest, which
    # is where its title comes from (`packages.lua`, `tools/packages.py`).
    with open(os.path.join(alpine, "package.txt"), "w") as f:
        f.write("name        alpine\ntitle       Alpine Linux\nkind        sandbox\n"
                "about       Alpine Linux 3.24.2 for a sandbox\n")

    for step in ([sys.executable, os.path.join(HERE, "alpine.py"), alpine],
                 [sys.executable, os.path.join(HERE, "homeimage.py"), folder, disk, "128"]):
        made = subprocess.run(step, capture_output=True, text=True)

        if made.returncode != 0:
            print("FAIL: %s would not run:\n%s%s" % (os.path.basename(step[1]), made.stdout, made.stderr))
            return 1

    R.virtualize("1G")
    R.extra_args(image, ["-drive", "file=%s,format=raw,if=none,id=home" % disk,
                         "-device", R.device(image, "blk") + ",drive=home"])
    guest = R.Guest(image, 120)
    failed, checks = [], 0

    def check(ok, complaint):
        nonlocal checks
        checks += 1
        if not ok:
            failed.append(complaint)

    def until(text, since, seconds):
        # The serial line ends "\r\n"; what is matched is what is checked.
        end = time.monotonic() + seconds
        while (text not in guest.seen[since:].replace("\r", "")
               and time.monotonic() < end):
            time.sleep(0.3)
            guest._read_available()
        return guest.seen[since:].replace("\r", "")

    def do(command, wait_for, seconds=60):
        at = len(guest.seen)
        guest.type(command)
        return until(wait_for, at, seconds)

    try:
        guest.wait_for("kosmos> ", "reached a prompt")
        start = len(guest.seen)

        # The machine's RAM, asked of QEMU rather than assumed (SMBIOS type
        # 19, `hal/qemu-virt/memory.c`): it was 512 MB whatever `-m` said.
        check(re.search(r"Memory\s+\d+ of 1024 MB used", guest.seen) is not None,
              "a 1 GB machine was not seen as 1024 MB: %r"
              % re.findall(r"Memory\s+[^\n]*", guest.seen)[:1])

        said = do("sandbox images", "\nkosmos> ", 30)
        check(re.search(r"\nalpine\s+Alpine Linux\s+\d+ MB", said) is not None,
              "sandbox images did not name Alpine: %r" % said[-300:])

        said = do("sandbox run alpine -- echo $((6*7)); uname -m; exit 3",
                  "the sandbox is gone", 240)
        check("\n42\n" in said and "\naarch64\n" in said,
              "the task's output did not come back: %r" % said[-600:])
        check(re.search(r"sandbox: echo exited 3 after [\d.]+ s, up in [\d.]+ s; the sandbox is gone",
                        said) is not None and "ended, code 3" in said,
              "the task's exit code, 3, was not the program's: %r" % said[-400:])
        check("kosmos-end-" not in said, "the guest's end line reached the console")

        said = do("sandbox run alpine", ":~# ", 240)
        check(re.search(r"\nshell-\d+:~# ", said) is not None,
              "a shell sandbox's prompt never came: %r" % said[-400:])
        said = do("cat /etc/alpine-release", "\n3.24.2\n", 30)
        check("\n3.24.2\n" in said, "the shell did not answer cat: %r" % said[-300:])
        said = do("exit", "\nkosmos> ", 60)
        check("\nkosmos> " in said, "exit did not end the shell's sandbox: %r" % said[-300:])

        do("sandbox new web --image alpine --service -- while true; do echo tick; sleep 1; done",
           "\nkosmos> ", 30)
        said = do("sandbox start web", "\nkosmos> ", 240)
        check("sandbox: web started" in said, "the service would not start: %r" % said[-300:])
        said = do("sandbox list", "\nkosmos> ", 30)
        check(re.search(r"\nweb\s+alpine\s+running\s+256 MB", said) is not None,
              "sandbox list did not show web running: %r" % said[-400:])

        at = len(guest.seen)
        guest.type("sandbox console web")
        said = until("tick\ntick\n", at, 60)
        check("tick\ntick\n" in said, "the service's console did not come, attached: %r" % said[-300:])
        guest.sendkey("ctrl-w")
        time.sleep(0.3)
        guest.type("d")
        said = until("\nkosmos> ", at, 30)
        check("left web running" in said, "Control-W then D did not leave it running: %r" % said[-300:])

        do("sandbox stop web", "\nkosmos> ", 30)
        time.sleep(3)
        said = do("sandbox list", "\nkosmos> ", 30)
        check(re.search(r"\nweb\s+alpine\s+stopped", said) is not None,
              "web was not stopped: %r" % said[-300:])

        # **A folder of /Home** (S3), by virtio-fs: read only - listed, read,
        # a write refused - then read and write, a file the guest made read
        # back by Kosmos.
        said = do("sandbox run alpine --share /Home/Projects/site:/work:ro -- "
                  "ls -R /work; cat /work/hello.txt; echo x > /work/new.txt",
                  "the sandbox is gone", 240)
        check("/work/pages:\none.txt" in said and "\nhello from Kosmos\n" in said,
              "a read-only folder was not listed and read: %r" % said[-500:])
        check("Read-only file system" in said, "a write to a read-only folder was not refused: %r" % said[-300:])
        said = do("sandbox run alpine --share /Home/Projects/site:/work -- "
                  "echo made in alpine > /work/out.txt; mkdir /work/d; ls /work",
                  "the sandbox is gone", 240)
        check(re.search(r"\nd\s+hello.txt\s+out.txt\s+pages\n", said) is not None,
              "a file and a folder made in a read-write folder were not there: %r" % said[-400:])
        # 120 s, not 30: under the whole gate's load a first `cat` answered
        # after 30 (13 October), with the right bytes.
        said = do("cat /Home/Projects/site/out.txt", "\nkosmos> ", 120)
        check("\nmade in alpine\n" in said, "what the guest wrote did not reach /Home: %r" % said[-300:])

        check("\x1b" not in guest.seen[start:], "a terminal escape reached Kosmos's console")

        with open(os.path.join("build", "gate", "arm-sandbox-linux.ppm"), "wb") as f:
            f.write(guest.screendump())
    finally:
        transcript = guest.seen
        guest.close()

    if failed:
        print("FAIL: %d of %d checks on sandboxes:" % (len(failed), checks))
        for f in failed:
            print("  " + f)
        print("--- the guest said ---\n" + transcript[-4000:])
        return 1

    print("PASS: %d checks on sandboxes (Alpine Linux 3.24.2 at EL2 under Kosmos: its "
          "image listed; a task's output and its exit code, 3; a shell answering what "
          "was typed and ending with exit; a service started, found running, its "
          "console attached and left with Control-W D, and stopped; a folder of "
          "/Home read only and read-write, by virtio-fs)" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
