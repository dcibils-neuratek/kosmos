#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Lite XL on the machine: an editor that opens a file, edits it and saves it.

`make test` checks the port's C shim and its host arithmetic on this computer,
and loads its Lua with the modules stubbed. None of that is the editor: a
window, keys arriving in the order a person pressed them, and a file that says
afterwards what was typed into it. This is.

**Checked through the file, not the screen.** What was typed is read back at
the prompt, after the desktop has stopped, from the file Lite XL saved - so a
pass means the keys arrived, the document took them, `io.open` wrote them
through the namespace and the file server kept them. A picture of text on a
screen would pass with the save quietly failing.

**Control-N is checked by what the editor did**, from `--trace`, which prints
every command Lite XL runs. That is the check that would have caught the
queue losing a Control release: the key reached the window and the command
never ran.

**The title is checked by what the window manager said**, which is every
rename: the file's name when it opens, marked `*` once it is edited, and
plain again once it is saved. The window manager could always rename a
window; for a long time the editor never asked, and nothing here noticed.

Only for an image built with `make LITEXL=1`, which the ordinary image is not,
so this is `make litexl-check`, which `make prepush` runs, rather than a phase
of `make test`.

Usage: run_litexl.py [image]
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

from run_screenshot import Guest, Failure, PROMPT      # noqa: E402

# Starting the editor is tens of thousands of lines of Lua under TCG, and
# nothing prints when it is ready to take keys. Measured well inside this.
SETTLE = 40

# QEMU's names for the keys that are not letters.
KEY = {"/": "slash", ".": "dot", " ": "spc", "-": "minus"}


def soak(guest, seconds):
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        guest._read_available()
        time.sleep(0.5)


def said_after(guest, mark, text, seconds):
    """Whether `text` appears in what the machine said after `mark`."""
    end = time.monotonic() + seconds
    while time.monotonic() < end:
        guest._read_available()
        if text in guest.seen[mark:]:
            return True
        time.sleep(0.3)
    return False


def windows(said):
    """The window manager's lines about windows, which say every rename."""
    return "\n".join(line for line in said.splitlines()
                     if "wm: window " in line) or "(none)"


def keys(guest, text):
    """Types `text` through QEMU's own keyboard, one key at a time."""
    for ch in text:
        guest.sendkey(KEY.get(ch, ch))


def start(guest, spec):
    """`wm` with Lite XL, and a window, and an editor that did not fail."""
    mark = len(guest.seen)
    guest.type("wm " + spec)

    if not said_after(guest, mark, "wm: window Lite XL", 60):
        raise Failure("Lite XL opened no window:\n" + guest.seen[mark:][-1500:])

    soak(guest, SETTLE)

    said = guest.seen[mark:]

    for bad in ("litexl: start.lua", "litexl: require core", "litexl: core.init",
                "litexl: no window", "this image was not built"):
        if bad in said:
            raise Failure(f"Lite XL did not start ({bad!r}):\n{said[-1500:]}")

    return said


def stop_and_read(guest, path):
    """Control-C stops the desktop; then the file, as the prompt sees it."""
    mark = len(guest.seen)
    guest.sendkey("ctrl-c")

    if not said_after(guest, mark, PROMPT, 30):
        raise Failure("Control-C did not stop the desktop.")

    command = "head -n 3 " + path
    mark = len(guest.seen)
    guest.type(command)
    said_after(guest, mark, PROMPT, 20)

    return guest.seen[mark:].split(command, 1)[-1]


def edit_a_file(image):
    guest = Guest(image, 300)
    checks = 0

    try:
        guest.wait_for(PROMPT, "reached a shell")

        mark = len(guest.seen)
        guest.type('fs.write("/home/notes.txt", "first line\\n")')

        if not said_after(guest, mark, PROMPT, 20):
            raise Failure("the prompt did not come back after making the file.")

        said = start(guest, "litexl:/home/notes.txt")
        checks += 2                          # a window, and an editor

        # Lite XL's own faces, out of the image rather than stood in for.
        for face in ("FiraSans-Regular.ttf", "icons.ttf"):
            if f"litexl: font {face}: from the image" not in said:
                raise Failure(f"{face} did not come from the image:\n"
                              + said[-1500:])

        checks += 1

        # The title Lite XL composes, which the window manager says each time
        # it changes: `~` is the home directory and `*` is unsaved changes.
        opened, edited = "~/notes.txt - Lite XL", "~/notes.txt* - Lite XL"

        if f"wm: window Lite XL is now {opened}" not in said:
            raise Failure("the window was not named for its file:\n"
                          + windows(said))

        mark = len(guest.seen)
        keys(guest, "added")
        soak(guest, 3)

        if not said_after(guest, mark, f"wm: window {opened} is now {edited}",
                          10):
            raise Failure("the title did not mark the file edited:\n"
                          + windows(guest.seen[mark:]))

        mark = len(guest.seen)
        guest.sendkey("ctrl-s")
        soak(guest, 4)

        back = stop_and_read(guest, "/home/notes.txt")

        if "addedfirst line" not in back:
            raise Failure("the file does not say what was typed into it:\n"
                          + back[-600:])

        checks += 1

        # After the file, which is the better evidence that the save happened.
        if f"wm: window {edited} is now {opened}" not in guest.seen[mark:]:
            raise Failure("the title still marks the file edited once it was "
                          "saved:\n" + windows(guest.seen[mark:]))

        checks += 1
    finally:
        guest.close()

    return checks


def a_new_document(image):
    guest = Guest(image, 300)
    checks = 0

    try:
        guest.wait_for(PROMPT, "reached a shell")
        start(guest, "litexl:--trace")

        mark = len(guest.seen)
        guest.sendkey("ctrl-n")

        if not said_after(guest, mark, "command core:new-doc -> true", 15):
            raise Failure("Control-N did not make a new document:\n"
                          + guest.seen[mark:][-1500:])

        checks += 1

        keys(guest, "hello")
        soak(guest, 3)

        # An unnamed document asks where to go. A name relative to the
        # project is the same file whatever the prompt started with.
        guest.sendkey("ctrl-s")
        soak(guest, 3)
        keys(guest, "hello.txt")
        guest.sendkey("ret")
        soak(guest, 4)

        back = stop_and_read(guest, "/home/hello.txt")

        if "hello" not in back:
            raise Failure("the new document was not saved under its name:\n"
                          + back[-600:])

        checks += 1
    finally:
        guest.close()

    return checks


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"

    try:
        checks = edit_a_file(image) + a_new_document(image)
    except Failure as e:
        print(f"FAIL: {e}")
        return 1

    print(f"PASS: {checks} checks on Lite XL (a window, its own faces out of "
          f"the image, a title that follows its file, a file edited and "
          f"saved, Control-N, and a new document saved under a name).")
    return 0


if __name__ == "__main__":
    sys.exit(main())
