#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Attributes and the queries over them, on a machine with a real disk.

**M7's definition of done was a live query, and nothing in `make test` ever
checked one.** `qbench` measures how fast a query is and `latency.lua`
measures how quickly a watch wakes, and neither of them would notice a
query returning the wrong paths - which is what it had been doing on the
disk for as long as the disk could answer.

The bug this exists to keep out: one disk is mounted three times, at
`/system`, `/user` and `/Home`, each naming a subtree of itself. The
namespace maps `/Home/doc.pdf` onto `/Home/doc.pdf` in the server and put
the mount prefix back on the way out, giving `/Home/home/doc.pdf`; and the
server answered a question asked about `/Home` with everything on the disk,
`/system` included. Both were invisible because every query test used
`/Temporary`, which is the one mount with no root - so the two paths through
that code had never both been walked.

So the checks below are all about *which* paths come back, on both kinds of
mount. Speed is `bench/`'s job and is measured elsewhere.
"""

import os
import sys
import scratch

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_disk
import run_interchange


class Failure(Exception):
    pass


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0

    work = scratch.directory()
    disk = os.path.join(work, "queries.img")

    try:
        #
        # **A disk made before `/home` was `/Home`**, spelled as every stick
        # before 27 September is: its home folder is `home`. Mounted at
        # `/Home`, it has to answer in the new spelling - which a query does
        # only because the namespace takes the disk's own name for its root
        # off whatever case it is in.
        #
        os.environ["KFS_LAYOUT"] = "/system,/user,/home"   # as a stick before 27 September
        run_interchange.kfs("create", disk, "32")
        del os.environ["KFS_LAYOUT"]

        #
        # **A preference where it was before 28 September** - a dotfile at
        # the top of the home - for the shell to move into
        # `/Home/Preferences` as it starts (`roadmap.md` 6s d).
        #
        old_pref = os.path.join(work, "old-tracker")

        with open(old_pref, "w") as f:
            f.write("kept across the move")

        run_interchange.kfs("put", disk, old_pref, "/home/.tracker")

        #
        # Every answer is printed as one line with a marker, so a check is a
        # string comparison rather than a parse. `table.concat` with a comma
        # keeps the order the server sorted them into, which is part of what
        # is being tested: a query that returned its answers in table order
        # would differ between runs.
        #
        out = run_disk.boot(image, disk, [
            # Two files on the disk, one a folder down, and one in memory, so
            # both kinds of mount are exercised by the same run. The folder
            # stands where `/system` did (`roadmap.md` 6s c3): a second part
            # of the same disk, which a question asked of one part must
            # leave out.
            'fs.send("/Home/other", { type = "mkdir" }) '
            'fs.write("/Home/a.txt", "one") '
            'fs.write("/Home/other/b.txt", "two") '
            'fs.write("/Temporary/c.txt", "three")',

            'fs.setattr("/Home/a.txt", { kind = "book" }) '
            'fs.setattr("/Home/other/b.txt", { kind = "book" }) '
            'fs.setattr("/Temporary/c.txt", { kind = "book", size = "small" })',

            'print("Q-HOME", table.concat(fs.query("/Home", '
            '{ kind = "book" }) or {}, ","))',

            'print("Q-SUB", table.concat(fs.query("/Home/other", '
            '{ kind = "book" }) or {}, ","))',

            'print("Q-DATA", table.concat(fs.query("/Temporary", '
            '{ kind = "book" }) or {}, ","))',

            # Two terms, and the second one is what narrows it.
            'print("Q-TWO", table.concat(fs.query("/Temporary", '
            '{ kind = "book", size = "small" }) or {}, ","))',

            'print("Q-NONE", table.concat(fs.query("/Home", '
            '{ kind = "nothing-has-this" }) or {}, ","))',

            # The path a query returns has to be one that can be read back.
            # A doubled prefix is still a string and still looks like an
            # answer; only reading it says whether it names anything.
            'local hit = (fs.query("/Home", { kind = "book" }) or {})[1] '
            'print("Q-READ", hit and (fs.read(hit) or "unreadable") or "none")',

            # A sixty-four character name, on both kinds of mount, and in
            # memory a directory down: a 64-character directory holding a
            # 64-character file is 136 bytes of path, which /Temporary's old
            # 128-byte field refused outright.
            'D, N = string.rep("d", 64), string.rep("n", 60) .. ".txt" '
            'fs.send("/Temporary/" .. D, { type = "mkdir" }) '
            'fs.write("/Temporary/" .. D .. "/" .. N, "deep") '
            'fs.write("/Home/" .. N, "long")',

            'print("L-RAMFS", fs.read("/Temporary/" .. D .. "/" .. N) or "none", '
            'table.concat(fs.list("/Temporary/" .. D) or {}, ",")) '
            'print("L-HOME", fs.read("/Home/" .. N) or "none")',

            # A name longer tha /Running keeps, registered the way `ui.window`
            # registers a window under its title. The namespace kit used to
            # raise inside `string.pack` before the registry saw it, which
            # killed the process that asked - a window titled with a ROM's
            # No-Intro name was the first to be that long.
            'E = sys.endpoint() '
            'R = fs.send("/Running", { type = "register", '
            'name = string.rep("t", 46) }, E) '
            'print("A-LONG", R and R.name or "refused") '
            'if R then fs.send("/Running", { type = "unregister", name = R.name }) end',

            # And the rest of that class: a name longer than its field at /Devices
            # and at /bin is an answer - no such device, no such program -
            # rather than a raise from `string.pack` inside the namespace kit.
            'print("F-LONG", pcall(fs.getattr, "/Devices/" .. string.rep("d", 40)), '
            'pcall(fs.getattr, "/Kosmos/Programs/" .. string.rep("b", 80)))',

            # **Names whatever their case, kept as they were given**
            # (`roadmap.md` 6s): one file on the disk and one in memory,
            # written and read however the path is typed, and a write in
            # another case replacing the file and keeping its name.
            'fs.write("/Home/Case.txt", "disk") fs.write("/TEMPORARY/Case.txt", "memory") '
            'fs.write("/HOME/CASE.TXT", "disk again")',

            'print("C-READ", fs.read("/home/case.txt"), fs.read("/temporary/CASE.TXT"))',

            'local n, c = 0, nil for _, x in ipairs(fs.list("/Home") or {}) do '
            'if x:lower() == "case.txt" then n, c = n + 1, x end end '
            'print("C-LIST", n, c)',

            # The disk's index is keyed by path - it was built by the queries
            # above - so an attribute set through another case has to land
            # under the file's own spelling, once.
            'fs.setattr("/HOME/case.TXT", { kind = "cased" }) '
            'print("C-QUERY", table.concat(fs.query("/home", { kind = "cased" }) or {}, ","))',

            # The programs, the devices, the registry and a library.
            'print("C-BIN", (fs.getattr("/KOSMOS/apps/CLOCK.lua") or {}).kind, type(fs.read("/devices/CPU")))',

            'E1, E2 = sys.endpoint(), sys.endpoint() '
            'R1 = fs.send("/RUNNING", { type = "register", name = "Cased" }, E1) '
            'R2 = fs.send("/Running", { type = "register", name = "CASED" }, E2) '
            'print("C-APP", R1 and R1.name, R2 and R2.name) '
            'fs.send("/Running", { type = "unregister", name = R1 and R1.name or "" }) '
            'fs.send("/Running", { type = "unregister", name = R2 and R2.name or "" })',

            # A table larger than a message, kept in memory: stored in pieces
            # as it always was, and no longer refused by `sys.pack` for being
            # larger than the one message it used to pack into - the Clock's
            # replicant crossed that line when `/dev/cpu` became `/Devices/cpu`.
            'local t = {} for i = 1, 200 do t[i] = ("r"):rep(40) end '
            'local ok = fs.write("/Temporary/big.tbl", t) local back = fs.read("/Temporary/big.tbl") '
            'print("T-BIG", ok and true, type(back) == "table" and #back, back and back[200] == ("r"):rep(40))',

            # **The root as agreed** (`roadmap.md` 6s): what `/` lists, and
            # a directory changed to in the old spelling answering in the new.
            'local r = fs.list("/") or {} table.sort(r) print("R-ROOT", table.concat(r, ","))',

            'cd /home',

            # **`/Kosmos`** (6s c): a folder made of mounts, holding the
            # libraries and the kits - the kits answered in-process, each
            # one a thing to `use` and never a file to read.
            'local k = fs.getattr("/kosmos") local p = fs.getattr("/Kosmos/Kits/pdf") '
            'local _, why = fs.read("/Kosmos/Kits/pdf") '
            'print("K-KOSMOS", k and k.kind, table.concat(fs.list("/Kosmos") or {}, ","), '
            'p and p.kind, why)',

            # **The applications and the programs, apart** (6s c2): one
            # store in the image, two folders, each holding only its kind;
            # and a program found by its name, or by the path a launcher
            # made before the split still says.
            'local function has(p) return fs.getattr(p) ~= nil end '
            'print("K-APPS", has("/Kosmos/Apps/clock.lua"), has("/Kosmos/Programs/clock.lua"), '
            'has("/Kosmos/Programs/ls.lua"), has("/Kosmos/Apps/ls.lua"), '
            'fs.program("clock"), fs.program("ls"), fs.program("/bin/tracker.lua"))',

            # **The looks, and folders in a store** (6s c3b): the five
            # looks in `/Kosmos/Themes`, a look a file and a library a
            # library; and `luacheck` a folder in `/Kosmos/Libraries`, once,
            # where the listing used to hold each of its files by its path.
            'local function kind(p) return (fs.getattr(p) or {}).kind end '
            'local n, flat = 0, 0 for _, x in ipairs(fs.list("/Kosmos/Libraries") or {}) do '
            'if x == "luacheck" then n = n + 1 end if x:find("/") then flat = flat + 1 end end '
            'print("K-THEMES", table.concat(fs.list("/Kosmos/Themes") or {}, ","), '
            'kind("/Kosmos/Themes/Plex.theme"), kind("/Kosmos/Libraries/ui.lua"), '
            'kind("/Kosmos/Libraries/luacheck"), n, flat)',

            'local n = 0 for _, x in ipairs(fs.list("/Kosmos/Kits") or {}) do '
            'if x == "pdf" or x == "compress" or x == "3d" then n = n + 1 end end '
            'print("K-KITS", n)',

            # **What an application opens, from its header** (`roadmap.md`
            # 6z): Photo's three types, Video's film and Play's - a program
            # - in the attributes the store reports, and nothing for an
            # application that says nothing.
            'local function o(p) return table.concat((fs.getattr(p) or {}).opens or {}, " ") end '
            'print("K-OPENS", o("/Kosmos/Apps/photo.lua"), o("/Kosmos/Apps/video.lua"), '
            'o("/Kosmos/Programs/play.lua"), '
            '(fs.getattr("/Kosmos/Apps/calc.lua") or {}).opens == nil)',

            # And where it went: into /Home/Preferences, under its name
            # without the dot, and nothing left at the top.
            'print("P-MOVED", fs.getattr("/Home/.tracker") == nil, '
            'fs.read("/Home/Preferences/tracker"), '
            '(fs.getattr("/Home/Preferences") or {}).kind)',

            # **The Deskbar's menu as it ships** (`roadmap.md` 6zd): a folder
            # a section, laid out from each application's header - Tracker a
            # launcher in Applications, starting its whole path with its own
            # picture; a group a submenu; neither the Deskbar nor Info, whose
            # section is none; and nothing can be written into it.
            'local function a(p) return fs.getattr(p) or {} end '
            'local t = a("/Kosmos/Deskbar/Applications/tracker") '
            'local wrote = fs.write("/Kosmos/Deskbar/Applications/mine", "") '
            'print("K-DESKBAR", table.concat(fs.list("/Kosmos/Deskbar") or {}, ","), '
            't.kind, t.program, t.icon, a("/Kosmos/Deskbar/Demos/GL Demos").kind, '
            'a("/Kosmos/Deskbar/Demos/GL Demos/glgears").kind, '
            'fs.getattr("/Kosmos/Deskbar/Applications/deskbar") == nil, '
            'fs.getattr("/Kosmos/Deskbar/Applications/info") == nil, not wrote, '
            # Each one's name for a person (`kosmos: name`, 3 October): the
            # launcher's title, with the file's name still its own.
            '"[" .. tostring(a("/Kosmos/Deskbar/System/procs").title) .. "]", '
            '"[" .. tostring(fs.getattr("/Kosmos/Deskbar/Applications/machine") == nil) .. "]", '
            '"[" .. tostring(a("/Kosmos/Apps/machine.lua").title) .. "]", '
            # A page the menu opens (`user/pages`): the browser, at its address.
            'a("/Kosmos/Deskbar/Development/Documentation/cheatsheet").program, '
            'a("/Kosmos/Deskbar/Development/Documentation/cheatsheet").args, '
            '"[" .. tostring(a("/Kosmos/Deskbar/Development/Documentation/cheatsheet").title) .. "]")',

            # **A file's time is a date** (`roadmap.md` 6za step b): written
            # to the disk now, its `modified` is the clock's second, give or
            # take the few a write and a read take - where it was a count
            # since boot, which no clock could be held to.
            'fs.write("/Home/dated.txt", "when") '
            'local a = fs.getattr("/Home/dated.txt") or {} '
            'local c = fs.read("/Devices/clock") or {} '
            'print("D-DATED", math.type(a.modified), c.epoch ~= nil and '
            'math.type(a.modified) == "integer" and math.abs(a.modified - c.epoch) <= 5)',

            # `use` is a program's, not the prompt's: a program that asks
            # for one library by two spellings.
            'fs.write("/Temporary/usetwice.lua", "print(\\"C-USE\\", '
            'use(\\"/KOSMOS/libraries/Text.lua\\") == use(\\"/Kosmos/Libraries/text.lua\\"))\\n")',

            'run /Temporary/usetwice.lua',

            # And a name's spelling changed by a rename to itself in another
            # case, on both kinds of mount.
            'print("C-RENAME", fs.send("/Home/case.txt", { type = "rename", to = "/Home/CASE.txt" }) and true, '
            'fs.send("/Temporary/case.txt", { type = "rename", to = "/Temporary/CASE.txt" }) and true) '
            'local seen = {} for _, m in ipairs({ "/Home", "/Temporary" }) do '
            'for _, x in ipairs(fs.list(m) or {}) do if x:lower() == "case.txt" then '
            'seen[#seen + 1] = x end end end print("C-NAMES", table.concat(seen, ","))',
        #
        # Five seconds a command, not forty. `pump` waits the whole time
        # rather than stopping at the prompt, so `each` is a real cost per
        # line and these lines are three filesystem calls apiece.
        #
        ], each=5)

        flat = out.replace("\t", " ")

        expected = [
            ("Q-HOME /Home/a.txt,/Home/other/b.txt",
             "a query on a mount that names a subtree returns the paths the "
             "caller can use - not the mount prefix twice over"),
            ("Q-SUB /Home/other/b.txt",
             "and a query asked a folder down answers with that folder's "
             "files"),
            ("Q-DATA /Temporary/c.txt",
             "a mount with no root still works, which is the case that used "
             "to be the only one tested"),
            ("Q-TWO /Temporary/c.txt",
             "a second term narrows rather than widens"),
            ("Q-NONE ",
             "and a value nothing carries finds nothing"),
            ("Q-READ one",
             "the path a query hands back names the file it found"),
        ]

        for marker, what in expected:
            if marker not in flat:
                raise Failure(f"{what}.\nLooked for {marker!r} in:\n"
                              + flat[-1200:])
            checks += 1

        #
        # Names whatever their case: each marker's line, exactly.
        #
        missed = []

        for marker, want, what in [
            ("C-READ", "disk again memory",
             "a file was not found whatever the case of its path, on the disk "
             "and in memory - or a write in another case did not replace it"),
            ("C-LIST", "1 Case.txt",
             "the disk did not keep one file under the name it was made with"),
            ("C-QUERY", "/Home/Case.txt",
             "an attribute set through another case was not indexed under the "
             "file's own spelling, once"),
            ("C-BIN", "application table",
             "/KOSMOS/apps/CLOCK.lua or /devices/CPU was not found whatever its case"),
            ("C-APP", "Cased CASED2",
             "/Running did not take CASED for the name Cased already has"),
            ("C-USE", "true",
             "a library used through two spellings was loaded twice"),
            ("C-RENAME", "true true",
             "a rename to a name's own other case was refused"),
            ("C-NAMES", "CASE.txt,CASE.txt",
             "a rename to another case did not change the name's spelling"),
            ("K-KOSMOS", 'directory Apps,Deskbar,Kits,Libraries,Programs,Themes kit a kit is C, and is used rather '
                         'than read: use("/Kosmos/Kits/pdf")',
             "/Kosmos was not a folder of Kits and Libraries, a kit in it a kit, "
             "and reading one an answer saying to use it"),
            ("K-APPS", "true false true false /Kosmos/Apps/clock.lua "
                       "/Kosmos/Programs/ls.lua /Kosmos/Apps/tracker.lua",
             "the Clock was not in /Kosmos/Apps and ls in /Kosmos/Programs, each "
             "only there, and found by name and by a launcher's old /bin path"),
            ("K-THEMES", "Classic.theme,Endeavour.theme,Night.theme,Plex.theme,"
                         "PlexNight.theme,Studio.theme file library directory 1 0",
             "/Kosmos/Themes did not hold the six looks, a look a file and a "
             "library a library, or /Kosmos/Libraries did not show luacheck as "
             "one folder and no path as a name"),
            ("K-KITS", "3",
             "/Kosmos/Kits did not list the image's kits - pdf, compress, 3d"),
            ("P-MOVED", "true kept across the move directory",
             "a preference at the top of the home was not moved into "
             "/Home/Preferences as the shell started"),
            # The Super Nintendo's cartridges were here until it became an
            # installed application (28 September); `test_filetypes.lua` holds
            # what an installed one opens.
            ("K-OPENS", "png jpg jpeg mp4 mp4 true",
             "an application's `kosmos: opens` did not reach its attributes - "
             "Photo's three, Video's and Play's film - or one that declares "
             "nothing had some"),
            ("K-DESKBAR", "Applications,Demos,Development,Preferences,System launcher "
                          "/Kosmos/Apps/tracker.lua App_Tracker directory "
                          "launcher true true true [Process Viewer] [true] [About This Machine] "
                          "/Kosmos/Apps/browser.lua asset:cheatsheet.html [Cheat Sheet]",
             "/Kosmos/Deskbar was not the shipped menu - its five sections, "
             "Tracker a launcher of its own path and picture, GL Demos a "
             "submenu, no Deskbar and no Info, nothing written into it, and "
             "each application's name for a person carried as its title"),
            ("D-DATED", "integer true",
             "a file written to the disk did not carry the clock's date as "
             "its modified, within five seconds"),
            ("T-BIG", "true 200 true",
             "a table larger than a message was not kept in /Temporary and read "
             "back whole"),
        ]:
            lines = [l for l in flat.splitlines() if l.startswith(marker + " ")]

            # All of them, rather than the first: a control that breaks one
            # mechanism should show every check that stands on it.
            if not lines or lines[-1][len(marker) + 1:].strip() != want:
                missed.append(f"{what}: wanted {marker} {want!r}, got "
                              + (repr(lines[-1]) if lines else "nothing"))
            checks += 1

        #
        # The root: every name the agreed layout gives it that this machine
        # mounts, and none of the ones it replaced.
        #
        lines = [l for l in flat.splitlines() if l.startswith("R-ROOT ")]
        root = set(lines[-1][len("R-ROOT "):].strip().split(",")) if lines else set()
        want_there = {"Home", "Devices", "Running", "Temporary", "Kosmos"}
        gone = {"home", "dev", "app", "ramfs", "net", "drives", "lib", "kits", "bin",
                "system", "user"}

        if not want_there <= root or root & gone:
            missed.append("the root did not list %s and none of %s: got %s"
                          % (sorted(want_there), sorted(gone), sorted(root)))
        checks += 1

        # `cd /home` answers with where it went: the line after the command.
        after = flat.split("kosmos> cd /home", 1)
        went = after[1].split("\n")[1].strip() if len(after) == 2 else None

        if went != "/Home":
            missed.append("cd /home did not answer in the mount's own spelling, "
                          "/Home: got %r" % went)
        checks += 1

        if missed:
            raise Failure(f"{len(missed)} of the checks on names whatever their "
                          "case:\n  " + "\n  ".join(missed))

        #
        # The sixty-four character names. Read back rather than listed alone:
        # a name cut short still lists as something, and only the content says
        # whether it is the file that was written.
        #
        long_name = "n" * 60 + ".txt"

        for marker, content, what in [
            ("L-RAMFS", "deep",
             "a 64-character file inside a 64-character directory in /Temporary "
             "- 136 bytes of path"),
            ("L-HOME", "long", "a 64-character file on the disk"),
        ]:
            lines = [l for l in flat.splitlines() if l.startswith(marker + " ")]

            if not lines or content not in lines[-1].split()[1:2]:
                raise Failure(f"{what} did not read back.\n"
                              + "\n".join(lines or [flat[-1200:]]))

            if marker == "L-RAMFS" and long_name not in lines[-1]:
                raise Failure(f"{what} read back, and its directory does not "
                              "list it under its whole name.\n" + lines[-1])

            checks += 1

        #
        # The long name comes back as what the field keeps - 23 bytes and a
        # terminator - rather than as an error from the process that asked.
        #
        want = "A-LONG " + "t" * 23
        lines = [l for l in flat.splitlines() if l.startswith("A-LONG ")]

        if not lines or lines[-1].strip() != want:
            raise Failure("a 46-byte name registered in /Running did not come "
                          "back as its first 23 bytes.\n"
                          + "\n".join(lines or [flat[-1200:]]))
        checks += 1

        # `pcall` says whether the call returned at all: true for both is an
        # answer from each protocol, false is a raise from inside the kit.
        lines = [l for l in flat.splitlines() if l.startswith("F-LONG ")]

        if not lines or not lines[-1].startswith("F-LONG true true"):
            raise Failure("a name longer than its field at /Devices or /bin raised "
                          "inside the namespace kit instead of being answered.\n"
                          + "\n".join(lines or [flat[-1200:]]))
        checks += 1

        #
        # And the one that would have caught the original bug on its own: a
        # query asked about one part of a disk must not answer with another
        # part, though one server holds both - `/system` against `/Home` when
        # the disk was mounted three times, a folder against its parent now.
        #
        for line in flat.splitlines():
            if line.startswith("Q-SUB") and "/Home/a.txt" in line:
                raise Failure(
                    "a query asked about /Home/other answered with a file "
                    "beside it. A question asked of a folder is about that "
                    "folder.\n" + line)

        checks += 1

        #
        # ---- the file verbs, on the mount that had none -----------------
        #
        # `mkdir`, `delete` and `rename` did not exist in `ramproto.h` at
        # all, because everything that had ever used /Temporary *published* - a
        # replicant writing its own source, the web server writing its
        # status - and nothing took anything back out. An operation with no
        # caller does not get written.
        #
        # What made that a bug rather than an absence is `rm`: a verb that
        # works on one mount and not another is the namespace failing at the
        # one thing it exists for. So this is the same session the disk
        # suite runs, on the other kind of mount, and the answers have to
        # match line for line.
        #
        verbs = run_disk.boot(image, disk, [
            "cd /Temporary",
            "touch alpha.txt",
            "mkdir box",
            "cp alpha.txt box",
            "mv alpha.txt beta.txt",
            "rm box",
            "rm -r box",
            "rm beta.txt",
            "ls",
        ])

        for marker, what in [
            ("made /Temporary/alpha.txt",
             "touch did not make a file in memory"),
            ("made /Temporary/box",
             "mkdir did not make a directory in memory"),
            ("copied to /Temporary/box/alpha.txt",
             "cp did not copy into a directory in memory"),
            ("moved to /Temporary/beta.txt",
             "rename did not move a file in memory"),
            ("is a directory; use -r",
             "a directory with something in it was removed without -r"),
            ("removed 2",
             "rm -r did not remove the directory and what was in it"),
        ]:
            if marker not in verbs:
                raise Failure(f"{what}.\nLooked for {marker!r} in:\n"
                              + verbs[-1200:])
            checks += 1

        # Emptied, and said so by the listing rather than by the verbs'
        # own reports: what is there at the end is the only claim that
        # cannot be made by a program that did nothing.
        if "(empty)" not in verbs.split("rm beta.txt")[-1]:
            raise Failure("what /Temporary holds at the end is not what the "
                          "session did to it.\n" + verbs[-1200:])

        checks += 1

        print(f"PASS: {checks} checks on attributes and the queries over "
              "them, on both kinds of mount - and names found whatever "
              "their case and kept as they were given, on the disk, in "
              "memory, in /bin, /Devices, /Running and a library used twice.")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
