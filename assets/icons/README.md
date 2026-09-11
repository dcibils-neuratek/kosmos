# assets/icons

Haiku's icons, as exported by `darealshinji/haiku-icons`: 48 of the 452
PNGs it has at 32x32, chosen because something in Kosmos draws each one -
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
2026. The names are Haiku's, not ours:

| what draws it | icons |
| ------------- | ----- |
| Tracker, by what a file is | `Folder_generic`, `File_Text`, `File_SourceCode`, `File_Image_1`, `File_PDF`, `File_HTML`, `File_Audio`, `Prefs_Fonts`, `File_Generic` |
| Tracker, by where it is | `Folder_home` for `/home`, `Device_Ramdisk` for `/ramfs` |
| a launcher | `Device_Harddisk` for Drive, `App_Generic` for one that names none |
| the Trash | `Trash_Empty` and `Trash_Full`, by whether anything is in it |
| the Deskbar | whatever each program's `-- kosmos: icon` line names: `App_Tracker`, `App_Terminal`, `TeamIcon` for Processes, `App_Poorman` for the web server, and the rest |

`files.lua` holds the first four rows and the programs' own headers hold
the last, so adding an application that draws a window is one line in its
header and no change here.

**32x32 as exported, and never scaled.** There is no scaler in this system,
which is also why a menu row with a picture in it is as tall as the picture.

Each is 8 bits a channel, colour type 6 - RGBA - which is what `gfx.png`
decodes and what `surface:blend` composites. 81 KB for all 48.

**What was here before** was seven icons from the Tango Icon Library 0.8.90,
public domain. They were replaced because Haiku's are the BeOS lineage's
own, and because seven icons could not give each application a face.
