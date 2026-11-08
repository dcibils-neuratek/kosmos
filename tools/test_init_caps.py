#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**The shell's capabilities, counted three times and held equal.**

init starts the shell with a list of endpoints (`start("the shell", ...)`),
and the kernel hands them over in that order as indices 0, 1, 2 and on. The
shell's own chunk then calls `shell_main(0, 1, 2, ...)`, whose parameters
name them. Three lists that must agree, written in three places, and
nothing that fails when they do not: an index the call leaves out reaches
its parameter as nil, which reads as "this machine has none".

That is what happened on 8 November 2026. `/Models` added a parameter
before `sandboxes_cap` and an endpoint before `SANDBOXES_EP`, and the call
stayed at 0 to 25. `/Models` took 25, `/Sandboxes` was handed nil, and was
never mounted, though its server ran - found by `usb_sandboxes` in the
gate, an hour later, with nothing said by the machine.

Usage: test_init_caps.py [init.lua]
"""

import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
INIT = os.path.join(os.path.dirname(HERE), "user", "init", "init.lua")


def counted(src):
    params = re.search(r"local function shell_main\(([^)]*)\)", src)
    call = re.search(r"\n\s*shell_main\(([\d,\s]*)\)", src)
    listed = re.search(r"\{\s*(CONSOLE_EP,\s*RAMFS_EP,[^}]*)\}", src)

    if not (params and call and listed):
        return None

    names = [p.strip() for p in params.group(1).split(",") if p.strip()]
    indices = [int(i) for i in call.group(1).split(",") if i.strip()]
    # The list has comments in it; only the names count.
    body = re.sub(r"--[^\n]*", "", listed.group(1))
    eps = [e.strip() for e in body.split(",") if e.strip()]
    return names, indices, eps


def main():
    path = sys.argv[1] if len(sys.argv) > 1 else INIT

    with open(path) as f:
        got = counted(f.read())

    if got is None:
        print("FAIL: init.lua no longer has the shell's parameters, its call, or the list "
              "it is started with where this test looks")
        return 1

    names, indices, eps = got
    fails = []

    if indices != list(range(len(indices))):
        fails.append("the call does not pass 0 to %d in order: %r" % (len(indices) - 1, indices))
    if len(indices) != len(names):
        fails.append("shell_main has %d parameters and its call passes %d (the last: %s)"
                     % (len(names), len(indices), names[-1]))
    if len(eps) != len(names):
        fails.append("init starts the shell with %d endpoints and shell_main names %d"
                     % (len(eps), len(names)))

    if fails:
        for f in fails:
            print("FAIL: " + f)
        return 1

    print("PASS: the shell's %d capabilities - init's list, the call, the parameters - agree"
          % len(names))
    return 0


if __name__ == "__main__":
    sys.exit(main())
