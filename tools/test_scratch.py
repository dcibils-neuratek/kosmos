#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`scratch.py`: a tool's temporary files go when the tool does.

Two halves, because the leak it exists for had two causes (`scratch.py`).

**Nothing in `tools/` makes a temporary file any other way.** A
`tempfile.mkdtemp` somewhere is a promise to remember a `finally`, and the
19 GB that filled the disk on 19 September was many of those promises not
kept. The scanner is run on a file of its own first, holding each way, so a
scanner that stopped seeing them could not pass.

**And what scratch makes is gone after the process ends** - normally, by
`sys.exit` with a failure's code, and by an exception nobody caught, which
is how a suite that fails usually ends. Each is a real Python process with
its own temporary directory, empty before and checked empty after. Then
`os._exit`, which skips everything a process does on its way out: that one
must be left behind, and `leftovers` and `made_by` must find it and say
what made it - which is what `gate.py` relies on to name a tool killed
before it could tidy up.
"""

import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import scratch  # noqa: E402

# Every way the standard library has to make a temporary file or say where
# they go. `scratch.py` is the one file allowed them.
FORBIDDEN = re.compile(r"\btempfile\.(mkdtemp|mkstemp|mktemp|NamedTemporaryFile"
                       r"|TemporaryFile|TemporaryDirectory|SpooledTemporaryFile"
                       r"|gettempdir)\b")


def offenders(paths):
    found = []

    for p in paths:
        with open(p, errors="replace") as f:
            for number, line in enumerate(f, 1):
                if FORBIDDEN.search(line) and not line.lstrip().startswith("#"):
                    found.append("%s:%d: %s" % (os.path.relpath(p, HERE),
                                                number, line.strip()))

    return found


def main():
    checks, fails = 0, []

    def check(ok, complaint):
        nonlocal checks
        if ok:
            checks += 1
        else:
            fails.append(complaint)

    # 1. The scanner sees each way, on a file of its own.
    decoy = scratch.path("decoy.py")

    with open(decoy, "w") as f:
        f.write("import tempfile\n"
                "a = tempfile.mkdtemp()\n"
                "b = tempfile.NamedTemporaryFile(delete=False)\n"
                "c = tempfile.TemporaryDirectory()\n"
                "d = tempfile.mkstemp()\n"
                "e = tempfile.gettempdir()\n"
                "# tempfile.mkdtemp() in a comment is only talk\n")

    seen = offenders([decoy])
    check(len(seen) == 5,
          "the scanner found %d of the 5 ways in its own decoy: %r"
          % (len(seen), seen))

    # 2. And none of them in the tools.
    tools = [os.path.join(HERE, n) for n in sorted(os.listdir(HERE))
             if n.endswith(".py") and n not in ("scratch.py", "test_scratch.py")]
    found = offenders(tools)
    check(not found,
          "a tool makes a temporary file without scratch.py, which is how 19 "
          "GB were left behind - use scratch.directory, scratch.path or "
          "scratch.disk:\n  " + "\n  ".join(found))

    # 3. What scratch makes is gone after each way a process ends.
    endings = {
        "at its end": "",
        "by sys.exit(3)": "sys.exit(3)",
        "by an exception": "raise RuntimeError('a suite that failed')",
    }
    body = ("import sys\n"
            "sys.path.insert(0, %r)\n"
            "import scratch\n"
            "w = scratch.directory('work')\n"
            "open(w + '/inside', 'w').write('x')\n"
            "scratch.disk('d.img', 1 << 20)\n"
            "open(scratch.path('f'), 'w').write('y')\n"
            "print(scratch.root())\n" % HERE)

    for how, ending in endings.items():
        tmp = scratch.directory("tmp")
        env = dict(os.environ, TMPDIR=tmp)
        ran = subprocess.run([sys.executable, "-c", body + ending + "\n"],
                             env=env, capture_output=True, text=True)
        made = ran.stdout.strip()

        check(made.startswith(os.path.join(os.path.realpath(tmp), "kosmos-"))
              or made.startswith(os.path.join(tmp, "kosmos-")),
              "a process ending %s did not make its scratch in its own "
              "temporary directory: %r %r" % (how, made, ran.stderr[-300:]))
        check(os.listdir(tmp) == [],
              "a process ending %s left %r behind" % (how, os.listdir(tmp)))

    # 4. `os._exit` skips the tidying, so that one stays - and is found,
    #    named by what made it, which is how the gate reports one.
    tmp = scratch.directory("tmp")
    env = dict(os.environ, TMPDIR=tmp)
    subprocess.run([sys.executable, "-c", body + "import os\nos._exit(0)\n",
                    "killed-before-tidying"], env=env, capture_output=True)
    left = scratch.leftovers(tmp)

    check(len(left) == 1,
          "a process that skipped its exit left %r - the gate's check would "
          "see nothing" % (left,))

    if len(left) == 1:
        who = scratch.made_by(left.pop(), tmp) or ""
        check("killed-before-tidying" in who,
              "a leftover's made-by said %r, not the process that made it"
              % who)

    if fails:
        for f in fails:
            print("FAIL: " + f)
        print("scratch: %d checks, %d failed" % (checks + len(fails), len(fails)))
        return 1

    print("scratch: %d checks, all pass" % checks)
    return 0


if __name__ == "__main__":
    sys.exit(main())
