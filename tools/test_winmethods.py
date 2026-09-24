#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Every method an application calls on its window is one the kit has.

Diego, on the ThinkCentre M700 running 0.10.146: *"the tracker closed on me a
couple of times"*. Its Find button was `win:focus(search)`, and the window
keeps `focus` as an *index* into `root:focusables()` - a number. So the call
was `(5)(win, search)`, and pressing Find ended Tracker every time.

**Nothing static could catch it and nothing did.** Lua resolves a method at
the moment of the call, so the file parses, `luac -p` is happy, the window
opens, and the button sits there looking exactly like the others until
somebody presses it. `luaglobals.py` did not see it either: `win` is a local
and `focus` is a field, so no global is involved.

Worse, the bug was invited. The field is called `focus`, which is the name a
method would have, and Tracker already had a working `focus_on(v)` of its own
under a comment saying `win:focus(v)` does not exist. Two callers of one idea
in one file, one right and one wrong.

The kit has `window:focus_on(view)` now, and this is the check that the class
cannot come back: **for every `win:name(...)` in the userland, `ui.lua`
defines `function window:name`** - or the file itself defines `function
win:name`, which is how an application supplies its own `on_key`, `on_frame`
and the rest.

It is a heuristic in one respect and deliberately so: it trusts that a local
called `win` or `window` is a window. That is the convention every
application here follows, and a file that breaks it can be named in `SKIP`
with a reason.

**And a drawing context is left the way it was entered.** `g:push` returns
what `g:pop` needs to restore, and a `g:pop()` with nothing in it is the same
shape of bug as the first: it parses, the window opens, and the program ends
the first time that view paints - which on 24 September was Processes, as
the whole of its window, black. So a `pop` on a context (`g` or `gc`) has to
be given something, and a `push` on one has to be kept.
"""

import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

KIT = os.path.join(ROOT, "user", "lib", "ui.lua")
LOOK_IN = [os.path.join(ROOT, "user", "bin"),
           os.path.join(ROOT, "user", "lib")]

# A receiver whose methods this checks. Anything else is somebody's own
# object and none of this file's business.
RECEIVER = re.compile(r"\b(win|window)\s*:\s*([A-Za-z_][A-Za-z_0-9]*)\s*\(")

# `function win:name(` and `function window:name(` - an application giving
# its window a handler of its own, which is how `on_key` and `on_frame` are
# supplied and is not a call at all.
DEFINES = re.compile(r"\bfunction\s+(?:win|window)\s*:\s*([A-Za-z_][A-Za-z_0-9]*)")

# A drawing context's `pop` with nothing to restore, and a `push` whose
# answer is thrown away - the statement starts with it rather than assigning
# it.
BARE_POP = re.compile(r"\b(?:g|gc)\s*:\s*pop\s*\(\s*\)")
LOST_PUSH = re.compile(r"^\s*(?:g|gc)\s*:\s*push\s*\(", re.M)

# `ui.lua`'s own, which is the list of what a window can do.
KIT_DEFINES = re.compile(r"^function\s+window\s*:\s*([A-Za-z_][A-Za-z_0-9]*)",
                         re.M)

SKIP = {
    # The kit defines the methods; it is not a caller of them.
    os.path.join("user", "lib", "ui.lua"),
}


def uncommented(text):
    """The file with its comments gone, so a name in prose is not a call.

    Long comments first - `--[[ ... ]]`, which `ui.lua` and every application
    here use for the block above a function - then `--` to the end of a line.

    The line pass counts quotes before the `--`, because a Lua string may
    hold one: `emit("cd: " .. p)` has no comment in it and `-- see LICENSE`
    is all comment. An odd number of quotes to the left means the `--` is
    inside a string and stays.
    """
    text = re.sub(r"--\[(=*)\[.*?\]\1\]", "", text, flags=re.S)

    out = []

    for line in text.split("\n"):
        at = 0

        while True:
            at = line.find("--", at)

            if at < 0:
                out.append(line)
                break

            before = line[:at]

            if (before.count('"') - before.count('\\"')) % 2 == 0 and \
               (before.count("'") - before.count("\\'")) % 2 == 0:
                out.append(before)
                break

            at += 2

    return "\n".join(out)


def main():
    with open(KIT, encoding="utf-8") as f:
        kit_source = f.read()

    known = set(KIT_DEFINES.findall(uncommented(kit_source)))

    if "add" not in known or "run" not in known:
        print("test_winmethods: ui.lua's window methods were not found at all "
              "- `function window:add` and `function window:run` are both "
              "missing, so the scan is reading something it does not "
              "understand.", file=sys.stderr)
        return 1

    checks = 0
    bad = []
    contexts = []

    for where in LOOK_IN:
        for folder, _, names in os.walk(where):
            for name in sorted(names):
                if not name.endswith(".lua"):
                    continue

                path = os.path.join(folder, name)
                rel = os.path.relpath(path, ROOT)

                if rel in SKIP:
                    continue

                with open(path, encoding="utf-8") as f:
                    source = uncommented(f.read())

                # What this file gives its own window, which is as good as
                # the kit having it.
                mine = set(DEFINES.findall(source))

                for hit in RECEIVER.finditer(source):
                    method = hit.group(2)
                    checks += 1

                    if method in known or method in mine:
                        continue

                    line = 1 + source[:hit.start()].count("\n")
                    bad.append((rel, line, method))

                for pattern, what in ((BARE_POP, "pop"), (LOST_PUSH, "push")):
                    for hit in pattern.finditer(source):
                        line = 1 + source[:hit.start()].count("\n")
                        contexts.append((rel, line, what))

    if contexts:
        print("test_winmethods: FAIL", file=sys.stderr)

        for rel, line, what in contexts:
            print("  %s:%d %s" % (rel, line,
                  "calls g:pop() with nothing to restore - it takes what "
                  "g:push returned" if what == "pop" else
                  "calls g:push and throws away what g:pop will need"),
                  file=sys.stderr)

        return 1

    if bad:
        print("test_winmethods: FAIL", file=sys.stderr)

        for rel, line, method in bad:
            print("  %s:%d calls win:%s(), which `ui.lua` does not define "
                  "and the file does not supply. Lua resolves a method at "
                  "the call, so this is a window that opens and a control "
                  "that ends the program when it is used."
                  % (rel, line, method), file=sys.stderr)

        print("  %d of %d calls" % (len(bad), checks), file=sys.stderr)
        return 1

    print("PASS: %d calls on a window, every method one the kit defines or "
          "the file supplies (%d in `ui.lua`), and no drawing context left "
          "without what it was entered with" % (checks, len(known)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
