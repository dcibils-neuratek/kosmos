#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Scratch space for a test, and the promise that it goes when the test does.

**Every temporary file and directory the tools make comes from here**, and
`test_scratch.py` refuses any other way of making one. On 19 September the
Mac's disk filled and `make prepush` died writing a stick: the suites had
left 1515 directories in the temporary directory, 19 GB, because each
`tempfile.mkdtemp` was a promise somebody had to remember to keep - and the
512 MB `/home` test, which forgot, left 595 MB a run. Some tools removed
theirs in a `finally`, most did not, and nothing could tell the two apart.

So a process gets **one directory**, made the first time it asks, and
everything it makes lives under it: `directory(name)` for a working
directory, `path(name)` for a single file. The whole of it is removed when
the process exits - normally, by `sys.exit`, or by an exception that ends
it - which is the one moment every tool reaches without having to be told.

The directory's name is short on purpose. A QEMU monitor is a Unix socket
in it, and macOS refuses a socket path longer than 104 bytes; the
temporary directory's own path is 49 of them on this Mac. Which tool made a
directory is in the `made-by` file inside it, for whoever finds one left
behind - and `gate.py` looks, after every run, and fails the run if it does.
"""

import atexit
import os
import shutil
import sys
import tempfile

PREFIX = "kosmos-"

_root = None


def _remove_root():
    if _root is not None:
        shutil.rmtree(_root, ignore_errors=True)


def root():
    """This process's scratch directory, made on first use."""
    global _root

    if _root is None:
        _root = tempfile.mkdtemp(prefix=PREFIX)
        atexit.register(_remove_root)

        with open(os.path.join(_root, "made-by"), "w") as f:
            f.write(" ".join(sys.argv) + "\n")

    return _root


def directory(name="work"):
    """A new, empty directory of its own inside this process's scratch."""
    return tempfile.mkdtemp(prefix=name + "-", dir=root())


def path(name):
    """Where a file called `name` goes in this process's scratch. Not made:
    the caller writes it, and the same name twice is the same file."""
    return os.path.join(root(), name)


def disk(name, size):
    """An empty disk image of `size` bytes, as `path(name)`: sparse, so a
    64 MB disk costs what is written to it."""
    at = path(name)

    with open(at, "wb") as f:
        f.truncate(size)

    return at


def made_by(name, where=None):
    """What made a leftover called `name`, as its `made-by` file says."""
    try:
        with open(os.path.join(where or tempfile.gettempdir(), name,
                               "made-by")) as f:
            return f.read().strip()
    except OSError:
        return None


def leftovers(where=None):
    """The names in the temporary directory that look like this module's,
    for `gate.py` to compare before and after a run."""
    where = where or tempfile.gettempdir()

    try:
        return {n for n in os.listdir(where) if n.startswith(PREFIX)}
    except OSError:
        return set()
