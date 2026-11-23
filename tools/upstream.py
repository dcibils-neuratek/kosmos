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
    # **LLVM 23.1.2 whole** (`docs/gpu3d.md` G1): the release this Mac's
    # clang is and the C++ runtimes were vendored from (`tools/llvm_vendor.py`,
    # the same tarball and GitHub's digest for it), for LLVM's own libraries
    # - its code generation for both processors and the ORC JIT - which
    # Mesa's lavapipe compiles shaders with. Only `parts` are unpacked: the
    # release is two gigabytes of clang, MLIR, Flang and tests, and none of
    # those is built here.
    "llvm": {
        "url": "https://github.com/llvm/llvm-project/releases/download/"
               "llvmorg-23.1.2/llvm-project-23.1.2.src.tar.xz",
        "sha256": "c98bbef08a2b4c2613cd50e9aa9ae7b69b1fe6c16b2c40373bc0ab6116fdf78a",
        "top": "llvm-project-23.1.2.src",
        "parts": ["llvm", "cmake", "third-party", "libc", "LICENSE.TXT"],
        "leave": ["libc/test", "libc/docs", "libc/benchmarks", "libc/fuzzing",
                  "libc/utils", "llvm/test", "llvm/unittests", "llvm/docs", "llvm/benchmarks",
                  "llvm/utils/gn", "llvm/utils/lit/tests", "third-party/benchmark",
                  "third-party/unittest"],
    },
    # **Mesa 26.2.5** (`docs/gpu3d.md` G1, decision 8): the release the
    # M700's assembler corpus was read from (`tools/eu_corpus.py`), its
    # sum from the release notes on docs.mesa3d.org. Its Vulkan runtime,
    # NIR and `util` first, then lavapipe (G2); not its tests or docs.
    "mesa": {
        "url": "https://archive.mesa3d.org/mesa-26.2.5.tar.xz",
        "sha256": "794b3d1729075dae7ebd47c7283b7ca0cdb5dec668f7f11cc0a5d82f6138a0a5",
        "top": "mesa-26.2.5",
    },
    # **The Media Kit's FFmpeg** (`docs/media.md` M1): 9.0.2, the release
    # the old `tools/ffmpeg_vendor.py` vendored H.264 and AAC from, built
    # whole here - its demuxers, decoders and assembly - and the closure it
    # replaced left in M6d (Diego, 14 November 2026).
    "ffmpeg": {
        "url": "https://ffmpeg.org/releases/ffmpeg-9.0.2.tar.xz",
        "sha256": "8c3850283eb25fa026482078a04051e0be17347b09ef81a0849bec15a96e002e",
        "top": "ffmpeg-9.0.2",
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
    # **HTTP/2 for curl** (`docs/w5.md`; Diego, 16 November 2026: "also
    # lets add http/2"): nghttp2 1.71.0, its library only, the GitHub
    # release with its digest.
    "nghttp2": {
        "url": "https://github.com/nghttp2/nghttp2/releases/download/v1.71.0/"
               "nghttp2-1.71.0.tar.xz",
        "sha256": "c0f55121d88650e4b9227aec28cebaf664d2417ca60fe70be0f8d38829f52116",
        "top": "nghttp2-1.71.0",
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
    # **W5f, the web's other formats** (`docs/w5.md`; Diego, 14 November
    # 2026: "we need to have all decoders needed available"). Brotli 1.2.1,
    # for WOFF2 and for HTTP's `br`; Google's woff2 1.0.2, its newest tag.
    "brotli": {
        "url": "https://github.com/google/brotli.git",
        "commit": "db24a98aed38a16de78cc05ca15cdd8dc2a0d92d",
    },
    "woff2": {
        "url": "https://github.com/google/woff2.git",
        "commit": "1bccf208bca986e53a647dfe4811322adb06ecf8",
    },
    # dav1d 1.5.4, VideoLAN's AV1 decoder (GitHub's mirror of its tag), for
    # AVIF's pictures and AV1's video alike; libavif 1.4.2 on it.
    "dav1d": {
        "url": "https://github.com/videolan/dav1d.git",
        "commit": "54706fc6bc0cdecab7e9593974a4039cc038fca7",
    },
    "libavif": {
        "url": "https://github.com/AOMediaCodec/libavif.git",
        "commit": "c5240fc79fe5c2407e10afd35f5505ef6333ea49",
    },
    # **Quake3e** (`docs/quake3e.md`): ec-'s Quake III Arena engine, with
    # its Vulkan renderer, at main on 20 November 2026. It publishes no
    # release that carries the renderer as built here, so a commit.
    "quake3e": {
        "url": "https://github.com/ec-/Quake3e.git",
        "commit": "dec793de9677bc9c16c7027e8905c2d4f4643a19",
    },
    # **ioquake3's game code** (`docs/quake3e.md` Q3): the game, cgame and
    # q3_ui of id's GPL release, kept building by ioquake3, at main on 21
    # November 2026 - compiled native into `quake3.elf`, since the demo's
    # own QVMs (1999) speak a user interface older than any 1.32 engine.
    "ioq3": {
        "url": "https://github.com/ioquake/ioq3.git",
        "commit": "52088b9eae247d8e321c8dc1fb49a879652854f9",
    },
    # **llama.cpp's Vulkan shaders** (`docs/xavier.md` X12, V2), MIT: the
    # GLSL of its Vulkan backend and nothing else of it, at the commit its
    # release tag `b12205` names - the release the LLM Kit is already held
    # to (`LLAMA_CPP` in the Makefile). Compiled for the LLM Kit by
    # `tools/vkshaders.py`.
    "ggml-vulkan": {
        "url": "https://github.com/ggml-org/llama.cpp.git",
        "commit": "a691b2345844ce1b430201fc5fd404ce062e05f2",
        "sparse": ["/ggml/src/ggml-vulkan/vulkan-shaders/", "/LICENSE"],
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


# **The Vulkan conformance suite** (`docs/gpu3d.md` G2): Khronos's VK-GL-CTS
# at the commit its release tag `vulkan-cts-1.4.7.1` names, and the three of
# its externals the Vulkan tests compile with, each at the commit that
# release's `external/fetch_sources.py` pins - glslang for the shaders it
# writes in GLSL, SPIRV-Tools to assemble and validate SPIR-V, SPIR-V's
# headers, and three its CMake adds whatever is built: Amber (a test
# language some groups are written in), jsoncpp (the framework reads and
# writes JSON with it) and Vulkan SC's pipeline-cache utilities. The rest -
# the validation layers, video's samples and generator - are for tests
# Kosmos does not build. A commit is the publisher's sum, as test262's is.
CTS = {
    "vk-gl-cts": {
        "url": "https://github.com/KhronosGroup/VK-GL-CTS.git",
        "commit": "0d18b347943e5e843529a05c95220e27fe30c538",
    },
    "glslang": {
        "url": "https://github.com/KhronosGroup/glslang.git",
        "commit": "2ff6f609379ce43c4291c732cf6a19dd2461a680",
    },
    "spirv-tools": {
        "url": "https://github.com/KhronosGroup/SPIRV-Tools.git",
        "commit": "1d0401cd2b68ae34cda9ff625bedd1be4ed6214a",
    },
    "spirv-headers": {
        "url": "https://github.com/KhronosGroup/SPIRV-Headers.git",
        "commit": "cb42dec3830d3ac67fa449ecdc0c0f73d5e74498",
    },
    "amber": {
        "url": "https://github.com/google/amber.git",
        "commit": "c9f1e2871ccbb0b0564c4a33e359801144f6b7c1",
    },
    "jsoncpp": {
        "url": "https://github.com/open-source-parsers/jsoncpp.git",
        "commit": "89e2973c754a9c02a49974d839779b151e95afd6",
    },
    "vulkansc-pcutil": {
        "url": "https://github.com/KhronosGroup/VulkanSC-pcutil.git",
        "commit": "f67d9e8e1c04f179ebeb837d21ab06192ad43fa0",
    },
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
# W6c's: LayoutTests with text, set in Ahem - every test at the commit whose
# render tree is one file for every port (no port overrides it), whose page
# names Ahem by family, and which needs nothing a page would load or run:
# no script, no picture, no style sheet or font from a file, no form
# control. Ahem's glyphs are squares of the em, so these trees measure the
# same on every port. Their pictures are not compared: a glyph's pixels are
# the font engine's. Left out: `fast/text/emphasis-overlap.html`, which has
# one space in the default font - a serif with Times's metrics on every
# port's test fonts, which Kosmos's folder does not have.
WEBKIT_TEXT_TESTS = [
    "fast/ruby/floating-ruby-text.html",
    "fast/ruby/overhang-horizontal.html",
    "fast/ruby/overhang-horizontal-no-overlap1.html",
    "fast/ruby/overhang-horizontal-no-overlap2.html",
    "fast/ruby/overhang-vertical.html",
    "fast/ruby/overhang-vertical-mixed.html",
    "fast/ruby/overhang-vertical-no-overlap1.html",
    "fast/ruby/overhang-vertical-no-overlap2.html",
    "fast/ruby/overhang-vertical-no-overlap2-mixed.html",
    "fast/ruby/position-after.html",
    "fast/ruby/positioned-ruby-text.html",
    "fast/table/overflow-table-collapsed-borders-cell-painting.html",
    "fast/table/overflow-table-collapsed-borders-cell-painting-table-self-painting-layer.html",
    "fast/table/overflow-table-collapsed-borders-section-layer-painting.html",
    "fast/table/overflow-table-collapsed-borders-section-layer-table-self-painting-layer.html",
    "fast/table/overflow-table-collapsed-borders-section-self-painting-layer-painting.html",
    "fast/table/overflow-table-collapsed-borders-section-self-painting-layer-table-self-painting-layer.html",
    "fast/text/apply-start-width-after-skipped-text.html",
    "fast/text/bidi-embedding-pop-and-push-same-2.html",
    "fast/text/bidi-explicit-embedding-past-end.html",
    "fast/text/emphasis-avoid-ruby-001.html",
    "fast/text/justify-padding-distribution.html",
    "fast/text/soft-hyphen-2.html",
    "fast/text/soft-hyphen-3.html",
    "fast/text/whitespace/017.html",
]

# W6d's: the same kind of test, whose page runs a script or loads a file
# beside it - a picture - and whose tree has no text but Ahem's. Their
# scripts change the page as it loads; the pictures come through the
# harness's loader.
WEBKIT_SCRIPT_TESTS = [
    "fast/block/basic/quirk-percent-height-grandchild.html",
    "fast/block/float/float-not-removed-from-next-sibling.html",
    "fast/block/float/float-not-removed-from-next-sibling2.html",
    "fast/block/float/float-on-zero-height-line.html",
    "fast/block/float/float-overflow-hidden-containing-block-width.html",
    "fast/block/positioning/complex-positioned-movement.html",
    "fast/block/positioning/move-with-auto-width.html",
    "fast/block/positioning/rtl-fixed-positioning.html",
    "fast/block/positioning/static-inline-position-dynamic.html",
    "fast/borders/borderRadiusDashed05.html",
    "fast/css/contentDiv.html",
    "fast/css/contentDivWithChildren.html",
    "fast/css/contentImage.html",
    "fast/css/first-letter-set-text.html",
    "fast/css/style-tag-display-none.html",
    "fast/multicol/span/before-child-anonymous-column-block.html",
    "fast/ruby/float-overhang-from-ruby-text.html",
    "fast/ruby/ruby-block-style-not-updated.html",
    "fast/ruby/ruby-inline-style-not-updated.html",
    "fast/ruby/ruby-text-before-child-split.html",
    "fast/table/table-anonymous-cell-bug.html",
    "fast/table/table-anonymous-row-bug.html",
    "fast/table/table-anonymous-section-bug.html",
    "fast/table/table-cell-split.html",
    "fast/table/table-insert-before-non-anonymous-block.html",
    "fast/table/table-row-before-after-content-around-table-cell.html",
    "fast/table/table-row-split.html",
    "fast/table/table-row-split2.html",
    "fast/table/table-section-split.html",
    "fast/table/table-section-split-with-after-content.html",
    "fast/table/table-section-split2.html",
    "fast/table/table-split.html",
    "fast/table/table-split2.html",
    "fast/text/decorations-transformed.html",
    "fast/text/selection-painted-separately.html",
    "fast/text/setData-dirty-lines.html",
    "fast/text/splitText-dirty-lines.html",
]
WEBKIT_TEST_FILES = ["fast/css/resources/show-shoes-vs-pie.png"]

# W5f's in WebCore: WebKit's reference tests of WOFF2 and AVIF, each page
# painted beside its `-expected.html` - the same font as TrueType, the same
# green as a box - and the two held pixel for pixel. Both are Kosmos's
# paintings, so no port's pixels come into it; GTK and WPE pass all four.
WEBKIT_REF_TESTS = [
    "fast/text/woff2.html",
    "fast/text/woff2-totalsfntsize.html",
    "fast/images/avif-as-image.html",
    "fast/images/avif-heif-container-as-image.html",
]
WEBKIT_REF_FILES = [
    "fast/text/resources/ahem.woff2",
    "fast/text/resources/header-totalsfntsize-001.woff2",
    "fast/text/resources/header-totalsfntsize-001.ttf",
    "fast/images/resources/green-313x313.avif",
    "fast/images/resources/green-400x400.avif",
]

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
    ] + [
        # W6c's: the same fourteen as GTK's port paints them - its
        # expected pictures, which Kosmos's are held to pixel for pixel,
        # since both draw with the same Skia and these hold no text.
        "/LayoutTests/platform/gtk/css2.1/20110323/abspos-containing-block-initial-%s-expected.png"
        % name
        for name in ("004a", "004b", "004c", "004d", "004e", "004f",
                     "005a", "005b", "005c", "005d", "009a", "009b", "009e", "009f")
    ] + [
        "/LayoutTests/" + t.rsplit(".", 1)[0] + ext
        for t in WEBKIT_TEXT_TESTS + WEBKIT_SCRIPT_TESTS
        for ext in ("." + t.rsplit(".", 1)[1], "-expected.txt")
    ] + ["/LayoutTests/" + f for f in WEBKIT_TEST_FILES] + [
        "/LayoutTests/" + t.rsplit(".", 1)[0] + ext
        for t in WEBKIT_REF_TESTS for ext in (".html", "-expected.html")
    ] + ["/LayoutTests/" + f for f in WEBKIT_REF_FILES],
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

    if os.path.exists(stamp) and open(stamp).read() == unpacked_as(name):
        return into

    shutil.rmtree(into, ignore_errors=True)
    os.makedirs(into)

    if path.endswith(".zip"):
        with zipfile.ZipFile(path) as z:
            z.extractall(into)
    else:
        # A release named with `parts` is unpacked only those, less what it
        # says to `leave`: the paths are the release's, under its top.
        release = RELEASES[name]
        command = ["tar", "-xf", path, "-C", into]
        top = release.get("top")
        for left in release.get("leave", []):
            command += ["--exclude", "%s/%s" % (top, left)]
        command += ["%s/%s" % (top, part) for part in release.get("parts", [])]
        subprocess.run(command, check=True)

    with open(stamp, "w") as f:
        f.write(unpacked_as(name))

    return into


def unpacked_as(name):
    """What an unpacking is of: the tarball's sum, and the parts taken
    and left when a release names them - so naming another part unpacks
    again, as a new tarball does."""
    release = RELEASES[name]
    said = digest(release)[1]
    if "parts" in release:
        said += " " + " ".join(release["parts"]) + " - " + " ".join(release.get("leave", []))
    return said


def where(name):
    top = RELEASES[name]["top"]
    into = os.path.join(UNPACKED, name)
    return os.path.join(into, top) if top else into


def patched_copy(release, dest, want, patches, port=None, overlay=None, who="upstream"):
    """
    **A release's copy with Kosmos's changes in it**, for a build to run on:
    `release` cloned file by file into `dest` (free on this Mac's file
    system until a file changes), `overlay(cut)` given the copy first if
    there is one, every `.patch` in `patches` applied in order, and the
    files under `port` - Kosmos's own sources for a platform the release
    has none of - laid in at their places. `want` is the fingerprint of all
    that, and a copy whose `.patched` says it is cut again.

    **Cut beside the copy, then only what differs is carried into it**
    (`docs/w7.md` W7a): a copy cut again from nothing gives every file a
    new time, and ninja then builds all of WebKit again - an hour and more
    - for one port file. So the new copy is cut at `dest.next`, and rsync,
    comparing contents rather than times, writes into `dest` only the
    files whose bytes changed, and removes what went.

    WebKit's, LLVM's and Mesa's builds all use it.
    """
    stamp = os.path.join(dest, ".patched")
    if os.path.exists(stamp) and open(stamp).read() == want:
        return dest

    cut = dest + ".next"
    shutil.rmtree(cut, ignore_errors=True)
    os.makedirs(os.path.dirname(dest), exist_ok=True)
    subprocess.run(["cp", "-Rc", release, cut], check=True)

    if overlay:
        overlay(cut)

    for name in sorted(os.listdir(patches)):
        if name.endswith(".patch"):
            with open(os.path.join(patches, name), "rb") as f:
                r = subprocess.run(["patch", "-s", "-p1", "-d", cut], stdin=f)
            if r.returncode != 0:
                sys.exit("%s: %s does not apply" % (who, name))

    for path in port_files(port):
        os.makedirs(os.path.dirname(os.path.join(cut, path)), exist_ok=True)
        shutil.copy2(os.path.join(port, path), os.path.join(cut, path))

    if os.path.isdir(dest):
        r = subprocess.run(["rsync", "-rl", "--checksum", "--delete",
                            "--exclude", "/.patched", cut + "/", dest + "/"])
        if r.returncode != 0:
            sys.exit("%s: the new copy did not carry into %s" % (who, dest))
        shutil.rmtree(cut)
    else:
        os.rename(cut, dest)

    with open(stamp, "w") as f:
        f.write(want)
    return dest


def port_files(port):
    """Kosmos's own files for a release, by their place in its tree."""
    found = []
    if port and os.path.isdir(port):
        for folder, _, names in os.walk(port):
            for name in names:
                found.append(os.path.relpath(os.path.join(folder, name), port))
    return sorted(found)


def patches_digest(h, patches, port=None):
    """Every patch and port file, by name and bytes, into the hash `h`."""
    for name in sorted(os.listdir(patches)):
        if name.endswith(".patch"):
            h.update(name.encode())
            with open(os.path.join(patches, name), "rb") as f:
                h.update(f.read())
    for path in port_files(port):
        h.update(path.encode())
        with open(os.path.join(port, path), "rb") as f:
            h.update(f.read())
    return h


def main():
    names = list(RELEASES) + list(GIT)
    if len(sys.argv) != 3 or sys.argv[1] not in ("fetch", "unpack", "path") \
            or sys.argv[2] not in names:
        sys.exit("usage: upstream.py fetch|unpack|path %s" % "|".join(names))

    action, name = sys.argv[1], sys.argv[2]

    if name in GIT:
        print(checkout(name, GIT[name]["url"], GIT[name]["commit"], GIT[name].get("sparse")))
    elif action == "fetch":
        print(fetch(name))
    elif action == "unpack":
        unpack(name)
        print(where(name))
    else:
        print(where(name))


if __name__ == "__main__":
    main()
