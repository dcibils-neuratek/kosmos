<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# NetSurf's layout, as NetSurf released it

The HTML layout engine of the NetSurf web browser, version 3.11 - the box
tree, block and inline layout, floats, tables, flex, and the drawing of boxes
- with the CSS selection and hints, and the few utilities they stand on.
Vendored for Kosmos's browser (`roadmap.md` 6zz j), which lays pages out with
it rather than with a layout of its own: Diego's choice, 30 September 2026.

**GPLv2**, in `COPYING` beside this file. Nothing in Kosmos is linked
dynamically, so an image carrying the browser is a GPLv2 work as a whole;
Kosmos's own sources stay MIT (`LICENSE`).

## Where it came from

`https://download.netsurf-browser.org/netsurf/releases/source/netsurf-3.11-src.tar.gz`,
4,311,767 bytes, SHA-256
`c28a626aefee428d053b13f88b5c440922245976522d12eaf137cfd32d201cb2`, fetched on
30 September 2026 over HTTPS from the project's own server. No checksum or
signature is published beside it. 3.11 is NetSurf's newest release, and it
was released with the libcss 0.9.2, libdom 0.4.2 and libhubbub 0.3.8 already
in `runtime/upstream/netsurf/`.

## What is here

**Unmodified, byte for byte**, each file where it sits in the release - the
rule `lua/upstream/` keeps. Twenty-two sources and the headers they include,
found by compiling them and reading what they include, and nothing else:

    content/handlers/html/   box_construct box_inspect box_manipulate
                             box_normalise box_special font layout
                             layout_flex redraw redraw_border table
    content/handlers/css/    select hints internal dump
    desktop/                 plot_style system_colour
    utils/                   corestrings nsoption talloc
    utils/nsurl/             nsurl parse

## What is not, and what stands in for it

What the layout calls of the browser around it - fetching a picture, a
scrollbar, measuring a word, running work later - is
`user/bin/apps/browser/web_netsurf.c`, Kosmos's own. Three of NetSurf's files
were left out and their few functions written there instead:

- `utils/utils.c`: two string functions the layout uses, beside POSIX
  stand-ins (`stat`, `scandir`, `uname`, `realpath`) this system does not
  have and has decided not to.
- `utils/idna.c` and `utils/punycode.c`: host names in other scripts, through
  utf8proc, which is not here. ASCII names are lowered, others refused - and
  NetSurf's URL parser then keeps the name as written, its own fallback.
- `utils/utf8.c`: nothing that is here reaches it, and it wants `iconv`.

## Built with

The Makefile's `WEB_NETSURF` and `WEB_NSB_CFLAGS`: warnings off (`-w`) as for
every vendored tree, the include paths NetSurf's own build gives, and two of
its own build switches, the log filters `nsoption.c` names.
