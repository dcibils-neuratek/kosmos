# puff

`puff.c` and `puff.h` from zlib's `contrib/puff`, by Mark Adler, **byte for
byte as released**. The licence is at the top of both files and is the zlib
licence; it is reproduced in `LICENSE.puff` beside them.

## Why this is here

PNG's compressed data is a zlib stream, so decoding a PNG means having an
inflate. This is zlib's own reference implementation of one: about eight
hundred lines, written to be read rather than to be fast, and correct by
construction in the sense that it is the thing other decoders are checked
against.

## Why this one rather than a faster one

Speed is not what an image loader in this system is short of. `puff` decodes
a bit at a time and is perhaps an order of magnitude slower than a table
driven inflate, which for the images this system loads - a few, once, at
startup - is the difference between imperceptible and imperceptible.

What it buys instead is that it is small enough to read in an afternoon and
that it makes no allocations at all: the caller provides the output buffer
and `puff` fills it, which matters here because there is no allocator in the
kernel and a bounded one in a process.

## The rule this follows

The same one `lua/upstream/` follows and for the same reason: what is in the
tree is what the author released, and anything done to it is a build step
somebody can read. If this ever needs changing, the change goes in a patch
applied during the build and not in these files.

**Where it runs.** At EL0, inside whichever application is loading an image,
which is what makes it acceptable to hand a C decoder a file: a bug in it
kills that application and nothing else. That is the same test `CLAUDE.md`
applies everywhere - if it can only kill its own process, its failure is its
own business.
