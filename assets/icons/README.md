# assets/icons

The Tango Icon Library, version 0.8.90, released 2009. Seven icons of the
215 it ships at 32x32, chosen because Tracker can tell those seven things
apart today - an icon for a distinction the system cannot make is a picture
that lies.

**Public domain.** `LICENSE.tango` is the release's own `COPYING`, byte for
byte, and it is the whole of it: one sentence putting the icons in the
public domain. No attribution is required; this file exists because
`CLAUDE.md` says vendored data records where it came from, and because
knowing the version is what makes it possible to fetch the same bytes again.

They are here byte for byte as the release shipped them, from
`tango-icon-theme-0.8.90/32x32/`:

| here                 | in the release                      |
| -------------------- | ----------------------------------- |
| `folder.png`         | `places/folder.png`                 |
| `user-home.png`      | `places/user-home.png`              |
| `text-x-generic.png` | `mimetypes/text-x-generic.png`      |
| `text-x-script.png`  | `mimetypes/text-x-script.png`       |
| `image-x-generic.png`| `mimetypes/image-x-generic.png`     |
| `font-x-generic.png` | `mimetypes/font-x-generic.png`      |
| `drive-harddisk.png` | `devices/drive-harddisk.png`        |

The names are the release's, not ours, and `files.lua` maps a file's kind
onto one of them. That is on purpose: these are the freedesktop icon-naming
names, so a second theme dropped in beside this one needs no new mapping.

**32x32 from the release, not scaled from a larger one.** Tango ships each
size drawn separately and the small ones are hinted by hand - the folder's
tab is a clean two-pixel step at 32 and a grey smear if you resample the
512. There is no scaler in this system anyway, and this is the reason not
to want one yet.

Each is 8 bits a channel, colour type 6 - RGBA - which is what `gfx.png`
decodes and what `surface:blend` composites. 8 KB for all seven.
