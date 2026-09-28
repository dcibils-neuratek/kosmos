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
(`puff`, beside this). Diego chose on 27 September to vendor miniz's
compressor rather than write one (`docs/rightclick.html`, answer 4;
`roadmap.md` 6v).

## What of it is used

Only `tdefl`, the deflater, and `mz_crc32`. miniz also reads and writes whole
zip archives, but through `malloc` and `stdio`, and this system moves a file
through regions (`fs.read_into`, `fs.write_from`) with a 2 MB heap in each
process. So it is built with

    -DMINIZ_NO_STDIO -DMINIZ_NO_TIME -DMINIZ_NO_ZLIB_COMPATIBLE_NAMES
    -DMINIZ_NO_MALLOC -DMINIZ_NO_INFLATE_APIS

(the Makefile's `MINIZ_FLAGS`; no inflater takes the archive code with it,
`miniz.h` says so itself), and `user/kits/compress/deflate.c` runs the
deflater over two regions with its state in pages of its own. The zip's
structure is `user/lib/zip.lua`'s; reading one inflates with `puff`.

## The rule this follows

The same one `lua/upstream/` and `puff/` follow: what is in the tree is what
the author released, and anything done to it is a build step somebody can
read. It is compiled as every vendored file is, `-Wno-error`.
