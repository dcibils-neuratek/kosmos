# expat, vendored

Upstream: <https://libexpat.github.io/> - release 2.8.5,
`https://github.com/libexpat/libexpat/releases/download/R_2_8_5/expat-2.8.5.tar.xz`.
Licence:  MIT. See `COPYING`.

Fetched on 30 September 2026 with its signature, `expat-2.8.5.tar.xz.asc`,
which `gpgv` held to the release key fetched from keys.openpgp.org by its
fingerprint, CB8DE70A90CFBF6C3BF5CC5696262ACFFBD3AEC6: "Good signature
from Sebastian Pipping", expat's maintainer. The tarball's SHA-256:

    1e727b8933ec51a77a9a9d9afcf8e688bce45d907c13e36ab7393fe36e703182

`lib/` - the parser's sources and the headers they include, 26 files -
with `COPYING` and `README.md`, byte for byte and checked so by `cmp`
against a fresh unpacking. **Unmodified**, the rule every vendored thing
here follows; no Kosmos copyright line is added.

## Why expat

**SVG, in the browser** (`roadmap.md` 6zz j5) - Diego, 30 September: "go
ahead with SVG". An SVG is XML, and libsvgtiny (`../netsurf/libsvgtiny/`)
reads it as a DOM built by libdom's XML binding - which is written against
expat, as NetSurf itself builds it. libdom's HTML parser is hubbub, which
reads HTML's rules and not XML's; expat is the other half.

## How it is built

`lib/xmlparse.c`, `xmlrole.c`, `xmltok.c` and `random_arc4random_buf.c`
into the images that carry the web kit, with `-w` as vendored code is, and
configured by `runtime/config/expat_config.h` rather than by a `configure`
script: little-endian, namespaces, DTDs with expat's own limits on how far
an entity may amplify, and its hash salt from `arc4random_buf`, which the
libc answers from the kernel's entropy (`user/init/misc_user.c`).

`runtime/config/` holds one more file, an empty `sys/time.h`: expat
includes it for `gettimeofday`, which it calls only when there is no
`arc4random_buf`. ARM's toolchain had been finding newlib's; x86's has
none, which is how it was noticed.
