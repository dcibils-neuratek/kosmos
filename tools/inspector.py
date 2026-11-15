#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**Web Inspector's front end, for JetSki** (`docs/webkit.html`, *Developer
mode*): Safari's inspector pages - `Main.html` and the rest of
`WebInspectorUI` - made from the WebKit source the build already unpacked,
for `/Home/System/Apps/JetSki/Inspector/`, seen as `/Kosmos/Apps/JetSki/Inspector/`.

    python3 tools/inspector.py          # makes build/inspector/res/WebInspectorUI

WebKit's own script makes them (`copy-user-interface-resources.pl`, as
GTK's and WPE's builds run it), combining and minifying 1,300 files into
254. It wants the protocol's commands, which JavaScriptCore's build
generated, and two minifiers from JavaScriptCore's scripts, in folders it
is told of. The localised strings are not among what it copies on a
platform it does not know, so they are copied after it, where
`WebInspectorUI::localizedStringsURL` looks on Kosmos (patch 0031).

Made once: the folder is kept until the WebKit source or this file is newer.
`files()` is the list `installed.py` puts on a disk.
"""

import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
SRC = os.path.join(ROOT, "build", "webkit", "src", "Source")
OUT = os.path.join(ROOT, "build", "inspector")
RES = os.path.join(OUT, "res", "WebInspectorUI")
STAMP = os.path.join(OUT, ".made")
COMMANDS = os.path.join(ROOT, "build", "webkit", "x86_64", "JavaScriptCore", "DerivedSources",
                        "inspector", "InspectorBackendCommands.js")
STRINGS = os.path.join(SRC, "WebInspectorUI", "Localizations", "en.lproj", "localizedStrings.js")


def fresh():
    if not os.path.exists(STAMP):
        return False

    made = os.path.getmtime(STAMP)

    return all(os.path.getmtime(p) <= made for p in (__file__, COMMANDS, STRINGS))


def make():
    if fresh():
        return

    for need in (COMMANDS, STRINGS):
        if not os.path.exists(need):
            sys.exit("inspector: no %s - JetSki's WebKit is built first (make ARCH=x86_64 apps)"
                     % os.path.relpath(need, ROOT))

    shutil.rmtree(OUT, ignore_errors=True)
    scripts = os.path.join(OUT, "scripts")
    derived = os.path.join(OUT, "derived")
    protocol = os.path.join(derived, "UserInterface", "Protocol")

    os.makedirs(scripts)
    os.makedirs(protocol)
    shutil.copy(COMMANDS, scripts)
    shutil.copy(COMMANDS, protocol)

    for minifier in ("cssmin.py", "jsmin.py"):
        shutil.copy(os.path.join(SRC, "JavaScriptCore", "Scripts", minifier), scripts)

    env = dict(os.environ,
               DERIVED_SOURCES_DIR=derived,
               SRCROOT=os.path.join(SRC, "WebInspectorUI"),
               JAVASCRIPTCORE_PRIVATE_HEADERS_DIR=scripts,
               TARGET_BUILD_DIR=os.path.join(OUT, "res"),
               UNLOCALIZED_RESOURCES_FOLDER_PATH="WebInspectorUI",
               COMBINE_INSPECTOR_RESOURCES="YES",
               COMBINE_TEST_RESOURCES="NO",
               INCLUDE_BROWSER_INSPECTOR_FRONTEND_HOST="YES",
               PYTHON="python3")

    with open(os.path.join(OUT, "log.txt"), "w") as log:
        done = subprocess.run(["perl", os.path.join(SRC, "WebInspectorUI", "Scripts",
                                                    "copy-user-interface-resources.pl")],
                              env=env, stdout=log, stderr=subprocess.STDOUT)

    if done.returncode or not os.path.exists(os.path.join(RES, "Main.html")):
        sys.exit("inspector: WebKit's script failed - build/inspector/log.txt says why")

    strings = os.path.join(RES, "Localizations", "en.lproj")
    os.makedirs(strings, exist_ok=True)
    shutil.copy(STRINGS, strings)

    open(STAMP, "w").close()


def files():
    """(host path, path inside Inspector/) for every file the front end is."""
    make()
    out = []

    for top, _, names in os.walk(RES):
        for name in sorted(names):
            path = os.path.join(top, name)
            out.append((path, os.path.relpath(path, RES)))

    return sorted(out, key=lambda pair: pair[1])


if __name__ == "__main__":
    print("inspector: %d files" % len(files()))
