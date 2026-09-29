#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The disk server refuses what its protocol cannot say, and stays whole.

**A server receives exactly what it expects** (`CLAUDE.md`): `/Home` speaks
`diskproto.h` since `docs/diskfs.md` step 3, and a caller that is wrong, out
of date or hostile has to meet a refusal rather than a server that did
something with it. The namespace only ever sends well-formed requests, so
nothing else in the gate sends a bad one - this does, with `fs.raw`, a
program on the disk sending bytes straight to the server: a request the
wrong size, an operation there is none of, a path with no end, a
destination with no end, lengths past what their fields hold, attributes
and query terms that are not a flat table, a region missing or too small
for what is asked, a format without its words, a write to `.super`, and a
path through a file.

**Each has to be refused with its own number** - `diskproto.h`'s, or the
filesystem's for the path through a file - **and afterwards the disk is as
it was**: a file written before reads back the same, the free space has not
moved, nothing the refused writes named exists, and the server still lists.

Usage: run_diskwire.py IMAGE
"""

import os
import re
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import scratch                                              # noqa: E402

# Each: a name, the refusal it must get, and the Lua that makes the request.
CASES = [
    ("wrong size", 1, 'send("0123456789")'),
    ("no such operation", 1, 'send(req(99, 0, 0, 0, 0, "/Home"))'),
    ("a path with no end", 1,
     'send(string.pack(REQ, 1, 0, 0, 0, 0, 0, string.rep("a", 512), ""))'),
    ("a destination longer than a path", 1,
     'send(req(5, 0, 0, 0, 600, KEPT, "x"))'),
    ("a destination with no end", 1,
     'send(req(5, 0, 0, 0, 10, KEPT, string.rep("b", 20)))'),
    ("a write longer than its field", 1,
     'send(req(3, 0, 0, 0, 5000, "/Home/wire-x.txt", "abc"))'),
    ("attributes that are not a table", 6,
     'send(req(8, 0, 0, 0, nil, KEPT, "\\1\\2\\3 not a table"))'),
    ("attributes with a table inside", 6,
     'send(req(8, 0, 0, 0, nil, KEPT, sys.pack({ a = { b = 1 } })))'),
    ("query terms named by numbers", 6,
     'send(req(9, 0, 0, 0, nil, "/Home", sys.pack({ [1] = "x" })))'),
    ("a read into no region", 7, 'send(req(2, 1, 0, 100, 0, KEPT))'),
    ("a read larger than its region", 7,
     'send(req(2, 1, 0, 1 << 40, 0, KEPT), page)'),
    ("a write larger than its region", 7,
     'send(req(3, 1, 0, 1 << 20, 0, "/Home/wire-y.txt"), page)'),
    ("a format without its words", 8,
     'send(req(12, 0, 0, 0, nil, "/Home", "please"))'),
    ("a write to .super", 3, 'send(req(3, 0, 0, 0, nil, "/Home/.super", "x"))'),
    ("a path through a file", 32 + 8,
     'send(req(7, 0, 0, 0, 0, KEPT .. "/inside"))'),
]

PROBE_HEAD = r'''
local REQ = "<I4I4I8I8I4I4c512c1024"
local KEPT = "/Home/wire-kept.txt"

local function req(op, flags, offset, bytes, length, path, data)
  data = data or ""
  return string.pack(REQ, op, flags, offset, bytes, length or #data, 0, path, data)
end

local function send(bytes, pass)
  local raw, why = fs.raw("/Home", bytes, pass, "disk")

  if not raw then return "none:" .. tostring(why) end
  if #raw < 1104 then return "short:" .. #raw end

  return (string.unpack("<I4", raw))
end

local body = "kept " .. string.rep("k", 10000)
fs.write(KEPT, body)

local page = sys.memory(1)
local before = fs.read("/Home/.super").free_blocks
'''

PROBE_TAIL = r'''
local back = fs.read(KEPT)
local after = fs.read("/Home/.super").free_blocks

print(("WHOLE %s %s %s %s %s %s"):format(tostring(back == body), tostring(before),
      tostring(after), tostring(fs.getattr("/Home/wire-x.txt") == nil),
      tostring(fs.getattr("/Home/wire-y.txt") == nil),
      tostring(type(fs.list("/Home")) == "table")))
'''


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    work = scratch.directory("diskwire")
    probe = os.path.join(work, "wire.lua")
    disk = os.path.join(work, "disk.img")

    with open(probe, "w") as f:
        f.write(PROBE_HEAD)

        for i, (_, _, lua) in enumerate(CASES):
            f.write('print("CASE %d " .. tostring(%s))\n' % (i + 1, lua))

        f.write(PROBE_TAIL)

    subprocess.run([os.path.join(ROOT, "build", "host", "lua"),
                    os.path.join(HERE, "kfs.lua"), "create", disk, "32",
                    probe + ":/Home/wire.lua"],
                   check=True, capture_output=True, cwd=ROOT)

    os.environ["KOSMOS_DISK"] = disk
    import run_screenshot as R                                  # noqa: E402

    guest = R.Guest(image, 120)

    try:
        guest.wait_for("kosmos>", "a prompt")
        guest.type("run /Home/wire.lua")
        guest.wait_for("WHOLE ", "the disk looked at afterwards")
        time.sleep(0.5)
        guest._read_available()
    finally:
        guest.close()

    said = guest.seen
    fails = []

    for i, (name, want, _) in enumerate(CASES):
        got = re.search(r"^CASE %d (\S+)" % (i + 1), said, re.M)

        if not got or got.group(1) != str(want):
            fails.append("%s: refused with %s, where %d was due"
                         % (name, got.group(1) if got else "nothing", want))

    whole = re.search(r"^WHOLE (\S+) (\S+) (\S+) (\S+) (\S+) (\S+)", said, re.M)

    if not whole:
        fails.append("the disk was not looked at afterwards:\n" + said[-800:])
    else:
        same, before, after, no_x, no_y, lists = whole.groups()

        if same != "true":
            fails.append("the file written before does not read back the same")

        if before != after or before == "nil":
            fails.append("the free space moved: %s blocks, then %s" % (before, after))

        if no_x != "true" or no_y != "true":
            fails.append("a refused write made its file")

        if lists != "true":
            fails.append("the server does not list any more")

    checks = len(CASES) + 4

    if "died" in said:
        fails.append("something died: " + said[said.find("died") - 60:][:400])

    if fails:
        print("FAIL: %d of %d checks on the disk server's wire:" % (len(fails), checks))

        for f in fails:
            print("  " + f)

        return 1

    print("PASS: %d checks on the disk server's wire (%d malformed requests "
          "each refused with its own number, and afterwards the disk as it "
          "was: a file the same, the free space unmoved, nothing a refused "
          "write named, and a listing)." % (checks, len(CASES)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
