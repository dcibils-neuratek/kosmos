#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""JetSki's files as a suite changes and installs them.

JetSki is `jetski.lua` and the files beside it in `user/installed/JetSki`
(`jetski.lua`, *JetSki's files*; `testing.md` 18.904). A suite that builds
a /Home of its own installs every one of them, and a control changes
whichever of them holds its string:

    texts = jetski_files.read()
    jetski_files.tried(CONTROLS)                 # every control's string still there
    jetski_files.change(texts, old, new, "the control x")
    files = jetski_files.install(texts, WORK)    # [(host path, /Home path)]
"""

import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(ROOT, "user", "installed", "JetSki")
HOME = "/Home/System/Apps/JetSki"


def read():
    """Every Lua file of JetSki's, by name."""
    texts = {}
    for name in sorted(os.listdir(FOLDER)):
        if name.endswith(".lua"):
            with open(os.path.join(FOLDER, name)) as f:
                texts[name] = f.read()
    return texts


def change(texts, old, new, who):
    """`old` made `new` in whichever of the files hold it. Stopped when none
    does: a control whose string has gone changes nothing and passes, and
    so proves nothing. `fixed` did that from 0.11.430 until 0.11.436
    (`testing.md` 18.833), and `returnless` and `openless` until JetSki was
    split into files (18.904) - which is why `tried` runs every control on
    every run."""
    holding = [name for name, text in texts.items() if old in text]
    if not holding:
        sys.exit("FAIL: %s finds nothing to change: %r is in none of JetSki's files" % (who, old))
    for name in holding:
        texts[name] = texts[name].replace(old, new)


def tried(controls):
    """Each control's changes made on a copy of the files, in order, so one
    whose string has gone stops an ordinary run and not only its own."""
    texts = read()
    for name, changes in controls.items():
        trial = dict(texts)
        for old, new in changes:
            change(trial, old, new, "the control " + name)


def install(texts, work):
    """The files written under `work`, and where each goes in /Home."""
    folder = os.path.join(work, "JetSki")
    os.makedirs(folder, exist_ok=True)
    files = []
    for name, text in texts.items():
        path = os.path.join(folder, name)
        with open(path, "w") as f:
            f.write(text)
        files.append((path, HOME + "/" + name))
    return files
