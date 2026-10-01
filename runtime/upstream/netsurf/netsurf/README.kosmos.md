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
rule `lua/upstream/` keeps. Twenty-seven sources and the headers they
include, found by compiling them and reading what they include, and nothing
else:

    content/handlers/html/   box_construct box_inspect box_manipulate
                             box_normalise box_special font layout
                             layout_flex redraw redraw_border table
                             forms form box_textarea
    content/handlers/css/    select hints internal dump
    desktop/                 plot_style system_colour textarea
    utils/                   corestrings nsoption talloc url
    utils/nsurl/             nsurl parse

The last five arrived with forms that work (`roadmap.md` 6zz j6, 1 October
2026): `forms.c` finds a page's forms and makes a control for each field,
`form.c` keeps their values and encodes what is sent, `box_textarea.c` joins
a field's box to the text-editing widget, `desktop/textarea.c` is that
widget - caret, selection, cutting and pasting, scrolling its text - and
`utils/url.c` escapes a form's fields for its address.

**One header is changed as it is built**, by a patch in
`runtime/patches/netsurf/netsurf/content/fetch.h.patch`: `fetch.h`, which
`form.c` includes for its multipart data, includes `utils/inet.h` for one
declaration about the fetchers' sockets - and `inet.h` asks for
`sys/socket.h`. Kosmos has no sockets and is not going to (`CLAUDE.md`,
compatibility), so the port is patched: the include and that declaration,
of fetchers that are not here, taken out. The copy is made under `build/`
and found ahead of this one.

## What is not, and what stands in for it

What the layout calls of the browser around it - fetching a picture, a
scrollbar, measuring a word, running work later - is
`user/bin/apps/browser/web_netsurf.c`, Kosmos's own. These of NetSurf's files
were left out, and what of them is used written there instead:

- `utils/utils.c`: two string functions the layout uses, beside POSIX
  stand-ins (`stat`, `scandir`, `uname`, `realpath`) this system does not
  have and has decided not to.
- `utils/idna.c` and `utils/punycode.c`: host names in other scripts, through
  utf8proc, which is not here. ASCII names are lowered, others refused - and
  NetSurf's URL parser then keeps the name as written, its own fallback.
- `utils/utf8.c`: it wants `iconv`. The helpers the text area and the forms
  use are written over libparserutils, as NetSurf's own are, and a form's
  text goes into the charset it is sent in through libparserutils' own
  encoders.
- `content/handlers/html/interaction.c`: what a click and a key do on a page.
  Its form half is `web_ns_click` and `web_ns_key`; its other half is text
  selection, frames, image maps and scripts, which this browser does not
  have.

## Built with

The Makefile's `WEB_NETSURF` and `WEB_NSB_CFLAGS`: warnings off (`-w`) as for
every vendored tree, the include paths NetSurf's own build gives, and two of
its own build switches, the log filters `nsoption.c` names.
