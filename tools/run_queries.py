#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Attributes and the queries over them, on a machine with a real disk.

**M7's definition of done was a live query, and nothing in `make test` ever
checked one.** `qbench` measures how fast a query is and `latency.lua`
measures how quickly a watch wakes, and neither of them would notice a
query returning the wrong paths - which is what it had been doing on the
disk for as long as the disk could answer.

The bug this exists to keep out: one disk is mounted three times, at
`/system`, `/user` and `/home`, each naming a subtree of itself. The
namespace maps `/home/doc.pdf` onto `/home/doc.pdf` in the server and put
the mount prefix back on the way out, giving `/home/home/doc.pdf`; and the
server answered a question asked about `/home` with everything on the disk,
`/system` included. Both were invisible because every query test used
`/ramfs`, which is the one mount with no root - so the two paths through
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
        run_interchange.kfs("create", disk, "32")

        #
        # Every answer is printed as one line with a marker, so a check is a
        # string comparison rather than a parse. `table.concat` with a comma
        # keeps the order the server sorted them into, which is part of what
        # is being tested: a query that returned its answers in table order
        # would differ between runs.
        #
        out = run_disk.boot(image, disk, [
            # Two files on the disk and one in memory, so both kinds of
            # mount are exercised by the same run.
            'fs.write("/home/a.txt", "one") '
            'fs.write("/system/b.txt", "two") '
            'fs.write("/ramfs/c.txt", "three")',

            'fs.setattr("/home/a.txt", { kind = "book" }) '
            'fs.setattr("/system/b.txt", { kind = "book" }) '
            'fs.setattr("/ramfs/c.txt", { kind = "book", size = "small" })',

            'print("Q-HOME", table.concat(fs.query("/home", '
            '{ kind = "book" }) or {}, ","))',

            'print("Q-SYSTEM", table.concat(fs.query("/system", '
            '{ kind = "book" }) or {}, ","))',

            'print("Q-DATA", table.concat(fs.query("/ramfs", '
            '{ kind = "book" }) or {}, ","))',

            # Two terms, and the second one is what narrows it.
            'print("Q-TWO", table.concat(fs.query("/ramfs", '
            '{ kind = "book", size = "small" }) or {}, ","))',

            'print("Q-NONE", table.concat(fs.query("/home", '
            '{ kind = "nothing-has-this" }) or {}, ","))',

            # The path a query returns has to be one that can be read back.
            # A doubled prefix is still a string and still looks like an
            # answer; only reading it says whether it names anything.
            'local hit = (fs.query("/home", { kind = "book" }) or {})[1] '
            'print("Q-READ", hit and (fs.read(hit) or "unreadable") or "none")',

            # A sixty-four character name, on both kinds of mount, and in
            # memory a directory down: a 64-character directory holding a
            # 64-character file is 136 bytes of path, which /ramfs's old
            # 128-byte field refused outright.
            'D, N = string.rep("d", 64), string.rep("n", 60) .. ".txt" '
            'fs.send("/ramfs/" .. D, { type = "mkdir" }) '
            'fs.write("/ramfs/" .. D .. "/" .. N, "deep") '
            'fs.write("/home/" .. N, "long")',

            'print("L-RAMFS", fs.read("/ramfs/" .. D .. "/" .. N) or "none", '
            'table.concat(fs.list("/ramfs/" .. D) or {}, ",")) '
            'print("L-HOME", fs.read("/home/" .. N) or "none")',

            # A name longer than /app keeps, registered the way `ui.window`
            # registers a window under its title. The namespace kit used to
            # raise inside `string.pack` before the registry saw it, which
            # killed the process that asked - a window titled with a ROM's
            # No-Intro name was the first to be that long.
            'E = sys.endpoint() '
            'R = fs.send("/app", { type = "register", '
            'name = string.rep("t", 46) }, E) '
            'print("A-LONG", R and R.name or "refused") '
            'if R then fs.send("/app", { type = "unregister", name = R.name }) end',

            # And the rest of that class: a name longer than its field at /dev
            # and at /bin is an answer - no such device, no such program -
            # rather than a raise from `string.pack` inside the namespace kit.
            'print("F-LONG", pcall(fs.getattr, "/dev/" .. string.rep("d", 40)), '
            'pcall(fs.getattr, "/bin/" .. string.rep("b", 80)))',

            # **Names whatever their case, kept as they were given**
            # (`roadmap.md` 6s): one file on the disk and one in memory,
            # written and read however the path is typed, and a write in
            # another case replacing the file and keeping its name.
            'fs.write("/Home/Case.txt", "disk") fs.write("/RAMFS/Case.txt", "memory") '
            'fs.write("/HOME/CASE.TXT", "disk again")',

            'print("C-READ", fs.read("/home/case.txt"), fs.read("/ramfs/CASE.TXT"))',

            'local n, c = 0, nil for _, x in ipairs(fs.list("/home") or {}) do '
            'if x:lower() == "case.txt" then n, c = n + 1, x end end '
            'print("C-LIST", n, c)',

            # The disk's index is keyed by path - it was built by the queries
            # above - so an attribute set through another case has to land
            # under the file's own spelling, once.
            'fs.setattr("/HOME/case.TXT", { kind = "cased" }) '
            'print("C-QUERY", table.concat(fs.query("/Home", { kind = "cased" }) or {}, ","))',

            # The programs, the devices, the registry and a library.
            'print("C-BIN", (fs.getattr("/BIN/CLOCK.lua") or {}).kind, type(fs.read("/Dev/CPU")))',

            'E1, E2 = sys.endpoint(), sys.endpoint() '
            'R1 = fs.send("/App", { type = "register", name = "Cased" }, E1) '
            'R2 = fs.send("/app", { type = "register", name = "CASED" }, E2) '
            'print("C-APP", R1 and R1.name, R2 and R2.name) '
            'fs.send("/app", { type = "unregister", name = R1 and R1.name or "" }) '
            'fs.send("/app", { type = "unregister", name = R2 and R2.name or "" })',

            # `use` is a program's, not the prompt's: a program that asks
            # for one library by two spellings.
            'fs.write("/ramfs/usetwice.lua", "print(\\"C-USE\\", '
            'use(\\"/LIB/Text.lua\\") == use(\\"/lib/text.lua\\"))\\n")',

            'run /ramfs/usetwice.lua',

            # And a name's spelling changed by a rename to itself in another
            # case, on both kinds of mount.
            'print("C-RENAME", fs.send("/home/case.txt", { type = "rename", to = "/home/CASE.txt" }) and true, '
            'fs.send("/ramfs/case.txt", { type = "rename", to = "/ramfs/CASE.txt" }) and true) '
            'local seen = {} for _, m in ipairs({ "/home", "/ramfs" }) do '
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
            ("Q-HOME /home/a.txt",
             "a query on a mount that names a subtree returns the path the "
             "caller can use - not the mount prefix twice over"),
            ("Q-SYSTEM /system/b.txt",
             "and the same disk answers a different mount with that mount's "
             "files"),
            ("Q-DATA /ramfs/c.txt",
             "a mount with no root still works, which is the case that used "
             "to be the only one tested"),
            ("Q-TWO /ramfs/c.txt",
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
            ("C-QUERY", "/home/Case.txt",
             "an attribute set through another case was not indexed under the "
             "file's own spelling, once"),
            ("C-BIN", "application table",
             "/BIN/CLOCK.lua or /Dev/CPU was not found whatever its case"),
            ("C-APP", "Cased CASED2",
             "/app did not take CASED for the name Cased already has"),
            ("C-USE", "true",
             "a library used through two spellings was loaded twice"),
            ("C-RENAME", "true true",
             "a rename to a name's own other case was refused"),
            ("C-NAMES", "CASE.txt,CASE.txt",
             "a rename to another case did not change the name's spelling"),
        ]:
            lines = [l for l in flat.splitlines() if l.startswith(marker + " ")]

            # All of them, rather than the first: a control that breaks one
            # mechanism should show every check that stands on it.
            if not lines or lines[-1][len(marker) + 1:].strip() != want:
                missed.append(f"{what}: wanted {marker} {want!r}, got "
                              + (repr(lines[-1]) if lines else "nothing"))
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
             "a 64-character file inside a 64-character directory in /ramfs "
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
            raise Failure("a 46-byte name registered in /app did not come "
                          "back as its first 23 bytes.\n"
                          + "\n".join(lines or [flat[-1200:]]))
        checks += 1

        # `pcall` says whether the call returned at all: true for both is an
        # answer from each protocol, false is a raise from inside the kit.
        lines = [l for l in flat.splitlines() if l.startswith("F-LONG ")]

        if not lines or not lines[-1].startswith("F-LONG true true"):
            raise Failure("a name longer than its field at /dev or /bin raised "
                          "inside the namespace kit instead of being answered.\n"
                          + "\n".join(lines or [flat[-1200:]]))
        checks += 1

        #
        # And the one that would have caught the original bug on its own: a
        # query asked about `/home` must not answer with what is under
        # `/system`, even though one server holds both.
        #
        for line in flat.splitlines():
            if line.startswith("Q-HOME") and "/system" in line:
                raise Failure(
                    "a query asked about /home answered with files under "
                    "/system. One disk is mounted three times and a question "
                    "asked at one of them is about that subtree.\n" + line)

        checks += 1

        #
        # ---- the file verbs, on the mount that had none -----------------
        #
        # `mkdir`, `delete` and `rename` did not exist in `ramproto.h` at
        # all, because everything that had ever used /ramfs *published* - a
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
            "cd /ramfs",
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
            ("made /ramfs/alpha.txt",
             "touch did not make a file in memory"),
            ("made /ramfs/box",
             "mkdir did not make a directory in memory"),
            ("copied to /ramfs/box/alpha.txt",
             "cp did not copy into a directory in memory"),
            ("moved to /ramfs/beta.txt",
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
            raise Failure("what /ramfs holds at the end is not what the "
                          "session did to it.\n" + verbs[-1200:])

        checks += 1

        print(f"PASS: {checks} checks on attributes and the queries over "
              "them, on both kinds of mount - and names found whatever "
              "their case and kept as they were given, on the disk, in "
              "memory, in /bin, /dev, /app and a library used twice.")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
