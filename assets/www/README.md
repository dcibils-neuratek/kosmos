# The browser's test page

Kosmos's own, MIT like the rest of the tree - not vendored - except `dam.html`,
below. The browser's
test page and its page benchmark (`roadmap.md` 6zz a). Diego, 30 September
2026: "lets put a html there that tests the html capabilities", "images,
labels, titles, tables,etc", "that will be our page benchmark tool for the
browser".

- `index.html` - every kind of thing a document has, numbered, each part
  saying what it should look like: headings, text and its styles, lists,
  tables, images, labels and a form, quotes and code, CSS, a rule, and
  enough length to scroll. The browser's status line says what it cost.
- `second.html` - where its links lead, on a pale yellow ground.
- `kosmos.png` and `photo.jpg` - 240 by 135, made for it: the PNG blue into
  gold with a pure magenta square, the JPEG grey stripes with a pure cyan
  one, so a check can find each on the screen by a colour nothing else on
  the page uses.

- `dam.html` - **not ours**: Wikipedia's article "Dam", 1,435,447 bytes as
  en.wikipedia.org served it on 30 September 2026 to `NetSurf/3.11 (Kosmos
  0.10.200)`, unmodified, under CC BY-SA 4.0 by its contributors (`LICENSE`).
  The browser's standing large page (`roadmap.md` 6zz j) - Diego: "this is a
  good test for the browser as it is a big page with lots of content", "1 to
  10mb pages are normal". Its pictures and styles point at Wikipedia and are
  not here, which is what a test on this Mac wants: the page, and nothing
  that needs the internet.

`make www` puts all of it in `/Home/www` on the QEMU disk, where the Servers
window's web server serves it; `tools/run_browser.py` serves it from the
Mac in the gate.
