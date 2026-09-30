# The browser's test page

Kosmos's own, MIT like the rest of the tree - not vendored. The browser's
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

`make www` puts all of it in `/Home/www` on the QEMU disk, where the Servers
window's web server serves it; `tools/run_browser.py` serves it from the
Mac in the gate.
