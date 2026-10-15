#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""**Alpine's packages, kept on the Mac** (`docs/linux.md` L4): a repository
holding the packages asked for and everything they need, for a sandbox's
`apk` to install from - served from this Mac, so a suite installs Linux
onto its disk without the internet.

Alpine 3.24's signed indexes, `main` and `community`, fetched once; the
packages asked for, with every dependency resolved through the indexes'
names and `provides`, each fetched once and held to the size its index
gives. Laid out as a mirror is - `v3.24/<repo>/<arch>/APKINDEX.tar.gz` and
the packages beside it - so `apk -X http://<here>/v3.24/main` reads it as
it reads Alpine's, and checks every package against the signed index
itself. **Kept as fetched**: a second run uses what is there, so a suite
installs the same packages every time, however Alpine's branch moves on.

Usage: alpine_repo.py <aarch64|x86_64> <package>...    (prints the folder)
"""

import os
import re
import sys
import tarfile
import urllib.request

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
MIRROR = "https://dl-cdn.alpinelinux.org/alpine/"
BRANCH = "v3.24"
REPOS = ("main", "community")


def fetch(url, path):
    if os.path.exists(path):
        return

    os.makedirs(os.path.dirname(path), exist_ok=True)
    urllib.request.urlretrieve(url, path + ".part")
    os.replace(path + ".part", path)


def index(top, arch):
    """{name: record} over both repositories, and {provided: name}."""
    packages, provides = {}, {}

    for repo in REPOS:
        at = os.path.join(top, BRANCH, repo, arch, "APKINDEX.tar.gz")
        fetch("%s%s/%s/%s/APKINDEX.tar.gz" % (MIRROR, BRANCH, repo, arch), at)

        with tarfile.open(at) as t:
            text = t.extractfile("APKINDEX").read().decode("utf-8", "replace")

        for block in text.split("\n\n"):
            record = {"repo": repo}

            for line in block.splitlines():
                if len(line) > 2 and line[1] == ":":
                    record.setdefault(line[0], line[2:])

            if "P" not in record:
                continue

            packages.setdefault(record["P"], record)

            for p in record.get("p", "").split():
                provides.setdefault(re.split(r"[=<>~]", p)[0], record["P"])

    return packages, provides


def closure(packages, provides, wanted):
    """Every package `wanted` needs, by name."""
    seen, stack = set(), list(wanted)

    while stack:
        name = stack.pop()

        if name in seen:
            continue

        if name not in packages:
            sys.exit("alpine_repo: %s is in neither index" % name)

        seen.add(name)

        for dep in packages[name].get("D", "").split():
            if dep.startswith("!"):
                continue

            base = re.split(r"[=<>~]", dep)[0]
            found = base if base in packages else provides.get(base)

            if found is None:
                sys.exit("alpine_repo: nothing provides %s, which %s needs" % (dep, name))

            stack.append(found)

    return sorted(seen)


def main(argv):
    if len(argv) < 3 or argv[1] not in ("aarch64", "x86_64"):
        sys.exit(__doc__)

    arch, wanted = argv[1], argv[2:]
    top = os.path.join(ROOT, "build", "downloads", "alpine-repo")
    packages, provides = index(top, arch)
    names = closure(packages, provides, wanted)
    total = 0

    for name in names:
        record = packages[name]
        file = "%s-%s.apk" % (name, record["V"])
        at = os.path.join(top, BRANCH, record["repo"], arch, file)

        if not os.path.exists(at):
            print("alpine_repo: fetching %s" % file, file=sys.stderr, flush=True)

        fetch("%s%s/%s/%s/%s" % (MIRROR, BRANCH, record["repo"], arch, file), at)

        if os.path.getsize(at) != int(record.get("S", -1)):
            os.remove(at)
            sys.exit("alpine_repo: %s is not the size its index says; removed" % file)

        total += os.path.getsize(at)

    print("alpine_repo: %d packages, %.1f MB, for %s" % (len(names), total / 1e6, " ".join(wanted)),
          file=sys.stderr)
    print(top)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
