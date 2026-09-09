#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The programs a person types at a prompt, checked as a session.

`make test` proves the servers answer. This proves the *shell* is somewhere
you can work: that a file made at the prompt can be listed, counted,
searched, walked and measured, and that each of those verbs agrees with the
others about the same file.

**Checked against each other rather than against a constant.** `wc` says
five lines, so `head -n 2` and `tail -n 2` have to name the first and last
two of exactly those, and `tree` has to show the file `ls` showed. A suite
that checked each program against a number written here would pass with all
of them wrong in the same direction, which is the failure a session cannot
have.

It runs on `/ramfs`, which needs no disk - and that is the point of choosing
it: these verbs worked on the disk and not here until the mount grew
`mkdir`, `delete` and `rename`, and a test on the mount that was missing
them is the one that would have noticed.
"""

import sys

sys.path.insert(0, __file__.rsplit("/", 1)[0])

import run_disk                                        # noqa: E402


class Failure(Exception):
    pass


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0

    # No disk: `boot` takes one and never formats it, so nothing here can
    # reach a filesystem it did not make itself.
    import tempfile
    handle = tempfile.NamedTemporaryFile(suffix=".img", delete=False)
    handle.truncate(4 * 1024 * 1024)
    handle.close()

    try:
        out = run_disk.boot(image, handle.name, [
            "cd /ramfs",
            "mkdir notes",
            'fs.write("/ramfs/notes/a.txt", "one\\ntwo\\nthree\\nfour\\nfive")',
            "wc notes/a.txt",
            "head -n 2 notes/a.txt",
            "tail -n 2 notes/a.txt",
            "grep three notes/a.txt",
            "grep nothinghere notes/a.txt",
            "tree /ramfs",
            "du /ramfs",
        ])

        for marker, what in [
            ("5 lines  5 words  23 bytes",
             "wc did not count the file it was given"),
            ("no lines match",
             "grep claimed a match that is not in the file"),
            ("1 directories, 2 entries",
             "tree did not walk into the directory"),
        ]:
            if marker not in out:
                raise Failure(f"{what}.\nLooked for {marker!r} in:\n"
                              + out[-1400:])
            checks += 1

        # head and tail name the ends of the same five lines wc counted.
        after_head = out.split("head -n 2 notes/a.txt")[-1].split("kosmos>")[0]

        if "one" not in after_head or "two" not in after_head \
                or "three" in after_head:
            raise Failure("head did not show the first two lines.\n"
                          + after_head)

        checks += 1

        after_tail = out.split("tail -n 2 notes/a.txt")[-1].split("kosmos>")[0]

        if "four" not in after_tail or "five" not in after_tail \
                or "one" in after_tail:
            raise Failure("tail did not show the last two lines.\n"
                          + after_tail)

        checks += 1

        # grep reports the line *number*, and it has to be the right one:
        # "three" is the third of the five.
        after_grep = out.split("grep three notes/a.txt")[-1].split("kosmos>")[0]

        if "3" not in after_grep or "three" not in after_grep:
            raise Failure("grep did not report the matching line and its "
                          "number.\n" + after_grep)

        checks += 1

        # du agrees with wc about how many bytes there are.
        if "23 B" not in out.split("du /ramfs")[-1]:
            raise Failure("du disagrees with wc about the size of the only "
                          "file there is.\n" + out[-1400:])

        checks += 1

        #
        # The cheat sheet, and the two spellings that reach it.
        #
        # `help` names a value in the shell's environment, so a bare
        # `help shell` is Lua and fails - which the overview told people to
        # type for months. Both working spellings are checked here so that
        # the page cannot quietly stop being reachable.
        #
        sheet = run_disk.boot(image, handle.name, [
            "/help shell",
            'help "fs"',
        ])

        for marker, what in [
            ("MOVING AROUND", "the cheat sheet is not reachable as a command"),
            ("WHAT IS DELIBERATELY MISSING",
             "the cheat sheet does not say what this system does not have"),
            ("fs - this process's namespace",
             'help "fs" - the call spelling - does not reach a topic'),
        ]:
            if marker not in sheet:
                raise Failure(f"{what}.\nLooked for {marker!r} in:\n"
                              + sheet[-1200:])
            checks += 1

        #
        # ---- the up-arrow ------------------------------------------------
        #
        # **Checked by what the recalled line does, not by what appears on
        # the screen.** An echo test would pass on a console that printed
        # the text and forgot it; running the line is the only evidence that
        # the editor really replaced what it holds.
        #
        # `boot` sends each string followed by a newline, so an escape
        # sequence here means "walk back, then press enter". Two files with
        # different names make each landing identifiable: a `touch` of one
        # that exists says so, and says *which*.
        #
        # **A recalled line is itself remembered**, which is what the second
        # sequence has to account for - after the first one the newest entry
        # is `touch one.txt` again, so reaching `two` from there is up three
        # and down one. The first version of this check assumed the ring
        # stood still and failed for a reason that was in the test.
        #
        # A serial terminal sends ESC [ A and `hal/virtio/input.c` maps the
        # keyboard's own arrow to the same three bytes, so this covers the
        # graphical console too.
        #
        history = run_disk.boot(image, handle.name, [
            "cd /ramfs",
            "touch one.txt",
            "touch two.txt",
            "\x1b[A\x1b[A",              # up up: touch one.txt
            "\x1b[A\x1b[A\x1b[A\x1b[B",  # up up up down: touch two.txt
        ])

        if "/ramfs/one.txt is already there" not in history:
            raise Failure("the up-arrow did not recall and run the line two "
                          "back.\n" + history[-1500:])

        checks += 1

        if "/ramfs/two.txt is already there" not in history:
            raise Failure("the down-arrow did not walk back towards the line "
                          "being typed.\n" + history[-1500:])

        checks += 1

        #
        # ---- naming and ending things ------------------------------------
        #
        # `kill` is run through `run` with an id found at the prompt rather
        # than a number written here: process ids depend on what started,
        # and a check that hardcoded one would be testing the boot order.
        #
        tools = run_disk.boot(image, handle.name, [
            "which grep",
            "which nosuchthing",
            "cd /ramfs",
            "touch s.txt",
            "stat s.txt",
            "df",
            "say 90 later &",
            "kill say",
            "kill say",
            "ps",
        ])

        for marker, what in [
            ("/bin/grep.lua", "which did not find a program that is there"),
            ("is not in /bin", "which claimed a program that does not exist"),
            ("kind      file", "stat did not report what the node is"),
            ("blocks free of",
             "df did not read the superblock for a real free count"),
            ("ended ", "kill did not end the process it was given"),
        ]:
            if marker not in tools:
                raise Failure(f"{what}.\nLooked for {marker!r} in:\n"
                              + tools[-1500:])
            checks += 1

        # df measured the mounts by asking, so /bin - which is in the image
        # and always there - has to appear with a count.
        if "/bin" not in tools.split("df")[-1]:
            raise Failure("df did not list the mounts it can measure.\n"
                          + tools[-1500:])

        checks += 1

        # And `ps` names processes now, which is where `kill` gets an id
        # from. It printed pool counts and nothing else for a long time.
        if "band" not in tools or "shell" not in tools.split("band")[-1]:
            raise Failure("ps did not list the processes.\n" + tools[-1500:])

        checks += 1

        print(f"PASS: {checks} checks on the shell as a place to work "
              "(a file made at the prompt, then counted, read from both "
              "ends, searched, walked and measured - each verb agreeing "
              "with the others about it).")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
