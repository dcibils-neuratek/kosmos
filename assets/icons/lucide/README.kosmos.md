# Lucide, vendored

Upstream: <https://lucide.dev>, <https://github.com/lucide-icons/lucide>,
release `1.48.0` (commit `f53e5bfff0f909f3f451933538744330650d3ce0`,
24 September 2026), downloaded on 26 September 2026 as the release's
`lucide-icons-1.48.0.zip`:

    e27a6b58bebc563581f89668a5a652782075a85a07568e4fd8051a17ce81dba6  lucide-icons-1.48.0.zip

Licence: ISC, and MIT for the icons Lucide derived from Feather - both in
`LICENSE`, fetched from the same tag.

**The 1,854 SVGs, byte for byte, and nothing else of the release**: its
`.json` files, which are each icon's search tags and categories, are left
out. **Unmodified**: a line's width, where Kosmos wants another, is set when
`tools/lineicons.py` renders the icon, never in the file.

## What it is for

Every small grey icon in Kosmos - Preferences' categories, a header's search
and menu, Tracker's places, and the button bars, the Kosmos IDE's first
(`roadmap.md` 6o). Diego chose it on 26 September 2026 from three sets laid
side by side in the IDE's bar (`docs/icon-sets.html`): "Lucide it is".

## How it is used

Nothing here goes into an image. `tools/lineicons.py` names the icons Kosmos
uses - Kosmos's name beside Lucide's - renders each in a browser at the four
sizes the desktop's scale asks for, and writes their coverage alone to
`assets/icons/line/`, which the build carries and `gc:line_icon` paints in a
look's colour. An icon more is a line in that list and a run of the tool.
