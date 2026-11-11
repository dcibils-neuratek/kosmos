# LLVM's C++ runtimes, vendored

Upstream: <https://github.com/llvm/llvm-project> - release 23.1.2,
`llvm-project-23.1.2.src.tar.xz`, the release this Mac's clang is.
Licence:  Apache 2.0 with LLVM Exceptions. See `LICENSE.TXT`.

Fetched from the release's GitHub page on 11 November 2026 and held to the
SHA-256 GitHub states for the asset:

    c98bbef08a2b4c2613cd50e9aa9ae7b69b1fe6c16b2c40373bc0ab6116fdf78a

**Unmodified**, the rule every vendored thing here follows; no Kosmos
copyright line is added. `tools/llvm_vendor.py` makes this directory and
is the only thing that writes to it, except this file, which it keeps.

## What is here

- `libcxx/`, `libcxxabi/`, `libunwind/`: the three runtimes, whole, less
  their `test/` directories;
- `runtimes/`, `cmake/`, `llvm/cmake/` and `llvm/utils/llvm-lit/`: the
  CMake the three are configured with together - `runtimes/` adds
  `llvm-lit` as a directory even with every test off;
- `libc/`: 120 files of LLVM's C library, and only those. libc++ shares its
  number parsing (`from_chars`, `std::stod`) with LLVM's libc, and these
  are the headers the build included, read from ninja's log by the script
  for both architectures - so the closure was chosen by the compiler, as
  `musl-math/` and FFmpeg's files were. `libc-closure.txt` lists them.

## Why

**WebKit** (`docs/webkit.md`, W1c) is C++, and so is most of what a modern
browser is built from. libc++, because it is LLVM's and the compiler is;
libc++abi for exceptions and RTTI; LLVM's libunwind in its bare-metal form,
which finds the unwinding tables by the names `user/user.ld` gives them
rather than by asking a dynamic loader there is not one of.

## How it is built

By clang, with `runtime/cxx/kosmos.cmake` as the toolchain file and
`runtime/cxx/runtimes.cmake` as the options - against Kosmos's own libc
headers, `runtime/include/`, so C++ cannot reach a function an image does
not have. Threads, localization, wide characters, the filesystem, the
random device and the time zone database are off; `runtimes.cmake` says
why each, and when it comes back.

What the three ask of the libc, besides what C always did: `aligned_alloc`,
`clock_gettime`, `strtold`, `fileno` and `isatty` (which say there are no
descriptors), and on AArch64 libgcc's quad-float helpers.
