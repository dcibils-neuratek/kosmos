# miniz

`miniz.c`, `miniz.h` and `LICENSE` from miniz 3.0.2, by Rich Geldreich and
others, **byte for byte as released**:
`https://github.com/richgel999/miniz/releases/download/3.0.2/miniz-3.0.2.zip`,
99,268 bytes, SHA-256
`ada38db0b703a56d3dd6d57bf84a9c5d664921d870d8fea4db153979fb5332c5`,
fetched on 28 September 2026. The licence is MIT and is in `LICENSE`
beside them.

## Why this is here

A zip is written with DEFLATE, and the compress kit could only inflate
(with `puff`, then). Diego chose on 27 September to vendor miniz's
compressor rather than write one (`docs/rightclick.html`, answer 4;
`roadmap.md` 6v).

## What of it is used

`tdefl`, the deflater, `tinfl`, the inflater, and `mz_crc32`. miniz also
reads and writes whole zip archives, but through `malloc` and `stdio`, and
this system moves a file through regions (`fs.read_into`, `fs.write_from`).
So it is built with

    -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
    -DMINIZ_NO_MALLOC -DMINIZ_NO_ARCHIVE_APIS

(the Makefile's `MINIZ_FLAGS`), and `user/kits/compress/deflate.c` runs the
deflater over two regions with its state in pages of its own. The zip's
structure is `user/lib/zip.lua`'s, and reading one inflates with `tinfl`.

**The inflater since 30 September**: gzip, for the web's pages
(`user/kits/compress/gzip.c`, `roadmap.md` 6zz g). It was left out while
`puff` was the only inflater wanted - and `-DMINIZ_NO_INFLATE_APIS` took the
archive code with it, which is why `-DMINIZ_NO_ARCHIVE_APIS` is now said
instead. `tinfl` decodes once, into a window that wraps; `puff` decoded a
stream twice to learn its size first. **The only inflater since 5 October
2026**: PNG, `inflate` and a zip's files moved onto it, and `puff` left the
tree - two inflaters in one kit being a second copy (`CLAUDE.md`).

## The rule this follows

The same one `lua/upstream/` follows: what is in the tree is what
the author released, and anything done to it is a build step somebody can
read. It is compiled as every vendored file is, `-Wno-error`.
