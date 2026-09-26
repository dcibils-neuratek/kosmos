# ufbx, vendored

Upstream: <https://github.com/ufbx/ufbx>, tag `v0.23.1`, commit
`26a482ae66871d7de36eb722aa060bce95bce274` (25 September 2026), downloaded
on 26 September 2026 file by file from that commit.
Licence:  MIT or public domain (the Unlicense), whichever is preferred. See
          `LICENSE`; the two sources carry no notice of their own, so that
          file is the only one, and it travels beside them.

Three files, taken byte for byte: `ufbx.c`, `ufbx.h` and `LICENSE`.
**Unmodified**.

    4a956c26a708e40d82ecb0aeaee54da709d5ba7c144c9642174970059bda7e1d  ufbx.c
    f787be529af577efc04f3e3e735844a08556718dbb613d1211c2b313d72895dc  ufbx.h
    0dd48ebadf52273c736256325c8f078c03c8bb4facee22a4122de0ad3f615391  LICENSE

## What it is for

FBX in Cafesa3D (`roadmap.md` 4l, 5c): Autodesk's format, which game asset
stores and Mixamo hand out, and which nobody documents - ufbx is a reader
written against thousands of files from Maya, 3ds Max, Blender and the rest,
and fuzzed. It is read by `user/kits/3d/k3d_fbx.c`, which turns a scene into
the parts and materials `/lib/translators/fbx.lua` gives Cafesa3D.

## How it is built, as build steps rather than edits

- **Its switches are in `user/kits/3d/k3d_ufbx.h`**, named to it by
  `-DUFBX_CONFIG_HEADER`, which `ufbx.h` includes first - so `ufbx.c` and
  `k3d_fbx.c` see the same configuration and neither file is touched. What
  they leave out: stdio (the bytes come from Cafesa3D, which read the
  file), OBJ (which has a translator of its own), geometry caches,
  animation baking, subdivision, NURBS tessellation and skinning.
- **Its assertions call `k3d_ufbx_failed`**, which stops the process as any
  failed assertion here does. The libc's `assert` calls `panic`, and ufbx
  has a parameter named `panic` that such a macro would try to call.
- Compiled in a file of its own with `-w -Wno-error`, as every vendored
  thing here is: its warnings are not ours to fix.
- Its `malloc`, `realloc`, `free` and maths are the libc's.

## What was left out

The rest of the repository: the tests, the fuzzers, the examples, the
bindings and the test data. A few files of the test data are fetched for
`tools/test_fbx.c` by `tools/fetch_conformance.py fbx`, pinned to the same
commit and to their sums (`tools/fbx_conformance.txt`).
