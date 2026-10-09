#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The line icons, rendered from Lucide's vectors.

    python3 tools/lineicons.py            # writes assets/icons/line/

Every small grey icon in Kosmos - a category in Preferences' sidebar, the
search and the menu in a header, a place in Tracker, and the button bars -
is one of these. Diego, 26 September 2026, choosing Lucide from three sets
laid side by side in the IDE's bar (`docs/icon-sets.html`, `roadmap.md` 6o):
"Lucide it is". Until then they were 26 drawn by hand from the mockups.

**The vectors are the source, and the pictures are what the build carries.**
Lucide's SVGs are vendored unmodified in `assets/icons/lucide/`, with their
licence; each one Kosmos uses is named below and rendered by a real browser
at every size the desktop's scale can ask for - 15 at 100 per cent, 19 at
125, 23 at 150, 30 at 200 - rather than rendered once and resampled, because
a resampled one-pixel line is a grey smear. The kit paints a look's colour
through each one's coverage (`gfx.c`'s `tint`), so one picture per size
serves every look, light or dark.

**An icon more is a line below and a run of this**, which is by hand, not
by the build, because it needs a browser and the build must not. What it
writes is committed, as a font's BDF is: an input the build converts, and
whose origin is written down here.

Each PNG is white with the icon's coverage as its alpha - the colour is
never in the file, which is the point.
"""

import os
import struct
import subprocess
import sys
import zlib

import scratch

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, "assets", "icons", "line")
CHROME = "/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"

# The sizes the desktop's scale turns 15 points into: `scale.px(15, pct)`
# at 100, 125, 150 and 200 per cent.
SIZES = (15, 19, 23, 30)

LUCIDE = os.path.join(ROOT, "assets", "icons", "lucide")

#
# Kosmos's name -> Lucide's, and the width of its line in Lucide's 24-unit
# box when it is not Lucide's own 2. The names on the left are what callers
# of `gc:line_icon` and `ui.iconbutton` say, and they were the hand-drawn
# icons' names first, so none of those calls changed when the drawings did.
#
ICONS = {
    # Preferences' categories.
    "appearance": ("palette", None),
    "display":    ("monitor", None),
    "sound":      ("volume-2", None),
    "power":      ("power", None),
    "network":    ("wifi", None),
    "keyboard":   ("keyboard", None),
    "startup":    ("rocket", None),
    "datetime":   ("clock", None),
    "system":     ("cpu", None),

    # Headers: search, the menu, close, the arrows, the dots, reload.
    "search":     ("search", None),
    "menu":       ("menu", None),
    "close":      ("x", None),
    "back":       ("chevron-left", None),
    "forward":    ("chevron-right", None),
    "more":       ("ellipsis-vertical", None),
    "reload":     ("rotate-cw", None),

    # A table's heading, beside the column it is sorted by (Processes).
    "ascending":  ("chevron-up", None),
    "descending": ("chevron-down", None),

    # Tracker's places.
    "home":       ("house", None),
    "folder":     ("folder", None),
    "newfolder":  ("folder-plus", None),
    "recent":     ("rotate-ccw-clock", None),
    "trash":      ("trash", None),
    "document":   ("file", None),
    "music":      ("music", None),
    "pictures":   ("image", None),
    "drive":      ("hard-drive", None),
    # The places a person keeps (`roadmap.md` 6w): Movies and Captures,
    # beside Documents, Photos (`pictures`) and Music above.
    "movies":     ("film", None),
    "captures":   ("video", None),
    # And Downloads (9 October, Diego: "Create a downloads folder", "as we
    # have a browser now") - what the browser fetches, what Mail saves.
    "downloads":  ("download", None),

    # Preferences' Mouse page (9 October, roadmap 6zi): pointer speed and
    # double-click speed.
    "mouse":      ("mouse", None),

    # The kit's checkbox, ticked: white on the accent, so heavier than the
    # rest - at 15 pixels Lucide's own 2 on a filled box reads as a scratch.
    "check":      ("check", "3"),

    # Button bars: the Kosmos IDE's first (`docs/kosmos-ide.html`), and any
    # application's that wants the same words.
    "new":        ("file-plus", None),
    "open":       ("folder-open", None),
    "save":       ("save", None),
    "saveall":    ("save-all", None),
    "undo":       ("undo-2", None),
    "redo":       ("redo-2", None),
    "run":        ("play", None),
    "stop":       ("square", None),
    "debug":      ("bug", None),
    "copy":       ("copy", None),
    "paste":      ("clipboard-paste", None),
    "cut":        ("scissors", None),
    "settings":   ("settings", None),

    # The Deskbar's indicators (`roadmap.md` 6zl, `docs/statusicons.html`):
    # sound on is `sound` above, the same speaker Preferences shows. A run
    # renders every icon again, and on 27 September eighteen of the ones
    # already committed came back a shade different - the browser's, not the
    # vectors' - so only these new names were kept and the rest restored.
    "muted":      ("volume-x", None),
    "wired":      ("ethernet-port", None),
    "wifi":       ("wifi", None),
    "offline":    ("wifi-off", None),
    "battery-full":     ("battery-full", None),
    "battery-medium":   ("battery-medium", None),
    "battery-low":      ("battery-low", None),
    "battery-charging": ("battery-charging", None),

    # The browser's header (`docs/browser.html`, `roadmap.md` 6zz d): how a
    # page came - checked, plain, refused - a favorite, the sidebar of
    # favorites and history, and a new tab.
    "secure":     ("lock", None),
    "plain":      ("lock-open", None),
    "refused":    ("shield-x", None),
    "star":       ("star", None),
    "sidebar":    ("panel-left", None),
    "plus":       ("plus", None),

    # A page that is a favorite (6zz d3): the same star, filled.
    "starred":    ("star", None),

    # Passwords (`docs/keyring.md`, K6): a kept password, in its list and
    # its sidebar.
    "key":        ("key", None),

    # Do Not Disturb, in the notifications' history (`docs/notifications.html`),
    # and Preferences' Notifications.
    "moon":       ("moon", None),
    "bell":       ("bell", None),

    # Who is reaching this machine, beside the bell on the Deskbar's strip
    # (`roadmap.md`, the status icons; Diego, 8 October: "a screen sharing
    # status symbol near the notifications bell"): the screen shared by VNC
    # and a command line by Telnet.
    "screenshare": ("screen-share", None),
    "terminal":   ("terminal", None),

    # Sharing files over the network (`docs/sharing.html`, step N6): the
    # globe on Tracker's trail and beside "over the network", the amber
    # triangle of a server gone away, and a server in the Network group.
    "globe":      ("globe", None),
    "warning":    ("triangle-alert", None),
    "server":     ("server", None),

    # Kosmos Write's toolbar and its Format panel (`docs/write.html`), and
    # Present's and Sheets' after it. View is `sidebar`, Add Page `new`,
    # Media `pictures` and a chooser's arrow `descending`, as they are.
    "zoom":       ("zoom-in", None),
    "minus":      ("minus", None),
    "insert":     ("list-plus", None),
    "table":      ("table", None),
    "chart":      ("chart-column", None),
    "textbox":    ("type", None),
    "shape":      ("shapes", None),
    "comment":    ("message-square", None),
    "export":     ("share", None),
    "format":     ("paintbrush", None),
    "page":       ("file-text", None),
    "bold":       ("bold", None),
    "italic":     ("italic", None),
    "underline":  ("underline", None),
    "strike":     ("strikethrough", None),
    "align-left":    ("text-align-start", None),
    "align-center":  ("text-align-center", None),
    "align-right":   ("text-align-end", None),
    "align-justify": ("text-align-justify", None),

    # Kosmos Mail (`docs/mail.html`): the mailboxes by their use, and the
    # header's buttons - write, reply, reply to all, forward, archive, flag,
    # read or not - and an attachment's clip. Drafts is `page`, Trash
    # `trash`, Get Mail `reload`, as they are.
    "inbox":      ("inbox", None),
    "sent":       ("send", None),
    "archive":    ("archive", None),
    "junk":       ("ban", None),
    "flag":       ("flag", None),
    "flagged":    ("flag", None),
    "compose":    ("square-pen", None),
    "reply":      ("reply", None),
    "replyall":   ("reply-all", None),
    "forwardmail": ("forward", None),
    "mail":       ("mail", None),
    "mailopen":   ("mail-open", None),
    "attachment": ("paperclip", None),
}

# The ones drawn filled as well as stroked - Lucide's shapes are outlines,
# and a star that says yes is a solid one.
FILLED = {"starred", "flagged"}


def body(lucide):
    """What is inside Lucide's `<svg>`, as its file has it."""
    path = os.path.join(LUCIDE, lucide + ".svg")

    if not os.path.exists(path):
        sys.exit("lineicons: Lucide has no %s - `assets/icons/lucide/` is "
                 "release %s's" % (lucide, "1.48.0"))

    text = open(path).read()
    return text[text.index(">", text.index("<svg")) + 1:text.rindex("</svg>")]


STEP = 40          # one cell per icon on the sheet, wider than the largest


def sheet(size):
    """One page with every icon at `size`, in a row, black on nothing."""
    cells = []

    # Lucide's own attributes - a 24 box, lines of 2 with round ends and
    # joins - in black, since only the coverage is kept.
    for i, (name, (lucide, width)) in enumerate(sorted(ICONS.items())):
        cells.append(
            '<svg style="position:absolute;left:%dpx;top:0" width="%d" '
            'height="%d" viewBox="0 0 24 24" fill="%s" stroke="#000" '
            'stroke-width="%s" stroke-linecap="round" '
            'stroke-linejoin="round">%s</svg>'
            % (i * STEP, size, size, "#000" if name in FILLED else "none",
               width or "2", body(lucide)))

    return ('<!doctype html><html><head><style>html,body{margin:0;'
            'background:transparent}</style></head><body>%s</body></html>'
            % "".join(cells))


def rgba_rows(path):
    """A PNG's rows as RGBA bytearrays, undoing the filters."""
    data = open(path, "rb").read()
    pos, idat = 8, b""
    width = height = 0

    while pos < len(data):
        n = struct.unpack(">I", data[pos:pos + 4])[0]
        tag, body = data[pos + 4:pos + 8], data[pos + 8:pos + 8 + n]

        if tag == b"IHDR":
            width, height, depth, kind = struct.unpack(">IIBB", body[:10])

            if depth != 8 or kind != 6:
                sys.exit("lineicons: the browser gave a PNG that is not "
                         "8-bit RGBA (%d, %d)" % (depth, kind))
        elif tag == b"IDAT":
            idat += body

        pos += 12 + n

    raw = zlib.decompress(idat)
    stride = width * 4
    prev = bytearray(stride)
    rows = []

    for y in range(height):
        kind = raw[y * (stride + 1)]
        line = bytearray(raw[y * (stride + 1) + 1:(y + 1) * (stride + 1)])

        for i in range(stride):
            a = line[i - 4] if i >= 4 else 0
            b = prev[i]
            c = prev[i - 4] if i >= 4 else 0

            if kind == 1:
                line[i] = (line[i] + a) & 255
            elif kind == 2:
                line[i] = (line[i] + b) & 255
            elif kind == 3:
                line[i] = (line[i] + (a + b) // 2) & 255
            elif kind == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                line[i] = (line[i] + (a if pa <= pb and pa <= pc
                                      else b if pb <= pc else c)) & 255

        prev = line
        rows.append(line)

    return width, height, rows


def write_mask(path, size, rows, x0):
    """White, with the icon's coverage as alpha."""
    raw = bytearray()

    for y in range(size):
        raw.append(0)

        for x in range(size):
            raw += bytes((255, 255, 255, rows[y][(x0 + x) * 4 + 3]))

    def chunk(tag, body):
        return (struct.pack(">I", len(body)) + tag + body
                + struct.pack(">I", zlib.crc32(tag + body)))

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n"
                + chunk(b"IHDR", struct.pack(">IIBBBBB", size, size,
                                             8, 6, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(bytes(raw), 9))
                + chunk(b"IEND", b""))


def main():
    if not os.path.exists(CHROME):
        sys.exit("lineicons: needs Google Chrome at %s, to render the "
                 "vectors the way the mockups are rendered" % CHROME)

    os.makedirs(OUT, exist_ok=True)
    names = sorted(ICONS)
    written = 0

    # Through `scratch`, as every tool here makes its temporary files: it is
    # the one place that removes them when the tool is done.
    tmp = scratch.directory("lineicons")

    for size in SIZES:
        page = os.path.join(tmp, "sheet%d.html" % size)
        shot = os.path.join(tmp, "sheet%d.png" % size)

        with open(page, "w") as f:
            f.write(sheet(size))

        subprocess.run([CHROME, "--headless=new", "--disable-gpu",
                        "--hide-scrollbars",
                        "--force-device-scale-factor=1",
                        "--default-background-color=00000000",
                        "--window-size=%d,%d" % (len(names) * STEP, size),
                        "--screenshot=" + shot, "file://" + page],
                       stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, check=True)

        _, _, rows = rgba_rows(shot)

        for i, name in enumerate(names):
            write_mask(os.path.join(OUT, "%s-%d.png" % (name, size)),
                       size, rows, i * STEP)
            written += 1

    print("lineicons: %d icons at %s pixels into %s"
          % (len(names), ", ".join(map(str, SIZES)),
             os.path.relpath(OUT, ROOT)))
    return 0 if written else 1


if __name__ == "__main__":
    sys.exit(main())
