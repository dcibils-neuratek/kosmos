#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The host's disk tool on the C core, held to the same tool on the Lua.

`tools/kfs.lua` makes every disk the machine is given - the QEMU disk, a
suite's disk, the stick's `/Home` - and since `docs/diskfs.md` step 2 it
does it with `user/servers/kfs.c`, which the host's `lua` carries. The Lua
it used before is what the machine still runs, and `KFS_IMPL=lua` makes a
disk with it for as long as it exists.

`tools/test_kfs_cross.lua` holds the two cores to each other operation by
operation. This holds the two *tools* to each other, over what the tool is
used for: every command it has, run once with each, over real files - the
repository's documents and screenshots, some megabytes, in folders nested
several deep, an empty file, a file whose size is a whole number of blocks -
and **the images each makes must be the same, byte for byte, and each
command must have said the same thing**. Then what `get` and `getdir` take
out is what went in, and `copy` carries a file's attributes with it.

Usage: test_kfs_tool.py
"""

import glob
import os
import shutil
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

LUA = os.path.join(ROOT, "build", "host", "lua")

passed, failed = 0, 0


def check(ok, what):
    global passed, failed

    if ok:
        passed += 1
    else:
        failed += 1
        print("  FAIL: " + what)


def tool(impl, where, *words, env=None):
    """The tool, run with one implementation from `where`, and what it said."""
    e = dict(os.environ, KFS_IMPL=impl, **(env or {}))
    tool_path = os.path.join(HERE, "kfs.lua")
    done = subprocess.run([LUA, tool_path] + list(words), cwd=where, env=e,
                          capture_output=True, text=True)
    return done.returncode, done.stdout + done.stderr


# Sets a file's attributes, as the disk server would: the bytes `sys.pack`
# makes are the core's to keep and not to read, so any bytes will do.
ATTRS = r'''
local image, path = ...
local f = assert(io.open(image, "r+b"))
sys = {}
function sys.disk_read(sector, bytes)
  f:seek("set", sector * 512)
  local data = f:read(bytes) or ""
  return data .. string.rep("\0", bytes - #data)
end
function sys.disk_write(sector, data)
  f:seek("set", sector * 512)
  f:write(data)
  return true
end
function sys.pack(t) return "kind=" .. t.kind end
function sys.unpack(s) return { kind = s:match("kind=(.*)") } end
local kfs = os.getenv("KFS_IMPL") == "lua"
            and assert(loadfile("user/lib/kfs.lua"))() or require("kfsc")
local sb = assert(kfs.mount())
local number, node = assert(kfs.find(sb, path))
assert(kfs.write_attrs(sb, number, node, { kind = "launcher" }))
f:close()
'''


def main():
    base = scratch.path("kfs-tool")
    shutil.rmtree(base, ignore_errors=True)

    # The same files for both: documents and pictures from the repository,
    # into folders several deep, and the awkward sizes by hand.
    files = os.path.join(base, "files")
    os.makedirs(files)
    pairs = []

    for i, path in enumerate(sorted(glob.glob(os.path.join(ROOT, "docs", "*.md")))[:24]):
        pairs.append("%s:/Home/Docs/%s/%s" % (path, "abc"[i % 3], os.path.basename(path)))

    for path in sorted(glob.glob(os.path.join(ROOT, "docs", "screenshots", "*.png")))[-6:]:
        pairs.append("%s:/Home/Pictures/Screens/Old/%s" % (path, os.path.basename(path)))

    for name, size in (("empty", 0), ("block", 4096), ("blocks", 3 * 4096),
                       ("odd", 4097)):
        path = os.path.join(files, name)

        with open(path, "wb") as f:
            f.write(bytes((i * 31 + size) % 251 for i in range(size)))

        pairs.append("%s:/Home/Sizes/%s" % (path, name))

    total = sum(os.path.getsize(p.split(":", 1)[0]) for p in pairs)
    said = {}

    for impl in ("lua", "c"):
        work = os.path.join(base, impl)
        os.makedirs(os.path.join(work, "out"))
        os.symlink(os.path.join(ROOT, "user"), os.path.join(work, "user"))
        steps = []

        def step(*words, env=None):
            code, out = tool(impl, work, *words, env=env)
            steps.append("%s -> %d\n%s" % (" ".join(words[:2]), code, out))
            return code, out

        step("create", "disk.img", "64", *pairs)
        step("ls", "disk.img", "/")
        step("ls", "disk.img", "/Home/Docs/b")
        step("put", "disk.img", os.path.join(ROOT, "LICENSE"), "/Home/New/Deeper/LICENSE")
        step("get", "disk.img", "/Home/Sizes/odd", "out/odd")
        step("get", "disk.img", "/Home/New/Deeper/LICENSE", "out/LICENSE")
        step("getdir", "disk.img", "/Home/Pictures/Screens/Old", "out")
        step("rm", "disk.img", "/Home/Sizes/block")
        step("rm", "disk.img", "/Home/Sizes/nothing")
        step("df", "disk.img")

        # A file with attributes, carried by `copy` into a second disk.
        with open(os.path.join(work, "attrs.lua"), "w") as f:
            f.write(ATTRS)

        subprocess.run([LUA, "attrs.lua", "disk.img", "/Home/Sizes/blocks"],
                       cwd=work, env=dict(os.environ, KFS_IMPL=impl), check=True)
        step("create", "copy.img", "96")
        step("copy", "disk.img", "copy.img")
        step("create", "old.img", "16", env={"KFS_LAYOUT": "/system,/user,/home"})
        step("ls", "old.img", "/")

        said[impl] = "\n".join(steps).replace(work, "<work>")

    lua_dir, c_dir = os.path.join(base, "lua"), os.path.join(base, "c")

    check(said["lua"] == said["c"],
          "every command said the same with the C as with the Lua")

    if said["lua"] != said["c"]:
        for a, b in zip(said["lua"].splitlines(), said["c"].splitlines()):
            if a != b:
                print("    lua: %s\n    c:   %s" % (a, b))
                break

    check("-> 1" in said["c"] and said["c"].count("-> 0") == 13,
          "and every command did what it was asked, but the removal of a "
          "file that is not there")

    for image in ("disk.img", "copy.img", "old.img"):
        with open(os.path.join(lua_dir, image), "rb") as a, \
             open(os.path.join(c_dir, image), "rb") as b:
            check(a.read() == b.read(),
                  "%s is the same image from the C as from the Lua" % image)

    def same(path, original):
        with open(path, "rb") as a, open(original, "rb") as b:
            return a.read() == b.read()

    out = os.path.join(c_dir, "out")
    shots = sorted(glob.glob(os.path.join(ROOT, "docs", "screenshots", "*.png")))[-6:]

    check(same(os.path.join(out, "odd"), os.path.join(files, "odd"))
          and same(os.path.join(out, "LICENSE"), os.path.join(ROOT, "LICENSE"))
          and all(same(os.path.join(out, os.path.basename(p)), p) for p in shots),
          "what get and getdir took out is what went in")

    check("1 of them with attributes" in said["c"],
          "copy carried the one file's attributes")

    check("home" in said["c"] and "system" in said["c"],
          "a disk made with the layout from before 27 September has it")

    if failed:
        print("FAIL: %d of %d checks on the host's disk tool on the C core."
              % (failed, passed + failed))
        return 1

    print("PASS: %d checks on the host's disk tool on the C core (%d files, "
          "%.1f MB, through every command it has: the same images and the "
          "same words as the tool on the Lua)." % (passed, len(pairs),
                                                   total / 1048576))
    return 0


if __name__ == "__main__":
    sys.exit(main())
