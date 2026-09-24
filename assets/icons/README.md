# assets/icons

Haiku's icons, as exported by `darealshinji/haiku-icons`: 50 of the 452
it has, at each of the three sizes it exports - 32x32 here, and 16x16 and
64x64 in the folders of those names - chosen because something in Kosmos
draws each one -
Tracker's file icons, the launchers on the desktop, and the Deskbar's
picture for each application.

**MIT, Copyright (c) 2007-2026 Haiku, Inc.** `LICENSE` beside them is that
repository's own licence file, byte for byte. Its README says two more
things worth keeping here, because they are part of the terms rather than
decoration: the icons were exported from Haiku's `data/artwork` at commit
`49f62bc1728ce870478ae4bdb82201f0ee0c204b`, and several of those images
are trademarks of Haiku, Inc. - the HAIKU logo, the Leaf and the
Background Leaf. None of those is here. `App_Haiku3d`, whose green H comes
close enough to the leaf to wonder about, was left out for that reason.

They are here byte for byte as the repository has them, from `png/32x32/`
at commit `ccf434a0cf31aae47cc8aac91934651619c1b9b2` of
<https://github.com/darealshinji/haiku-icons>, fetched on 11 September
2026. The same icons from `png/16x16/` and `png/64x64/` at the same commit
were fetched on 22 September 2026, when the 32s already here were compared
with that commit's and found identical, all 49. Each folder has the same
`LICENSE` beside its icons, because the build finds a file's licence in the
file's own folder. The names are Haiku's, not ours:

| what draws it | icons |
| ------------- | ----- |
| Tracker, by what a file is | `Folder_generic`, `File_Text`, `File_SourceCode`, `File_Image_1`, `File_PDF`, `File_HTML`, `File_Audio`, `Prefs_Fonts`, `File_Generic` |
| Tracker, by where it is | `Folder_home` for `/home`, `Device_Ramdisk` for `/ramfs` |
| a launcher | `Device_Harddisk` for Drive, `App_Generic` for one that names none |
| the Trash | `Trash_Empty` and `Trash_Full`, by whether anything is in it |
| the Deskbar | whatever each program's `-- kosmos: icon` line names: `App_Tracker`, `App_Terminal`, `TeamIcon` for Processes, `App_Poorman` for the web server, and the rest |

**One of them is renamed, and only its name.** CodyCam, Haiku's webcam
application, is drawn separately at each size, so the repository exports it
as `App_CodyCam_16.png`, `App_CodyCam_32.png` and `App_CodyCam_64.png` - one
in each folder. The kit finds a picture's three sizes by one name, so here
all three are `App_CodyCam.png`, each in its own folder and each byte for byte
as fetched on 24 September 2026 from the same commit. The Camera app wears it
(`roadmap.md` 6d).

`files.lua` holds the first four rows and the programs' own headers hold
the last, so adding an application that draws a window is one line in its
header and no change here.

**Each size drawn as exported, and any other size from the 64.** `gc:icon`
draws a 16, a 32 or a 64 pixel for pixel, and averages the 64 down for any
other size (`stretch`'s `smooth` mode in `gfx.c`) - the Deskbar's 24, since
it became 32 pixels tall on 22 September (`roadmap.md` 5v), and every icon
at a scale when there is one (5z). In the image the 32s keep their names
and the others are `16x16/<name>` and `64x64/<name>`.

**And the three are what a person can choose between**, on the desktop and
in Tracker's icon view (`roadmap.md` 5za, `/lib/iconsize.lua`). Three and
no fourth, for the reason above turned around: a size the system works out
may be the 64 shrunk, and a size somebody picks off a menu should be the
best picture there is of it. So a fourth size here would be a fourth size
in that menu, and a fifth export would be four.

Each is 8 bits a channel, colour type 6 - RGBA - which is what `gfx.png`
decodes and what `surface:blend` composites. 81 KB for the 32s, 32 KB for
the 16s and 210 KB for the 64s.

**What was here before** was seven icons from the Tango Icon Library 0.8.90,
public domain. They were replaced because Haiku's are the BeOS lineage's
own, and because seven icons could not give each application a face.
