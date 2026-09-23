#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Servers and drivers are in the right directories, and the rule is one line.

Diego, 23 September 2026, splitting `user/servers/` when it reached 28 files:
"a driver drives hardware". `/drives` reads FAT off blocks somebody else
fetched and is therefore a server; `e1000` owns a card and is not.

**A rule nobody enforces is a rule that drifts**, and this one is unusually
easy to drift: a driver and a server are the same kind of process here - an
EL0 program with an endpoint and a role number - so nothing about building or
spawning one would complain if a new driver landed beside `binfs.c`. The next
person adding one has no reason to know, because the directory would build.

So the rule is mechanical. "Drives hardware" is not a matter of opinion: it
is the three primitives `docs/drivers.md` §4 names, and a process either asks
for them or it does not.

  - No file in `user/servers/` claims an interrupt, maps a device's
    registers, or asks the board where a device is.
  - Every directory under `user/drivers/` holds something that does. A kind
    with nothing driving anything is a directory that should not exist yet.

**And the language line, which drifts the same way.** `user/lib/` is Lua and
`user/kits/` is C - `glossary.md`: *a library is a kit's position, in Lua*.
They were one directory until 23 September, and Diego asked the question
that ends the argument: "why jpeg.c is in the same directory as clock.lua?".
A `.c` under `user/lib/` or a `.lua` under `user/kits/` is that directory
growing back together.
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# The three primitives, in the names a userland driver calls them by.
# `kosmos_mem_phys` is deliberately not here: a bus address is also what a
# process needs to hand a buffer to something else, and `drives` may one day
# want one without becoming a driver.
HARDWARE = ("kosmos_irq_claim", "kosmos_dev_map", "kosmos_dev_find")


def uses_hardware(path):
    with open(path, encoding="utf-8") as f:
        text = f.read()

    return [name for name in HARDWARE if name in text]


def main():
    fails = []
    checks = 0

    servers = os.path.join(ROOT, "user", "servers")

    for name in sorted(os.listdir(servers)):
        if not name.endswith(".c"):
            continue

        used = uses_hardware(os.path.join(servers, name))
        checks += 1

        if used:
            fails.append(
                "user/servers/%s calls %s, so it drives hardware and belongs "
                "under user/drivers/<kind>/ (docs/drivers.md 4b)"
                % (name, ", ".join(used)))

    drivers = os.path.join(ROOT, "user", "drivers")

    if not os.path.isdir(drivers):
        fails.append("there is no user/drivers/ at all")
    else:
        kinds = sorted(d for d in os.listdir(drivers)
                       if os.path.isdir(os.path.join(drivers, d)))

        checks += 1

        if not kinds:
            fails.append("user/drivers/ has no device kinds under it")

        for kind in kinds:
            here = os.path.join(drivers, kind)
            driving = [n for n in sorted(os.listdir(here))
                       if n.endswith(".c")
                       and uses_hardware(os.path.join(here, n))]
            checks += 1

            if not driving:
                fails.append(
                    "user/drivers/%s/ holds no file that claims an interrupt, "
                    "maps registers or asks the board for a device - so it is "
                    "a kind with no driver in it" % kind)

    #
    # Lua on one side of the line, C on the other.
    #
    for where, wanted, wrong in (
            (os.path.join("user", "lib"), ".lua", (".c", ".h")),
            (os.path.join("user", "kits"), None, (".lua",))):
        root = os.path.join(ROOT, where)

        for base, _dirs, names in os.walk(root):
            # Vendored data keeps whatever its author shipped.
            if "solar" in base:
                continue

            for name in sorted(names):
                if not name.endswith(wrong):
                    continue

                checks += 1
                fails.append(
                    "%s holds %s, and that directory is %s (docs/glossary.md: "
                    "a kit is C that runs inside your process, a library is "
                    "the same position in Lua)"
                    % (where, name, "Lua" if wanted == ".lua" else "C"))

        checks += 1

    #
    # A kit is reusable. An app's own C is the app's.
    #
    # Diego, 23 September 2026, finding `user/kits/doom/`: "a kit is a
    # reusable piece of code that an app, service, or server can leverage and
    # reuse. doom is an specific app. so all the doom specific code should
    # live in a doom specific directory".
    #
    # Checked as a name collision because that is what the mistake looks
    # like every time: `/kits/doom` existed beside `doom.lua`, and so did
    # quake, snes, litexl and web. A kit that shares its name with an app is
    # a binding to one engine wearing a general word - it can have no second
    # caller, which is the whole of what makes something a kit.
    #
    # Caller-counting was tried first and is the wrong test: `/kits/game` has
    # one caller today and is a rasterizer written to have many.
    #
    apps = os.path.join(ROOT, "user", "bin", "apps")

    if os.path.isdir(drivers) and os.path.isdir(apps):
        named = {n for n in os.listdir(apps)
                 if os.path.isdir(os.path.join(apps, n))}

        for kind in sorted(os.listdir(os.path.join(ROOT, "user", "kits"))):
            checks += 1

            if kind in named:
                fails.append(
                    "user/kits/%s/ has the same name as the app "
                    "user/bin/apps/%s/, so it is that app's own code rather "
                    "than a kit - a kit is reusable (docs/glossary.md)"
                    % (kind, kind))

    if fails:
        print("FAIL: %d of %d checks on where servers and drivers live:"
              % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on where servers and drivers live, on this machine "
          "(no server drives hardware, and every device kind has a driver)."
          % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
