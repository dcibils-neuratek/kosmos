#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**The linker Meson's trials run** (`tools/mesa_build.py`): whether a
program that calls a function links is how Meson asks whether a function
exists, and the answer has to be Kosmos's.

A real link of a trial cannot be had: the runtime's objects stand on the
Storage and Network Kits, the linker script's TLS symbols and init's own
`main`, which is the image's and not the trial's. So this links as told,
with what is undefined ignored, and then answers the question itself:
every symbol the trial's own objects leave undefined must be defined by
one of the archives on the line - the runtime's, the C++ runtimes',
libgcc. One that is not fails the link, by name, as a real linker would.

The real linker is `KOSMOS_TRIAL_LD`, which `mesa_build.py` sets for the
architecture it configures.
"""

import os
import subprocess
import sys

LLVM = os.environ.get("KOSMOS_LLVM", "/opt/homebrew/opt/llvm")


def nm(path, *flags):
    r = subprocess.run([LLVM + "/bin/llvm-nm", "-j"] + list(flags) + [path],
                       capture_output=True, text=True)
    return set(line.strip() for line in r.stdout.splitlines()
               if line.strip() and not line.endswith(":"))


def main():
    ld = os.environ.get("KOSMOS_TRIAL_LD")
    if not ld:
        sys.exit("mesa_trial_ld: KOSMOS_TRIAL_LD names the real linker")
    args = sys.argv[1:]
    if "--version" in args or "-v" in args:
        sys.exit(subprocess.run([ld] + args).returncode)

    r = subprocess.run([ld, "--unresolved-symbols=ignore-all"] + args)
    if r.returncode != 0:
        sys.exit(r.returncode)

    objects = [a for a in args if a.endswith((".o", ".obj")) and os.path.exists(a)]
    archives = [a for a in args if a.endswith(".a") and os.path.exists(a)]
    defined = set()
    for path in objects + archives:
        defined |= nm(path, "--defined-only")
    missing = set()
    for path in objects:
        missing |= nm(path, "-u") - defined
    if missing:
        for name in sorted(missing):
            sys.stderr.write("mesa_trial_ld: undefined reference to `%s'\n" % name)
        sys.exit(1)


if __name__ == "__main__":
    main()
