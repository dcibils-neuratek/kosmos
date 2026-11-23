#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""What a suite stands on, derived from what it exercises.

A suite names its *subjects* - the applications, programs, libraries, kits,
servers and drivers it exercises - and this turns them into the files of
the repository they are made of, so that `tools/gate.py` can run the suite
when one of those changed rather than when anything did.

    python3 tools/suitedeps.py app:paint server:audio
    python3 tools/suitedeps.py --why user/lib/ui.lua app:paint
    python3 tools/suitedeps.py --unresolved app:paint

Subjects:

    app:NAME       user/bin/apps/NAME.lua, user/bin/apps/NAME/, or
                   user/installed/NAME/ (any case)
    program:NAME   user/bin/programs/NAME.lua
    lib:PATH       /Kosmos/Libraries/PATH - user/lib/PATH, a file or a folder
    kit:NAME       /Kosmos/Kits/NAME - the folder its build function is in
    server:NAME    user/servers/NAME.c and NAME_*.c
    driver:PATH    user/drivers/PATH*
    files:GLOB     the files GLOB matches, from the top of the tree, as they are

Every file reached is followed by what it is: a Lua file through its
`use(...)`, `require ...` and `sys.kit(...)` calls and its header's
`-- kosmos: image`; a C source through the compiler's own `.d` file (the
build's `-MMD`), or its `#include` lines when there is none; and a header
`x.h` to the `x.c` beside it, which is what implements it. `files:` is not
followed: it is the escape hatch, and says exactly what it names.

How each namespace path becomes a file is read from what serves it, and
the code is named beside each mapping below.
"""

import collections
import glob
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)

# The x86-64 userland's objects, whose `.d` files say what each C source
# included (`UBUILD` in the Makefile; `-MMD -MP`). Every variant is read
# and their union taken: `KOSMOS_WEB` and `KOSMOS_FFMPEG` change what is
# included, and a superset is the safe way round.
VARIANTS = ["build/user-x86_64-web-ffmpeg", "build/user-x86_64-test", "build/user-x86_64"]

# Where the userland's `#include` is looked for when there is no `.d`: the
# including file's own folder first, then `UCFLAGS`'s `-I` list (Makefile).
INCLUDE_DIRS = ["runtime/upstream/stb", "runtime/upstream/miniz", "runtime/upstream/minimp3",
                "runtime/upstream/minih264", "runtime/upstream/minimp4",
                "user/include", "kernel", "runtime/include", "lua/upstream", "lua/kosmos"]

C_SOURCES = (".c", ".cc", ".cpp", ".S")
C_HEADERS = (".h", ".hpp")


class SubjectError(ValueError):
    """A subject that names nothing in the tree - a typo, said loudly."""


def _exists(rel):
    return os.path.isfile(os.path.join(ROOT, rel))


def _isdir(rel):
    return os.path.isdir(os.path.join(ROOT, rel))


def _files_under(rel):
    """Every file under the folder `rel`, sorted, relative to the top."""
    out = []
    for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, rel)):
        dirnames.sort()
        for f in filenames:
            out.append(os.path.relpath(os.path.join(dirpath, f), ROOT))
    return sorted(out)


def _norm(rel):
    """`rel` as a path inside the tree, or None: outside it, generated under
    `build/`, or not a file."""
    if os.path.isabs(rel):
        rel = os.path.relpath(rel, ROOT)
    rel = os.path.normpath(rel)
    if rel.startswith("..") or rel == "build" or rel.startswith("build/"):
        return None
    return rel if _exists(rel) else None


_cache = {}


def _cached(key, make):
    if key not in _cache:
        _cache[key] = make()
    return _cache[key]


# ---------------------------------------------------------------------------
# The namespace: how a path a program uses becomes a file in the tree
# ---------------------------------------------------------------------------

def bin_store():
    """`/Kosmos/Apps` and `/Kosmos/Programs`: one flat store, every file
    under its basename (Makefile `BIN_LUA`, `tools/progs2c.py`, `binfs.c`
    `view_of` - a view filters the store, it does not move a name)."""
    def make():
        store = {}
        for pattern in ("user/bin/apps/*.lua", "user/bin/apps/*/*.lua", "user/bin/programs/*.lua"):
            for path in sorted(glob.glob(os.path.join(ROOT, pattern))):
                store[os.path.basename(path)] = os.path.relpath(path, ROOT)
        return store
    return _cached("bin", make)


def installed_folder(name):
    """`user/installed/<Name>`, whatever the case `name` is in, or None."""
    def make():
        top = os.path.join(ROOT, "user/installed")
        return {d.lower(): "user/installed/" + d for d in sorted(os.listdir(top))
                if os.path.isdir(os.path.join(top, d))}
    return _cached("installed", make).get(name.lower())


def kit_table():
    """Kit name to the folder its build function is defined in.

    The names are `sys.kit`'s table in `user/init/sys_user.c` (`kits[]`:
    name, build function, own image); the folder is found by where that
    function is defined - `kosmos_net_kit` for `network` is in
    `user/kits/network/`, `kosmos_fractal_kit` in `user/bin/apps/fractal/`,
    `kosmos_h264_kit` in `user/kits/ffmpeg/`, `kosmos_doom_kit` in
    `user/installed/Doom/`."""
    def make():
        with open(os.path.join(ROOT, "user/init/sys_user.c"), encoding="utf-8") as f:
            text = f.read()
        names = re.findall(r'\{\s*"(\w+)",\s*kosmos_(\w+)_kit,\s*[01]\s*\}', text)

        defined = {}
        definition = re.compile(rb"^void\s+kosmos_(\w+)_kit\s*\(\s*lua_State\s*\*\s*L\s*\)\s*\{?\s*$",
                                re.M)
        for top in ("user/kits", "user/installed", "user/bin/apps"):
            for dirpath, dirnames, filenames in os.walk(os.path.join(ROOT, top)):
                for fn in filenames:
                    if not fn.endswith((".c", ".cpp")):
                        continue
                    path = os.path.join(dirpath, fn)
                    with open(path, "rb") as f:
                        data = f.read()
                    if b"_kit" not in data:
                        continue
                    for m in definition.finditer(data):
                        defined.setdefault(m.group(1).decode(),
                                           os.path.relpath(dirpath, ROOT))

        table = {}
        for name, symbol in names:
            if symbol in defined:
                table[name] = defined[symbol]
        return table
    return _cached("kits", make)


def kit_folder(name):
    """The folder of the kit `name`: `sys.kit`'s table, or a folder of
    `user/kits` of that name (`gfx`, `audio` - linked in, reached as
    globals or by C, and in no table)."""
    folder = kit_table().get(name)
    if folder is None and _isdir("user/kits/" + name):
        folder = "user/kits/" + name
    return folder


# The program store's other folders (`BIN_FOLDERS` in `user/init/init.lua`;
# the `--rooted` lists of `$(GEN)/programs.c` in the Makefile).
BIN_ROOTED = {
    "/Kosmos/Themes/": "user/themes/",
    "/Kosmos/Desktops/": "user/desktops/",
    "/Kosmos/Templates/": "user/templates/",
    "/Kosmos/Tutorial/": "user/tutorial/",
}


def resolve(path):
    """The files a namespace path a program uses stands for, or None.

    - `/Kosmos/Libraries/REL`: `user/lib/REL` - flat for `user/lib/*.lua`,
      by whole path for `solar/`, `translators/`, `wm/` and `groove/` - and
      `luacheck/REL` from `runtime/upstream/luacheck/src/luacheck/`
      (`$(GEN)/libraries.c` in the Makefile, `--rooted`).
    - `/Kosmos/Kits/NAME`: the kit's folder (`kit_table`).
    - `/Kosmos/Apps/NAME`, `/Kosmos/Programs/NAME`: the flat store
      (`bin_store`); `/Kosmos/Apps/JetSki/REL` a system application on the
      disk (`ns.SYSTEM_APPS` in `init.lua`), from `user/installed/JetSki/`.
    - `/Home/Apps/Name/REL`: an installed application's own file
      (`tools/installed.py`, `programs`), from `user/installed/Name/`.
    - `NAME.elf`: an application's own image, the kit of that name
      (`sys_user.c`, `own`), as `use("doom.elf")`.
    """
    if path.startswith("/Kosmos/Libraries/"):
        rel = path[len("/Kosmos/Libraries/"):]
        if rel.startswith("luacheck/") or rel == "luacheck":
            rel = "runtime/upstream/luacheck/src/" + rel
        else:
            rel = "user/lib/" + rel
        rel = os.path.normpath(rel)
        if _exists(rel):
            return [rel]
        return None

    if path.startswith("/Kosmos/Kits/"):
        name = path[len("/Kosmos/Kits/"):].strip("/")
        folder = kit_folder(name) if re.fullmatch(r"\w+", name) else None
        return [("kit", name)] if folder else None

    for prefix in ("/Kosmos/Apps/", "/Kosmos/Programs/"):
        if path.startswith(prefix):
            rel = path[len(prefix):]
            if "/" not in rel:
                found = bin_store().get(rel)
                return [found] if found else None
            if prefix == "/Kosmos/Apps/":
                folder, rest = rel.split("/", 1)
                inst = installed_folder(folder)
                if inst and _exists(inst + "/" + rest):
                    return [inst + "/" + rest]
            return None

    if path.startswith("/Home/Apps/"):
        rel = path[len("/Home/Apps/"):]
        if "/" in rel:
            folder, rest = rel.split("/", 1)
            inst = installed_folder(folder)
            if inst and _exists(inst + "/" + rest):
                return [inst + "/" + rest]
        return None

    for prefix, there in BIN_ROOTED.items():
        if path.startswith(prefix):
            rel = os.path.normpath(there + path[len(prefix):])
            return [rel] if _exists(rel) else None

    m = re.fullmatch(r"(\w+?)(?:-anv|-vk)?\.elf", path)
    if m and m.group(1) in kit_table():
        return [("kit", m.group(1))]

    if path == "webkit" and "webkit" in kit_table():
        return [("kit", "webkit")]

    return None


def resolve_prefix(prefix):
    """Every file a namespace path starting with `prefix` can be, for a
    path whose end is computed - `/Kosmos/Libraries/solar/fonts/f` is
    `user/lib/solar/fonts/f*`, every size. Libraries and installed
    applications only; None when nothing matches."""
    if prefix.startswith("/Kosmos/Libraries/"):
        rel = prefix[len("/Kosmos/Libraries/"):]
        tree = ("runtime/upstream/luacheck/src/" if rel.startswith("luacheck/") else "user/lib/") + rel
    elif prefix.startswith("/Home/Apps/") and prefix.count("/") >= 4:
        folder, rest = prefix[len("/Home/Apps/"):].split("/", 1)
        inst = installed_folder(folder)
        if inst is None:
            return None
        tree = inst + "/" + rest
    else:
        return None
    top = os.path.dirname(tree)
    if not _isdir(top):
        return None
    got = [f for f in _files_under(top) if f.startswith(tree)]
    return got or None


def project_c(rel, image):
    """A program's own C, which the C Kit builds into the image it names -
    `-- kosmos: image build/primes.elf` and `use("primes.elf")` beside
    `primes.c` (`docs/tinycc.md` C6, `user/templates/`): the C files in the
    program's folder - a project's, never a shared folder of the store's
    (Mandelbrot's is `fractal.c`, so the name is not required to match)."""
    here = os.path.dirname(rel)
    if not image.endswith(".elf") or here in ("user/bin/apps", "user/bin/programs", "user/lib"):
        return None
    got = sorted(os.path.join(here, f) for f in os.listdir(os.path.join(ROOT, here))
                 if f.endswith((".c", ".h")))
    return got or None


# ---------------------------------------------------------------------------
# Lua: what a file uses
# ---------------------------------------------------------------------------

# Comments and strings, so a `use(` in prose is never followed and a string
# is read whole. Long brackets first; everything between matches is code.
_LUA = re.compile(r"""
    (?P<comment>--\[(?P<e1>=*)\[.*?\](?P=e1)\] | --[^\n]*)
  | (?P<long>\[(?P<e2>=*)\[(?P<lbody>.*?)\](?P=e2)\])
  | "(?P<dq>(?:[^"\\\n]|\\.)*)"
  | '(?P<sq>(?:[^'\\\n]|\\.)*)'
""", re.S | re.X)

# What comes before a string that makes it a path to follow.
_CALL = re.compile(r"(?:(?<![\w])(?P<fn>use_?|require)|(?P<kit>\bsys\s*\.\s*kit))\s*\(?\s*$")
_CONCAT = re.compile(r"\s*\.\.")
_DYNAMIC = re.compile(r"(?<![\w.:])(?<!function )(use_?)\s*\(\s*(?=[^\"'\[\s)])")
_IMAGE = re.compile(r"--\s*kosmos:\s*image\s+(?:on\s+\S+\s+)?(\S+)")


def lua_uses(rel):
    """What the Lua file `rel` uses: (targets, unresolved). A target is a
    file or ("kit", name); an unresolved entry is (rel, line, text, why)."""
    def make():
        with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
            text = f.read()

        targets, unresolved = [], []

        # The header: the image a program runs in (`images_of` in
        # tools/installed.py, `IMAGES` in init.lua).
        for line in text.splitlines():
            if not line.startswith("--"):
                break
            m = _IMAGE.match(line)
            if m:
                got = resolve(os.path.basename(m.group(1))) or project_c(rel, m.group(1))
                if got:
                    targets.extend(got)
                else:
                    unresolved.append((rel, 0, m.group(1), "image"))

        if "use" not in text and "require" not in text and "kit" not in text:
            return targets, unresolved

        last = 0
        for m in _LUA.finditer(text):
            code = text[last:m.start()]
            last = m.end()

            for d in _DYNAMIC.finditer(code):
                line = text.count("\n", 0, m.start() - len(code) + d.start()) + 1
                unresolved.append((rel, line, code[d.start():d.start() + 60].split("\n")[0],
                                   "dynamic"))

            if m.group("comment") is not None:
                continue

            call = _CALL.search(code)
            if not call:
                continue

            value = m.group("dq") if m.group("dq") is not None else (
                m.group("sq") if m.group("sq") is not None else m.group("lbody"))
            line = text.count("\n", 0, m.start()) + 1

            if _CONCAT.match(text, m.end()):
                # A literal start and a computed end - `require("solar.fonts.f"
                # .. px)`: every file the start can lead to.
                prefix = value
                if call.group("fn") == "require":
                    prefix = "/Kosmos/Libraries/" + value.replace(".", "/")
                got = resolve_prefix(prefix)
                if got:
                    targets.extend(got)
                else:
                    unresolved.append((rel, line, value + " ..", "dynamic"))
                continue

            if call.group("kit"):
                path = "/Kosmos/Kits/" + value
            elif call.group("fn") == "require":
                path = None
                base = "/Kosmos/Libraries/" + value.replace(".", "/")
                for candidate in (base + ".lua", base + "/init.lua"):
                    if resolve(candidate):
                        path = candidate
                        break
                if path is None:
                    unresolved.append((rel, line, value, "require"))
                    continue
            else:
                path = value

            got = resolve(path) or project_c(rel, path)
            if got:
                targets.extend(got)
            else:
                unresolved.append((rel, line, value, "no such path"))

        tail = text[last:]
        for d in _DYNAMIC.finditer(tail):
            line = text.count("\n", 0, last + d.start()) + 1
            unresolved.append((rel, line, tail[d.start():d.start() + 60].split("\n")[0],
                               "dynamic"))

        return targets, unresolved
    return _cached(("lua", rel), make)


# ---------------------------------------------------------------------------
# C: what a source includes
# ---------------------------------------------------------------------------

def _dep_file(rel):
    """The headers every x86-64 build's `.d` file names for the source
    `rel`, or None when no build has compiled it."""
    found = None
    for variant in VARIANTS:
        d = os.path.join(ROOT, variant, rel + ".d")
        try:
            with open(d, encoding="utf-8", errors="replace") as f:
                text = f.read()
            stale = os.path.getmtime(d) < os.path.getmtime(os.path.join(ROOT, rel))
        except OSError:
            continue
        # The first rule: `obj: source dep dep ...`, continued by backslashes.
        rule = text.split("\n\n", 1)[0]
        rule = rule.replace("\\\n", " ")
        first = rule.split("\n", 1)[0]
        if ":" not in first:
            continue
        found = found or set()
        for word in first.split(":", 1)[1].split():
            p = _norm(word)
            if p and p != rel:
                found.add(p)
        if stale:
            found.add(("stale",))
    return found


_INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^">]+)[">]', re.M)


def _scan_includes(rel):
    """The files `rel`'s `#include` lines name that exist in the tree:
    beside it first, for a quoted one, then `INCLUDE_DIRS`. Every
    `#include` counts, whatever `#if` is around it."""
    with open(os.path.join(ROOT, rel), encoding="utf-8", errors="replace") as f:
        text = f.read()
    here = os.path.dirname(rel)
    out = []
    for kind, name in _INCLUDE.findall(text):
        dirs = ([here] if kind == '"' else []) + INCLUDE_DIRS
        for d in dirs:
            p = _norm(os.path.join(d, name))
            if p:
                out.append(p)
                break
    return out


def c_deps(rel):
    """What the C file `rel` stands on, one step: (files, scanned) - a
    source's headers from its `.d` (or its `#include` lines when no build
    has one, or when the `.d` is older than the source), and a header's
    `#include` lines when asked - plus, for a header `x.h` under `user/`,
    the `x.c` or `x.cpp` beside it."""
    def make():
        out, scanned = [], []
        if rel.endswith(C_SOURCES):
            deps = _dep_file(rel)
            if deps is None or ("stale",) in deps:
                scanned.extend(_scan_includes(rel))
            out.extend(sorted(p for p in (deps or ()) if isinstance(p, str)))
        elif rel.endswith(C_HEADERS) and rel.startswith("user/"):
            # Only the userland's own: `kernel/syscall.h` is the ABI's
            # numbers, which a server includes and does not link the
            # kernel's `syscall.c` for; `runtime/` and `lua/` are the
            # runtime every program carries, a suite's `RUNTIME`.
            stem = os.path.splitext(rel)[0]
            for ext in (".c", ".cpp", ".cc"):
                if _exists(stem + ext):
                    out.append(stem + ext)
        return out, scanned
    return _cached(("c", rel), make)


# ---------------------------------------------------------------------------
# The closure
# ---------------------------------------------------------------------------

FOLLOW = True       # the test's control turns this off: roots only


def kit_files(name):
    folder = kit_folder(name)
    if folder is None:
        raise SubjectError("kit:%s - no kit of that name (sys_user.c kits[], user/kits/)" % name)
    return _files_under(folder)


def closure(roots, parents=None, unresolved=None):
    """Every file reachable from `roots` - (file, how) pairs or files - as a
    sorted list. `parents`, given a dict, is filled with file -> (parent,
    how) for `--why`; `unresolved`, given a list, with what could not be
    followed."""
    if parents is None:
        parents = {}
    seen = set()
    headers_scanned = set()     # headers reached by a scan, whose own lines are read
    work = collections.deque()      # breadth first: `--why` gives the shortest chain

    def add(f, parent, how, scanned=False):
        if f in seen:
            return
        seen.add(f)
        parents[f] = (parent, how)
        if scanned:
            headers_scanned.add(f)
        work.append(f)

    for r in roots:
        f, how = r if isinstance(r, tuple) else (r, "root")
        add(f, None, how)

    while work and FOLLOW:
        f = work.popleft()
        if f.endswith(".lua"):
            targets, bad = lua_uses(f)
            if unresolved is not None:
                unresolved.extend(bad)
            for t in targets:
                if isinstance(t, tuple):
                    for k in kit_files(t[1]):
                        add(k, f, "kit:" + t[1])
                else:
                    add(t, f, "use")
        elif f.endswith(C_SOURCES + C_HEADERS):
            deps, scanned = c_deps(f)
            for d in deps:
                add(d, f, "header" if d.endswith(C_HEADERS) else "implements")
            for d in scanned:
                add(d, f, "#include", scanned=True)
            if f in headers_scanned and f.endswith(C_HEADERS):
                for d in _scan_includes(f):
                    add(d, f, "#include", scanned=True)

    return sorted(seen)


def lua_closure(path, unresolved=None):
    """The Lua file `path` and everything it uses, transitively."""
    rel = _norm(path)
    if rel is None:
        raise SubjectError("%s: no such file in the tree" % path)
    return closure([rel], unresolved=unresolved)


def c_closure(paths):
    """C sources and everything they include, and what implements that."""
    rels = []
    for p in paths:
        rel = _norm(p)
        if rel is None:
            raise SubjectError("%s: no such file in the tree" % p)
        rels.append(rel)
    return closure(rels)


# ---------------------------------------------------------------------------
# Subjects
# ---------------------------------------------------------------------------

KINDS = ("app", "program", "lib", "kit", "server", "driver", "files")


def subject_roots(subject):
    """The files a subject names directly, before anything is followed."""
    kind, sep, name = subject.partition(":")
    if not sep or kind not in KINDS or not name:
        raise SubjectError("%r - a subject is KIND:NAME, KIND one of %s"
                           % (subject, ", ".join(KINDS)))

    found = []
    if kind == "app":
        if _exists("user/bin/apps/%s.lua" % name):
            found.append("user/bin/apps/%s.lua" % name)
        if _isdir("user/bin/apps/" + name):
            found.extend(_files_under("user/bin/apps/" + name))
        inst = installed_folder(name) if "/" not in name else None
        if inst:
            found.extend(_files_under(inst))
    elif kind == "program":
        if _exists("user/bin/programs/%s.lua" % name):
            found.append("user/bin/programs/%s.lua" % name)
    elif kind == "lib":
        got = resolve("/Kosmos/Libraries/" + name)
        if got:
            found.extend(got)
        elif _isdir("user/lib/" + name):
            found.extend(_files_under("user/lib/" + name))
    elif kind == "kit":
        if kit_folder(name):
            found.extend(kit_files(name))
    elif kind == "server":
        for pattern in ("user/servers/%s.c" % name, "user/servers/%s_*.c" % name):
            found.extend(sorted(os.path.relpath(p, ROOT)
                                for p in glob.glob(os.path.join(ROOT, pattern))))
    elif kind == "driver":
        base = "user/drivers/" + name
        if _isdir(base):
            found.extend(_files_under(base))
        found.extend(sorted(os.path.relpath(p, ROOT)
                            for p in glob.glob(os.path.join(ROOT, base + "*"))
                            if os.path.isfile(p)))
    elif kind == "files":
        found.extend(sorted(os.path.relpath(p, ROOT)
                            for p in glob.glob(os.path.join(ROOT, name), recursive=True)
                            if os.path.isfile(p)))

    found = sorted(set(found))
    if not found:
        raise SubjectError("%s resolves to nothing in the tree" % subject)
    return found


def suite_files(subjects, parents=None, unresolved=None):
    """Every file the subjects stand on, as one sorted list."""
    if parents is None:
        parents = {}
    roots, plain = [], []
    for s in subjects:
        files = subject_roots(s)
        if s.startswith("files:"):
            plain.extend((f, s) for f in files)
        else:
            roots.extend((f, s) for f in files)
    out = set(closure(roots, parents, unresolved))
    for f, s in plain:
        parents.setdefault(f, (None, s))
        out.add(f)
    return sorted(out)


def subject_files(subject, unresolved=None):
    """The files one subject stands on."""
    return suite_files([subject], unresolved=unresolved)


def why(target, subjects):
    """The chain from a subject to `target`, as lines, or None."""
    parents = {}
    files = suite_files(subjects, parents)
    rel = os.path.normpath(target)
    if rel not in files:
        return None
    chain = []
    while rel is not None:
        parent, how = parents[rel]
        chain.append((rel, how))
        rel = parent
    chain.reverse()
    lines = [chain[0][1]]
    for f, how in chain[1:]:
        lines.append("  %-12s %s" % (how, f))
    return [lines[0], "  %-12s %s" % ("names", chain[0][0])] + lines[1:]


def main(argv):
    if not argv or argv[0] in ("-h", "--help"):
        print(__doc__.strip())
        return 0
    try:
        if argv[0] == "--why":
            if len(argv) < 3:
                print("usage: suitedeps.py --why FILE SUBJECT...", file=sys.stderr)
                return 2
            lines = why(argv[1], argv[2:])
            if lines is None:
                print("%s is not reached from %s" % (argv[1], " ".join(argv[2:])))
                return 1
            print("\n".join(lines))
            return 0
        if argv[0] == "--unresolved":
            bad = []
            files = suite_files(argv[1:], unresolved=bad)
            for rel, line, text, how in sorted(set(bad)):
                print("%s:%d: %s (%s)" % (rel, line, text, how))
            print("%d files, %d unresolved" % (len(files), len(set(bad))))
            return 0
        for f in suite_files(argv):
            print(f)
        return 0
    except SubjectError as e:
        print("suitedeps: %s" % e, file=sys.stderr)
        return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
