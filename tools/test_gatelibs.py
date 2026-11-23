#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""`gate.py`: a library's suite is skipped only while nothing it stands on
changed (`RUNTIME`; Diego, 13 November 2026).

The fingerprint is the whole of it, so it is held here, in a git repository
made for this test: it must change when a watched file is edited, staged,
committed, removed or added beside the others, and must not when a file
outside what is watched changes - or the skip would be either useless or
wrong. And every library suite in the gate must watch the runtime, or a
change to the POSIX layer would skip the very suites it can break.

**Control**: `KOSMOS_GATELIBS_BREAK=1` fingerprints only the tracked files,
as a first draft might; an edit not yet committed then goes unseen, and
this test must fail.
"""

import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import gate  # noqa: E402
import scratch  # noqa: E402

failed = []


def check(ok, complaint):
    if not ok:
        failed.append(complaint)


repo = scratch.directory("gatelibs")


def git(*args):
    subprocess.run(["git", "-C", repo, "-c", "user.name=test", "-c",
                    "user.email=test@example.invalid"] + list(args),
                   check=True, capture_output=True)


def write(name, text):
    path = os.path.join(repo, name)
    os.makedirs(os.path.dirname(path), exist_ok=True)

    with open(path, "w") as f:
        f.write(text)


def mark():
    if os.environ.get("KOSMOS_GATELIBS_BREAK") == "1":
        import hashlib
        return hashlib.sha256(subprocess.run(
            ["git", "-C", repo, "ls-files", "-s", "--", "runtime"],
            capture_output=True, check=True).stdout).hexdigest()

    return gate.fingerprint(["runtime"], root=repo, toolchain=[])


git("init", "-q")
write("runtime/posix/a.c", "int a;\n")
write("kernel/k.c", "int k;\n")
git("add", "-A")
git("commit", "-q", "-m", "one")

first = mark()
check(mark() == first, "the same tree gave two fingerprints")

write("kernel/k.c", "int k2;\n")
check(mark() == first, "a file outside what is watched changed the fingerprint")

write("runtime/posix/a.c", "int b;\n")
edited = mark()
check(edited != first, "an edit not yet committed went unseen")

git("add", "-A")
check(mark() != first, "an edit staged went unseen")

git("commit", "-q", "-m", "two")
committed = mark()
check(committed != first, "an edit committed went unseen")

write("runtime/posix/new.c", "int n;\n")
check(mark() != committed, "a file added beside the others went unseen")
os.remove(os.path.join(repo, "runtime/posix/new.c"))
check(mark() == committed, "a file added and taken away again left its mark")

os.remove(os.path.join(repo, "runtime/posix/a.c"))
check(mark() != committed, "a file removed went unseen")

# And the gate's own: every library suite stands on the runtime - every one
# that boots a guest, since the runtime is what a guest's image links. A
# `host-` suite is built here, on this Mac's own libc, and links none of it;
# it names the headers of ours it includes instead (`host-llm`, 23 November).
for s in gate.SUITES:
    if s.watch and not s.name.startswith("host-"):
        check("runtime" in s.watch and "user/init" in s.watch,
              "%s does not watch the runtime" % s.name)

if failed:
    print("FAIL: %d checks on a library suite's fingerprint:" % len(failed))
    for f in failed:
        print("  " + f)
    sys.exit(1)

print("PASS: a library suite's fingerprint sees edits, stages, commits, new "
      "and removed files under what it watches, and nothing outside it")
