--  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
--
--  Drop files here and they land in the machine's /home.
--
--  A droplet rather than a window with a file picker, because the thing that
--  was actually tedious is the round trip: find the file, remember where the
--  image is, remember what `kfs.lua put` wants its arguments in. Dropping a
--  handful of songs on a Dock icon is one gesture.
--
--  It uses the same `tools/kfs.lua` the machine itself runs - the filesystem
--  has one implementation and this is the host side of it - so a file put in
--  here is written by the code that will read it.
--
--  Build it with `make droplet`.

property repoPath : "__REPO__"

on runPut(hostPath, guestName)
	set img to repoPath & "/build/kosmos.img"
	set lua to repoPath & "/build/host/lua"
	set kfs to repoPath & "/tools/kfs.lua"
	
	return do shell script quoted form of lua & " " & ¬
		quoted form of kfs & " put " & ¬
		quoted form of img & " " & ¬
		quoted form of hostPath & " " & ¬
		quoted form of ("/home/" & guestName)
end runPut

on open droppedItems
	set img to repoPath & "/build/kosmos.img"
	
	tell application "System Events"
		if not (exists file img) then
			display dialog "No disk image yet." & return & return & ¬
				"Run `make disk` in " & repoPath & " first." ¬
				with title "Kosmos disk" buttons {"OK"} default button 1 with icon caution
			return
		end if
	end tell
	
	set report to ""
	set failures to 0
	
	repeat with anItem in droppedItems
		set hostPath to POSIX path of anItem
		
		--  A folder is its files, one level down. Dropping a folder of
		--  wallpapers should do the obvious thing rather than nothing.
		tell application "System Events"
			set isFolder to (exists folder hostPath)
		end tell
		
		if isFolder then
			set inner to paragraphs of (do shell script ¬
				"find " & quoted form of hostPath & " -maxdepth 1 -type f -not -name '.*'")
			repeat with f in inner
				if (f as string) is not "" then
					set nm to do shell script "basename " & quoted form of (f as string)
					try
						runPut(f as string, nm)
						set report to report & "  " & nm & return
					on error errMsg
						set failures to failures + 1
						set report to report & "  " & nm & " - " & errMsg & return
					end try
				end if
			end repeat
		else
			set nm to do shell script "basename " & quoted form of hostPath
			try
				runPut(hostPath, nm)
				set report to report & "  " & nm & return
			on error errMsg
				set failures to failures + 1
				set report to report & "  " & nm & " - " & errMsg & return
			end try
		end if
	end repeat
	
	if report is "" then set report to "  (nothing to copy)" & return
	
	set headline to "Copied into /home:"
	if failures > 0 then set headline to (failures as string) & " failed:"
	
	display dialog headline & return & return & report ¬
		with title "Kosmos disk" buttons {"OK"} default button 1
end open

on run
	display dialog "Drop files or folders on this icon and they are copied " & ¬
		"into the Kosmos disk image, under /home." & return & return & ¬
		"Then start the machine with `make qemu`." ¬
		with title "Kosmos disk" buttons {"OK"} default button 1
end run
