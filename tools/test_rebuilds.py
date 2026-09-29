#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A header changing rebuilds everything that read it.

On 29 September Quake died on the M700 every time it started: its glue,
`quake_kosmos.c.o`, had been compiled before `struct sysinfo` grew, and
nothing rebuilt it - its `.d` file named `kernel/syscall.h`, and the Makefile
read only the userland's own list of them, never an installed application's.
The x86 kernel had the same hole a different way: one compile of every
source, whose rule named none of their headers (`testing.md` 18.281).

**Every object says what it read** - `-MMD` writes a `.d` beside it - so the
test needs no list of its own to go stale: it asks make, without building
anything (`make -n -W kernel/syscall.h`), what it would do were that header
new, and every object whose `.d` names the header has to be in the answer.
For the gate's builds, which exist by the time this runs: the ARM kernel and
its test userland, every installed application on both boards, and the x86
kernel, which has one `.d` for its one compile.

`kernel/syscall.h` because it is the header the machine and every program
share - the syscalls' numbers and the structures they fill - and because a
stale copy of it is exactly the fault that shipped.

Usage: test_rebuilds.py
"""

import glob
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

HEADER = "kernel/syscall.h"

passed, failed = 0, 0


def check(ok, what):
    global passed, failed

    if ok:
        passed += 1
    else:
        failed += 1
        print("  FAIL: " + what)


def would_run_with(header, *words):
    """What make would run were `header` new, as one string."""
    done = subprocess.run(["make", "--no-print-directory", "-n", "-W", header]
                          + list(words), cwd=ROOT, capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


def would_run(*words):
    return would_run_with(HEADER, *words)


def headers_in(path):
    """The headers a `.d` file names."""
    with open(path, errors="replace") as f:
        return set(re.findall(r"(\S+\.h)\b", f.read()))


def kernel_only(kernel_d, user_dir):
    """A header of the kernel's own, read by no userland object - or None."""
    path = os.path.join(ROOT, kernel_d)

    if not os.path.exists(path):
        return None

    theirs = set()

    for d in glob.glob(os.path.join(ROOT, user_dir, "**", "*.d"), recursive=True):
        theirs |= headers_in(d)

    mine = sorted(h for h in headers_in(path) - theirs
                  if h.startswith("kernel/") and os.path.exists(os.path.join(ROOT, h)))
    return mine[0] if mine else None


left_behind = 0


def readers(directory):
    """Every object under `directory` whose `.d` names HEADER.

    Only objects whose source is still there: a source moved or removed
    leaves its object and its `.d` in `build/` - the kits' old places in
    `user/lib/`, the fixture blobs that became generated code - and nothing
    builds those any more. They are counted, and said.
    """
    global left_behind
    out = set()

    for d in glob.glob(os.path.join(ROOT, directory, "**", "*.d"), recursive=True):
        with open(d, errors="replace") as f:
            text = f.read()

        # The first rule is the object's; `-MP` adds an empty one per header.
        target, _, prereqs = text.partition(":")

        if re.search(r"(^|\s)" + re.escape(HEADER) + r"(\s|\\|$)", prereqs.split("\n\n")[0]):
            target = target.strip()
            source = target[len(directory) + 1:-len(".o")]

            if os.path.exists(os.path.join(ROOT, source)):
                out.add(target)
            else:
                left_behind += 1

    return out


def compiled(text):
    """The objects a dry run would compile - each `-o` of a `.o`."""
    return set(re.findall(r"-o\s+(\S+\.o)\b", text))


def objects(label, directory, *words):
    """Everything under `directory` that read HEADER is rebuilt by `words`."""
    wanted = readers(directory)

    if not wanted:
        check(False, "%s: no object in %s says it read %s - is it built?"
                     % (label, directory, HEADER))
        return 0

    code, text = would_run(*words)
    missing = sorted(wanted - compiled(text))

    check(code == 0, "%s: `make -n -W %s %s` ran: %s"
                     % (label, HEADER, " ".join(words), text[-400:]))
    check(not missing, "%s: %d of the %d objects that read %s would not be "
                       "rebuilt, among them %s"
                       % (label, len(missing), len(wanted), HEADER,
                          ", ".join(missing[:4])))
    return len(wanted)


def main():
    counted = 0
    apps = ("apptest.elf", "doom.elf", "quake.elf", "snes.elf")

    # The ARM kernel and its test userland, and each application against it.
    counted += objects("the ARM kernel", "build/test", "TEST=1", "build/test/kosmos.elf")
    counted += objects("ARM's applications", "build/user-test", "TEST=1", "FULL=0",
                       "MEGA=", *["build/user-test/apps/" + a for a in apps])

    have_x86 = subprocess.run(["which", "x86_64-elf-gcc"],
                              capture_output=True).returncode == 0

    if have_x86:
        counted += objects("x86-64's applications", "build/user-x86_64-test",
                           "ARCH=x86_64", "TEST=1", "FULL=0", "MEGA=",
                           *["build/user-x86_64-test/apps/" + a for a in apps])

        # One compile, and one `.d` for it: the link line is the rebuild.
        #
        # **With a header only the kernel reads.** The kernel carries the
        # userland's image, and the userland reads `syscall.h` too, so a
        # change to that one rebuilt this kernel through the image even with
        # no `.d` of its own - which is how the control that took it away
        # passed. A header in the kernel's `.d` and in no userland's is the
        # kernel's own rule or nothing.
        own = kernel_only("build/x86_64-test/kosmos.bin.d", "build/user-x86_64-test")
        check(own is not None, "no header only the x86-64 kernel reads - is "
                               "build/x86_64-test/kosmos.bin.d there?")

        # Asked of make's database rather than of a dry run: the kernel's
        # sources include `version.c`, whose rule is FORCE, and a dry run
        # counts a FORCE target as remade - so it showed the kernel rebuilt
        # for any header at all, and the control passed again.
        if own:
            target = "build/x86_64-test/kosmos.bin"
            done = subprocess.run(["make", "--no-print-directory", "-p", "-q",
                                   "ARCH=x86_64", "TEST=1", target], cwd=ROOT,
                                  capture_output=True, text=True)
            needs = set()

            for line in done.stdout.splitlines():
                if line.startswith(target + ":"):
                    needs.update(line.split(":", 1)[1].split())

            check(own in needs, "the x86-64 kernel does not depend on %s, a "
                                "header only it reads: %d prerequisites known"
                                % (own, len(needs)))

    if failed:
        print("FAIL: %d of %d checks on what a header rebuilds."
              % (failed, passed + failed))
        return 1

    print("PASS: %d checks on what a header rebuilds (every one of %d objects "
          "whose own dependencies name %s - the ARM kernel, both userlands, "
          "Doom, Quake and the Super Nintendo on both boards - rebuilt when "
          "it changes, and the x86-64 kernel when a header of its own does; "
          "%d left behind by sources since "
          "moved or removed, not counted)." % (passed, counted, HEADER, left_behind))
    return 0


if __name__ == "__main__":
    sys.exit(main())
