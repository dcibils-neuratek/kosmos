#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**The C++ runtimes' source, with Kosmos's changes in it**: the vendored
LLVM runtimes (`runtime/upstream/llvm/`, never edited) cloned file by file
into `build/cxx/tree/` - free on this Mac's file system until a file
changes - with `runtime/patches/cxx/` applied in order, as LLVM's own build
does with its patches (`tools/llvm_build.py`). `make cxx-runtimes`
configures from the copy.

    python3 tools/cxx_source.py        # prints where the copy is

The copy is cut again when the vendored tree or a patch changes, and only
the files whose bytes differ are carried into it, so ninja rebuilds what a
patch touched and nothing else.
"""

import hashlib
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)

import upstream                                              # noqa: E402

VENDORED = os.path.join(ROOT, "runtime", "upstream", "llvm")
PATCHES = os.path.join(ROOT, "runtime", "patches", "cxx")
SRC = os.path.join(ROOT, "build", "cxx", "tree")


def fingerprint():
    """The vendored tree as git holds it, and every patch, in order. A
    vendored file changed and not committed is refused rather than built."""
    changed = subprocess.run(["git", "-C", ROOT, "status", "--porcelain", "--", VENDORED],
                             capture_output=True, text=True, check=True).stdout.strip()
    if changed:
        sys.exit("cxx_source: runtime/upstream/llvm/ has changed - it is vendored:\n" + changed)
    tree = subprocess.run(["git", "-C", ROOT, "rev-parse", "HEAD:runtime/upstream/llvm"],
                          capture_output=True, text=True, check=True).stdout.strip()
    return upstream.patches_digest(hashlib.sha256(tree.encode()), PATCHES).hexdigest()


def main():
    print(upstream.patched_copy(VENDORED, SRC, fingerprint(), PATCHES, who="cxx_source"))


if __name__ == "__main__":
    main()
