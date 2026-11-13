#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""
**Source fetched, not committed** (README's decision log, 11 November
2026; `docs/w4.md`): the release tarballs JetSki's ports build from, each
named with where it comes from and the SHA-256 its publisher gives for it.

    python3 tools/upstream.py fetch icu         # into build/downloads/, checked
    python3 tools/upstream.py unpack icu        # and into build/upstream/<name>/
    python3 tools/upstream.py path icu          # where it was unpacked

A tarball whose sum is not the one below is refused and removed, before
anything reads it: the sum is the record of exactly what was used, and the
only thing the tree keeps of a release. A new release is a new line here,
its sum copied from the publisher's page, never computed from the download.
Unpacking is repeated only when the tarball changed; what is unpacked is
never edited, as nothing vendored is - patches are files the build applies.
"""

import hashlib
import os
import shutil
import subprocess
import sys
import urllib.request
import zipfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DOWNLOADS = os.path.join(ROOT, "build", "downloads")
UNPACKED = os.path.join(ROOT, "build", "upstream")

RELEASES = {
    # ICU 4C 79.1, 22 October 2026: GitHub's digests for the release assets.
    "icu": {
        "url": "https://github.com/unicode-org/icu/releases/download/release-79.1/"
               "icu4c-79.1-sources.tgz",
        "sha256": "06fa63ecb521048f8082f12ad988f6b678d90cbd88aafce24cbc94902b191d99",
        "top": "icu",
    },
    # Its data, built by ICU and little-endian, as both of Kosmos's
    # processors are: `icudt79l.dat` inside.
    "icu-data": {
        "url": "https://github.com/unicode-org/icu/releases/download/release-79.1/"
               "icu4c-79.1-data-bin-l.zip",
        "sha256": "72489fb316a60ae1c1da205866edd01b17e8fbf8354ad0615e6cc60f74e08811",
        "top": None,
    },
    # WebKitGTK 2.54.2, 23 October 2026, the sum from the `.sums` file
    # webkitgtk.org publishes beside it. It carries the JSC-only port.
    "webkit": {
        "url": "https://webkitgtk.org/releases/webkitgtk-2.54.2.tar.xz",
        "sha256": "a76345b391bef6211fb5ab6cfab2e86b285d18cda17cbb9ade595f1252b86065",
        "top": "webkitgtk-2.54.2",
    },
    # **W5a's pictures** (`docs/w5.md`). zlib 1.4, 5 November 2026, and
    # libjpeg-turbo 3.2.1, 27 October 2026: GitHub's digests for the
    # release assets, as ICU's are.
    "zlib": {
        "url": "https://github.com/madler/zlib/releases/download/v1.4/zlib-1.4.tar.gz",
        "sha256": "02dd159783677a749c04adbdd02b55b8a2bcc7e054f51b76b549199a916ad584",
        "top": "zlib-1.4",
    },
    "libjpeg-turbo": {
        "url": "https://github.com/libjpeg-turbo/libjpeg-turbo/releases/download/3.2.1/"
               "libjpeg-turbo-3.2.1.tar.gz",
        "sha256": "c27ad72a68da151177929aa969ae773a2da5ac2af9ce946079314fdac8cece7f",
        "top": "libjpeg-turbo-3.2.1",
    },
    # **W5b's text**: HarfBuzz 15.1.0, 4 November 2026, GitHub's digest.
    "harfbuzz": {
        "url": "https://github.com/harfbuzz/harfbuzz/releases/download/15.1.0/"
               "harfbuzz-15.1.0.tar.xz",
        "sha256": "0fc7c565443a5f4be7da65ac5ee5bd737d9b158dc908708b7689a0a5796b0c8d",
        "top": "harfbuzz-15.1.0",
    },
    # **W5c**: SQLite 3.54.1, the amalgamation that is built and the
    # source tree whose test databases and `fuzzcheck.c` hold it. sqlite.org
    # gives SHA3-256 only, in the product data on its download page.
    "sqlite": {
        "url": "https://www.sqlite.org/2026/sqlite-amalgamation-3540100.zip",
        "sha3_256": "92281c86e2957f9b9305890f9ac04204caa7b677cc868dc326d942689df28e30",
        "top": "sqlite-amalgamation-3540100",
    },
    "sqlite-src": {
        "url": "https://www.sqlite.org/2026/sqlite-src-3540100.zip",
        "sha3_256": "877c6f4996ce7adb368686bce21638de9e3d91534832ad3d9d119037d1bc22ad",
        "top": "sqlite-src-3540100",
    },
    # **W5c**: libxml2 2.15.4, WebCore's XML and XSLT's parser, with the
    # test trees `runtest.c` holds it to. GNOME publishes a SHA-256 beside
    # each tarball.
    "libxml2": {
        "url": "https://download.gnome.org/sources/libxml2/2.15/libxml2-2.15.4.tar.xz",
        "sha256": "98087fd181d9070724f3fbc65c7377db03038eb92bd882374daff44940138821",
        "top": "libxml2-2.15.4",
    },
    # **W5c**: libpsl 0.23.3, the public suffix list a cookie is held to.
    "libpsl": {
        "url": "https://github.com/rockdaboot/libpsl/releases/download/0.23.3/"
               "libpsl-0.23.3.tar.gz",
        "sha256": "93941f85a1e7bd593fa94f299233cb5dfc91cd144fd9a78a6ceb75001c5b03be",
        "top": "libpsl-0.23.3",
    },
    # **W5d**: OpenSSL 4.1.0, the system's one TLS (`docs/w5.md`), and
    # curl 8.23.0 on it; both GitHub releases with their digests.
    "openssl": {
        "url": "https://github.com/openssl/openssl/releases/download/openssl-4.1.0/"
               "openssl-4.1.0.tar.gz",
        "sha256": "50c17baf54cb008f6ce42d50deb4b28050a1aad711636cabd4b241c7e90cc2fd",
        "top": "openssl-4.1.0",
    },
    "curl": {
        "url": "https://github.com/curl/curl/releases/download/curl-8_23_0/"
               "curl-8.23.0.tar.xz",
        "sha256": "8dadd7d2536a314f982ad05a651b713f426bf7d02cc2c76cffb9796f2218e6c5",
        "top": "curl-8.23.0",
    },
}

# **Releases that are tags**, with no asset GitHub keeps a digest for:
# libpng's and libwebp's are published as tags in their own repositories
# (and as tarballs on hosts that give a signature and no sum). A tag's
# commit is a sum the publisher gives, so each is fetched as test262 is,
# at that commit, and refused unless git says that is what it holds.
GIT = {
    # libpng 1.6.59, the newest of the stable 1.6 line.
    "libpng": {
        "url": "https://github.com/pnggroup/libpng.git",
        "commit": "cd952f49f95bb27154ae77dbb103032d95f6e580",
    },
    # libwebp 1.6.0, its newest release.
    "libwebp": {
        "url": "https://github.com/webmproject/libwebp.git",
        "commit": "4fa21912338357f89e4fd51cf2368325b59e9bd9",
    },
    # FreeType 2.15.0, from its own repository at freedesktop.org: its
    # tarballs on Savannah carry a signature and no sum.
    "freetype": {
        "url": "https://gitlab.freedesktop.org/freetype/freetype.git",
        "commit": "37515cf56ba3349656049edbed89ffc6f6466f2d",
    },
}


# **test262** (`docs/w4.md` W4e) publishes no releases and no sums: what it
# has is commits, and a commit's name is the hash of everything in it, which
# is a sum the publisher gives. So it is fetched by git at that commit, only
# as deep as that one, and refused unless git says that is what it holds.
TEST262 = {
    "url": "https://github.com/tc39/test262.git",
    # 11 November 2026, "Test exotic DateTimeFormat options with Temporal
    # objects": main the day W4e was started.
    "commit": "2e58f87d8350ad51224617a041c6e041d52f8444",
}


# **What WebKitGTK's release does not carry** (`docs/w6.md`, decided by
# Diego 13 November 2026): its tarball is cut for GTK, and WebKit on curl -
# the PlayStation port's shape, which JetSki's follows - needs files it
# leaves out. They come from WebKit's own repository at the commit the
# release's tag names, `webkitgtk-2.54.2`, so they are the same source the
# tarball was cut from; fetched sparse, only these paths, and the commit is
# the sum. `tools/webkit_build.py` lays them over the tarball's copy, and
# refuses one that is already there.
WEBKIT_OVERLAY = {
    "url": "https://github.com/WebKit/WebKit.git",
    "commit": "b95c91b1c0ee43cc86d25ff7af73038f67a88e04",
    # gitignore's patterns, as a sparse checkout reads them.
    "paths": [
        # curl's network code, in WebCore and in WebKit2's processes.
        "/Source/**/curl/",
        # The ports Kosmos's is modelled on: PlayStation's (curl, no GLib)
        # and WPE's (Skia, no toolkit).
        "/Source/cmake/OptionsPlayStation.cmake",
        "/Source/cmake/OptionsWPE.cmake",
        "/Source/**/PlatformPlayStation.cmake",
        "/Source/**/PlatformWPE.cmake",
    ],
}


def checkout(name, url, commit, sparse=None):
    """Where `name` is, fetched by git at `commit` if it is not there yet;
    only the paths `sparse` names, when it names any, and then no file's
    contents but those."""
    into = os.path.join(UNPACKED, name)

    def head():
        r = subprocess.run(["git", "-C", into, "rev-parse", "HEAD"],
                           capture_output=True, text=True)
        return r.stdout.strip() if r.returncode == 0 else None

    if head() != commit:
        shutil.rmtree(into, ignore_errors=True)
        os.makedirs(into)
        print("upstream: fetching %s at %s" % (name, commit))
        steps = [["init", "-q"], ["remote", "add", "origin", url]]
        if sparse:
            steps += [["config", "core.sparseCheckout", "true"],
                      ["sparse-checkout", "set", "--no-cone"] + sparse,
                      ["fetch", "-q", "--depth", "1", "--filter=blob:none", "origin", commit]]
        else:
            steps += [["fetch", "-q", "--depth", "1", "origin", commit]]
        for args in steps + [["checkout", "-q", "FETCH_HEAD"]]:
            subprocess.run(["git", "-C", into] + args, check=True)

    if head() != commit:
        shutil.rmtree(into, ignore_errors=True)
        sys.exit("upstream: %s is not at %s - removed" % (name, commit))

    # A list that grew at the same commit - W6c's LayoutTests one folder at
    # a time - is set again, and git fetches what it now names.
    if sparse:
        have = subprocess.run(["git", "-C", into, "sparse-checkout", "list"],
                              capture_output=True, text=True).stdout.split()
        if have != list(sparse):
            print("upstream: %s's paths changed - checking them out" % name)
            subprocess.run(["git", "-C", into, "sparse-checkout", "set", "--no-cone"] + sparse,
                           check=True)

    # And nothing in the checkout changed since: a modified file would be
    # taken as though it were the publisher's.
    if subprocess.run(["git", "-C", into, "status", "--porcelain"],
                      capture_output=True, text=True).stdout.strip():
        sys.exit("upstream: %s's checkout in %s has been changed" % (name, into))

    return into


# **WebKit's own tests** (`docs/w6.md`, Diego's decision 2A): what a
# page laid out on Kosmos is held to, from WebKit's git at the release's
# commit - apart from the overlay, since they are not WebKit's source and
# a test added here should not build WebKit again. W6b's is the Ahem font
# LayoutTests are written in, whose every glyph is a square of one em;
# W6c's LayoutTests join it.
WEBKIT_TESTS = {
    "url": WEBKIT_OVERLAY["url"],
    "commit": WEBKIT_OVERLAY["commit"],
    "paths": [
        "/LayoutTests/resources/Ahem.ttf",
    ] + [
        # W6b's: CSS 2.1's tests of the initial containing block, the
        # fourteen of that suite whose expected render tree is one file
        # for every port - boxes placed with no text in them.
        "/LayoutTests/css2.1/20110323/abspos-containing-block-initial-%s%s" % (name, ext)
        for name in ("004a", "004b", "004c", "004d", "004e", "004f",
                     "005a", "005b", "005c", "005d", "009a", "009b", "009e", "009f")
        for ext in (".htm", "-expected.txt")
    ],
}


def webkit_tests():
    """Where WebKit's tests are, fetched at the release tag's commit if
    they are not there yet."""
    o = WEBKIT_TESTS
    return checkout("webkit-tests", o["url"], o["commit"], o["paths"])


def webkit_overlay():
    """Where the files WebKitGTK's release lacks are, fetched at the
    release tag's commit if they are not there yet."""
    o = WEBKIT_OVERLAY
    return checkout("webkit-overlay", o["url"], o["commit"], o["paths"])


def test262():
    """Where test262 is, fetched at its commit if it is not there yet."""
    return checkout("test262", TEST262["url"], TEST262["commit"])


def digest(release):
    """The algorithm and the sum a publisher gives: SHA-256 for most, and
    SHA3-256 where that is all the publisher gives, as sqlite.org does."""
    for algorithm in ("sha256", "sha3_256"):
        if algorithm in release:
            return algorithm, release[algorithm]
    raise KeyError("a release names its sum")


def sum_of(path, algorithm):
    h = hashlib.new(algorithm)
    with open(path, "rb") as f:
        for block in iter(lambda: f.read(1 << 20), b""):
            h.update(block)
    return h.hexdigest()


def fetch(name):
    release = RELEASES[name]
    os.makedirs(DOWNLOADS, exist_ok=True)
    path = os.path.join(DOWNLOADS, os.path.basename(release["url"]))

    if not os.path.exists(path):
        print("upstream: fetching %s" % release["url"])
        part = path + ".part"
        with urllib.request.urlopen(release["url"]) as r, open(part, "wb") as f:
            shutil.copyfileobj(r, f)
        os.rename(part, path)

    algorithm, want = digest(release)
    got = sum_of(path, algorithm)

    if got != want:
        os.remove(path)
        sys.exit("upstream: %s has %s %s, not %s - removed"
                 % (os.path.basename(path), algorithm.upper().replace("_", "-"), got, want))

    return path


def unpack(name):
    path = fetch(name)
    into = os.path.join(UNPACKED, name)
    stamp = os.path.join(into, ".unpacked")

    if os.path.exists(stamp) and open(stamp).read() == digest(RELEASES[name])[1]:
        return into

    shutil.rmtree(into, ignore_errors=True)
    os.makedirs(into)

    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            z.extractall(into)
    else:
        subprocess.run(["tar", "-xf", path, "-C", into], check=True)

    with open(stamp, "w") as f:
        f.write(digest(RELEASES[name])[1])

    return into


def where(name):
    top = RELEASES[name]["top"]
    into = os.path.join(UNPACKED, name)
    return os.path.join(into, top) if top else into


def main():
    names = list(RELEASES) + list(GIT)
    if len(sys.argv) != 3 or sys.argv[1] not in ("fetch", "unpack", "path") \
            or sys.argv[2] not in names:
        sys.exit("usage: upstream.py fetch|unpack|path %s" % "|".join(names))

    action, name = sys.argv[1], sys.argv[2]

    if name in GIT:
        print(checkout(name, GIT[name]["url"], GIT[name]["commit"]))
    elif action == "fetch":
        print(fetch(name))
    elif action == "unpack":
        unpack(name)
        print(where(name))
    else:
        print(where(name))


if __name__ == "__main__":
    main()
