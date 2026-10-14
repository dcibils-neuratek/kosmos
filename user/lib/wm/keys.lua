-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Keys the window manager keeps: Super and its bindings, Control-W and
-- what follows it, the windows round with the keyboard - a part of
-- `wm.lua` in a file of its own (`roadmap.md` 6zn). The section as it
-- stood there, made by a function handed what it reads of the rest; the
-- loop's `running` is `OUT`'s, since Control-W Q is what ends it. It
-- answers `handlers.shortcuts` itself, and returns `key`.
--

return function(ctx)
  local EDIT_KEYS, EDIT_BINDINGS, KEY_CTRL_C, KEY_ESC, KEY_PREFIX =
    ctx.EDIT_KEYS, ctx.EDIT_BINDINGS, ctx.KEY_CTRL_C, ctx.KEY_ESC, ctx.KEY_PREFIX
  local KEY_TAB, OUT, focused_window, handlers =
    ctx.KEY_TAB, ctx.OUT, ctx.focused_window, ctx.handlers
  local move_window, post, raise, to_focused =
    ctx.move_window, ctx.post, ctx.raise, ctx.to_focused
  local windows =
    ctx.windows

  local keys = use("/Kosmos/Libraries/keys.lua")
  local prefs = use("/Kosmos/Libraries/prefs.lua")

  local STEP = 16
  local prefix = false
  local pending_escape = {}

  local function move_focused(dx, dy)
    local win = focused_window()
    if not win then return end
    move_window(win, win.x + dx, win.y + dy)

    -- Said, because a keyboard's move is otherwise visible only as pixels,
    -- and `tools/run_sysapps.py` holds it to where the window went.
    print(("wm: moved %s to %d,%d"):format(tostring(win.title), win.x, win.y))
  end

  --
  -- A prefix key, rather than a set of reserved ones.
  --
  -- The first version took Tab for "next window" and the arrows for "move the
  -- window". Both were wrong, and the gallery showed it in one screenshot:
  -- Tab is how every user interface ever built moves between controls, and
  -- the arrows are how every list is used. A window manager that keeps them
  -- has decided that no application may have a second control.
  --
  -- There are no modifiers to escape into. A virtio keyboard gives Control
  -- plus a letter and nothing else - no Alt, no Super, and Control plus an
  -- arrow is a terminal escape sequence this system does not speak. So this
  -- takes the approach screen and tmux took for exactly the same reason:
  -- **one** key is reserved, and it introduces a command rather than being
  -- one.
  --
  --   Control-W then an arrow    move the focused window
  --   Control-W then Tab         focus the next window
  --   Control-W then Control-W   send a literal Control-W to the application
  --
  --   Control-W then a           select everything in the focused control
  --   Control-W then c           copy the selection to the clipboard
  --   Control-W then x           cut it
  --   Control-W then v           paste
  --
  -- One key out of the application's vocabulary instead of five, and the one
  -- taken is the one applications want least.
  --
  -- **The clipboard four are here rather than as four more reserved keys**,
  -- and the reason is the paragraph above: there are no modifiers, so
  -- Control-C is a key an application can see, and in this system it is
  -- already the key that stops one. Nine checks in the display harness use
  -- it to get the screen back from the desktop, from `plasma`, from `cube3d`
  -- and from a terminal window, so it is not available and should not be.
  --
  -- Which leaves the choice between inventing a triple out of whatever
  -- control codes are unclaimed - and every candidate carries somebody's
  -- prior, `^Y` being paste to half the world and copy to nobody - or
  -- putting them behind the prefix that exists precisely because this
  -- machine has no Meta key. Behind the prefix the letters can be the ones
  -- everybody already knows, which is the whole point of a prefix.
  --
  -- **What crosses to the application is the intent, not the text.** This
  -- process does not know what a selection is; a text field does. So the
  -- prefix posts `{type = "copy"}` and the application answers by sending
  -- back a `clip_put` with whatever it decided that meant. The window
  -- manager holds the bytes and stays ignorant of them, which is the same
  -- division it already keeps with pixels.
  --
  --------------------------------------------------------------------------
  -- Round the open windows, without the pointer.
  --
  -- **Raise the bottom one.** `windows` is bottom-to-top, so the one that has
  -- been buried longest comes to the front and the one that was in front goes
  -- to the back of the queue - press it as many times as there are windows and
  -- you are back where you started. That is a real cycle rather than a swap
  -- between two, and it needs no "most recently used" list to fall out of step
  -- with what is on the screen.
  --
  -- **Scenery is not a window you can be hidden behind**, and this is the bug
  -- that was here: `raise(windows[1])` was written when `windows[1]` was an
  -- ordinary window, and by the time anybody pressed it `windows[1]` was the
  -- backdrop - which `raise` refuses outright, by design. So Control-W Tab
  -- had done nothing at all for as long as there had been a desktop. The same
  -- three flags `focusable` and the placement search already use.
  --
  -- A minimised window counts. It is an open application, which is what was
  -- asked for, and `raise` brings a hidden one back - so cycling walks
  -- through everything that is running rather than everything that happens to
  -- be visible, which is what every Alt-Tab since Windows 3 has done.
  --------------------------------------------------------------------------
  local function cycle_windows()
    for _, win in ipairs(windows) do
      if not (win.backdrop or win.strip or win.kind == "menu" or win.popup or win.tip) then
        raise(win)
        return
      end
    end
  end

  local function prefixed(c)
    prefix = false

    if c == KEY_TAB then
      cycle_windows()
      return
    end

    if c == KEY_PREFIX then
      to_focused(KEY_PREFIX)
      return
    end

    if c == KEY_ESC then
      -- An arrow after the prefix. The escape belongs to the sequence, not to
      -- the application, so the prefix stays on until the sequence finishes.
      prefix = true
      pending_escape = { KEY_ESC }
      return
    end

    --
    -- **Q ends the desktop**, and it is behind the prefix because ending the
    -- desktop is the most destructive thing this keyboard can do and should
    -- take two deliberate presses. It used to be Control-C, at the top of
    -- `key`, which is one press and the same one people use to copy.
    --
    -- Either case of the letter, because a prefix command is a command and
    -- nobody should have to notice the shift key to give one.
    --
    if c == 113 or c == 81 then                 -- q, Q
      OUT.running = false
      return
    end

    --
    -- And a literal Control-C through to the application, which is the same
    -- escape hatch `Control-W Control-W` is for the prefix itself.
    --
    -- Nothing needs it yet: the Terminal answers the console's `poll` with
    -- "no Control-C" and has never interrupted a child. It will one day, and
    -- on that day the key is already reachable - which is cheaper than
    -- discovering that the window manager has quietly taken the only way to
    -- stop a running program.
    --
    if c == 99 or c == 67 then                  -- c, C
      to_focused(KEY_CTRL_C)
      return
    end
  end

  --
  -- **Super, and why it needs a parser rather than a comparison.**
  --
  -- Windows on a PC keyboard, Command on an Apple one. `hal/keys.c` turns it
  -- into an escape sequence, because this system's input language is
  -- characters and escape sequences and a *held* modifier has nowhere else
  -- to go - xterm's two shapes (13 October 2026, for Magnet's keys): a key
  -- with a sequence of its own carries Super in its modifier number, Super
  -- with Right `ESC [ 1 ; 9 C`, with Alt as well `ESC [ 1 ; 11 C`; a
  -- character typed with Super is `ESC [ 27 ; 9 ; c ~`, the character in
  -- decimal; and Super tapped alone `ESC [ 1 ; 9 ~`.
  --
  -- Which means a sequence arrives a byte at a time, and every byte of it
  -- looks like the beginning of something an application was entitled to
  -- receive. So **a sequence is held until it ends**, and what turns out not
  -- to be Super's goes on to the window whole and in order: an editor's
  -- arrow, or Escape and then a bracket, must still arrive, or the window
  -- manager has quietly eaten somebody's keystroke.
  --
  -- That is the whole reason this is a state machine and not `if c == 27`.
  --
  local held_seq = nil            -- the bytes of a sequence not yet ended

  local ARROW = { [65] = keys.UP, [66] = keys.DOWN, [67] = keys.RIGHT, [68] = keys.LEFT }

  --
  -- **A whole sequence, as Super's chord** - its code, as `SUPER_KEYS` and
  -- the bindings name one: the character, 126 for Super alone, an arrow's
  -- `keys` number, each with Alt or Shift in it as `keys.with` puts them -
  -- or nil when it is not Super's.
  --
  local function super_chord(params, final)
    if params == "1;9" and final == 126 then return 126 end      -- tapped

    local p = {}

    for field in (params .. ";"):gmatch("([^;]*);") do p[#p + 1] = tonumber(field) or 0 end

    local mods = (p[2] or 1) - 1

    if mods < keys.SUPER or mods > 15 then return nil end

    mods = mods - keys.SUPER

    if final == 126 and p[1] == 27 and p[3] and p[3] > 0 and p[3] < 256 then
      return keys.with(p[3], mods & keys.ALT)    -- Shift is in the character
    end

    if ARROW[final] and (p[1] == 1) and (mods & keys.CTRL) == 0 then
      return keys.with(ARROW[final], mods)
    end

    return nil
  end

  --
  -- What Super plus a key does. True when it was ours, false to hand the
  -- keystroke back - which is what makes an unbound combination reach the
  -- application instead of vanishing.
  --
  -- A table rather than a chain of comparisons, for the reason `CLIP_KEYS`
  -- above gives: it is a lookup, and both cases of a letter being the same
  -- entry should be visible rather than argued. Nobody should have to notice
  -- the shift key to give a command.
  --
  local SUPER_KEYS

  local function super_command(c)
    local what = SUPER_KEYS[c]

    if not what then
      return false
    end

    what()
    return true
  end

  -- What was held, and a byte after it, on to the window.
  local function flush_held(extra)
    local was = held_seq

    held_seq = nil

    for _, b in ipairs(was or {}) do to_focused(b) end

    if extra then to_focused(extra) end
  end

  --
  -- The bindings themselves, filled in here because they need `focused_window`
  -- and the handlers above.
  --
  -- `~` is Super tapped alone, which opens the Kosmos menu - a modifier that
  -- means something by itself is unusual, and it is the one people reach for
  -- without being taught.
  --
  --
  -- Close, minimise and the launcher, each named so the table above reads as
  -- what it does rather than as how it is done.
  --
  -- All three go through the same handlers a message would, so a keystroke
  -- and a click on the Deskbar are the same operation - which is what stops
  -- the two drifting apart the first time one of them grows a rule.
  --
  local function close_focused()
    local win = focused_window()

    if win and not win.backdrop and not win.strip then
      handlers.close{ window = win.handle }
    end
  end

  local function minimise_focused()
    local win = focused_window()

    if win then
      handlers.minimise{ window = win.handle }
    end
  end
  --
  -- **The launcher pad**, which is a program rather than something built in
  -- here. The window manager starts applications; it does not draw dialogs,
  -- and a search box that can grow to search files has no business inside the
  -- compositor.
  --
  -- Started fresh each time and closing itself when it is done, so there is no
  -- window to hide, no state to keep, and nothing to go wrong while nobody is
  -- looking at it.
  --
  local function open_launchpad()
    handlers.launch{ program = "launchpad" }
  end

  --------------------------------------------------------------------------
  -- **One declaration, and the Shortcuts window reads it.**
  --
  -- These were a table keyed by character with a function in each slot, which
  -- is the right shape for dispatch and no shape at all for anything that
  -- wants to *list* them: `pairs` has no order, and a key code is not
  -- something to show a person. So the list is the array and `SUPER_KEYS` is
  -- built from it below.
  --
  -- The point is that a shortcut cannot be added without saying what it does.
  -- A hand-written cheat sheet in another file is a copy, and a copy of
  -- something that changes is a copy that will be wrong - this system already
  -- has `docs/cheatsheet.html`, written before any of these keys existed and
  -- silently missing all of them.
  --
  -- Each has an `action`, the name a person's shortcut points at
  -- (`docs/shortcuts.md`), and a `group` for Shortcuts' sidebar; `fixed` is
  -- one that cannot be moved - Super alone and Super+Tab, which the menu and
  -- the switcher read as held keys rather than as a chord. A letter without
  -- Shift also answers with it, unless that chord is somebody else's
  -- (`build_super_keys`): "Super+Q or Super+Shift+Q" is noise in a list
  -- somebody is reading to learn one thing.
  --------------------------------------------------------------------------
  local SUPER_BINDINGS = {
    {
      key = 126, shown = "Super", action = "menu", group = "desktop", fixed = true,
      what = "Open the Kosmos menu",
      run = function()
        --
        -- **Posted, and that word is the whole of what was wrong twice.**
        --
        -- This was `fs.send` and then `fs.write`. Both are *calls*: they wait
        -- for a reply, and a reply from the key path of the compositor is a
        -- reply the compositor is not running to receive. One press of the
        -- Windows key and the desktop stopped reading the keyboard - no error,
        -- no crash, just a machine that ignores you.
        --
        -- `post` appends to the window's queue and returns, which is what
        -- every mouse press and close request already does. Nothing in here
        -- may block: this runs between reading a key and reading the next one.
        --
        -- **Unless Preferences says it does nothing** - Keyboard's "Pressed
        -- alone", held in `OUT.keys` rather than read here, for this
        -- comment's reason.
        if OUT.keys.super == "nothing" then return end

        -- The Super a window was just moved with, let go (`OUT.chord`).
        if OUT.chord.super_moved then
          OUT.chord.super_moved = false
          print("wm: Super moved a window, so its tap opens nothing")
          return
        end

        OUT.open_kosmos_menu()
      end,
    },

    { key = 32, shown = "Super+Space", action = "launchpad", group = "desktop",
      what = "Start something by typing part of its name",
      run = function() open_launchpad() end },

    { key = 47, shown = "Super+/", action = "shortcuts", group = "desktop",
      what = "Shortcuts, this window",
      run = function() handlers.launch{ program = "shortcuts" } end },

    --
    -- **Super and º** (Diego, 3 October 2026): every shortcut, modal. The
    -- key left of 1 - º on his Spanish keyboard - which the one keymap Kosmos
    -- has (`hal/keys.c`, US) reads as the grave accent, so it is bound by
    -- that: the same key whatever is printed on it.
    --
    { key = 35, shown = "Super+Shift+3", action = "capture", group = "screen",
      what = "A picture of the whole screen, into Captures",
      run = function() handlers.launch{ program = "screenshot" } end },

    { key = 96, shown = "Super+º", action = "shortcuts_over", group = "desktop",
      what = "Every shortcut, over everything until Escape",
      run = function() handlers.launch{ program = "shortcuts", args = "--modal" } end },

    --
    -- **The app switcher** (`docs/switcher.html`, Diego, 10 October): its
    -- Tab is the event's, taken by `wm/switcher.lua` before any of this, so
    -- the character Super+Tab also makes does nothing here but be kept from
    -- the window. Alt+Tab steps the windows, which Super+Tab used to.
    --
    { key = 9, shown = "Super+Tab", action = "switcher", group = "desktop", fixed = true,
      what = "Show the running applications", note = "the app switcher",
      run = function() end },

    { key = 113, shown = "Super+Q", action = "close", group = "desktop",
      what = "Close the window in front",
      run = function() close_focused() end },

    { key = 104, shown = "Super+H", action = "hide", group = "desktop",
      what = "Get the window in front out of the way",
      run = function() minimise_focused() end },

    -- **Game mode for the application in front**, or off again (Diego, 11
    -- October: "Windows has this super+g key shortcut that sets game mode
    -- for the app in front"; `docs/scheduler.md` G3).
    { key = 103, shown = "Super+G", action = "game_mode", group = "desktop",
      what = "Game mode for the application in front, or off",
      run = function() OUT.game.toggle_front() end },

    --
    -- **Magnet from the keyboard** (`docs/magnet.html`, agreed 10 October;
    -- the keys' encoding 13 October): the window in front put where a drag
    -- to an edge or a corner would put it, and more. Thirds are Super+Alt
    -- with a number rather than the mockup's Super with one, since Super
    -- with a number is the browser's tabs; Shortcuts moves any of them.
    --
    { key = keys.LEFT, shown = "Super+Left", action = "snap_left", group = "windows",
      what = "The window in front on the left half",
      run = function() OUT.magnet_key("left") end },
    { key = keys.RIGHT, shown = "Super+Right", action = "snap_right", group = "windows",
      what = "The window in front on the right half",
      run = function() OUT.magnet_key("right") end },
    { key = keys.UP, shown = "Super+Up", action = "snap_whole", group = "windows",
      what = "The window in front over the whole room",
      run = function() OUT.magnet_key("whole") end },
    { key = keys.DOWN, shown = "Super+Down", action = "snap_back", group = "windows",
      what = "The window in front back as it was",
      run = function() OUT.magnet_key("back") end },
    { key = keys.with(keys.UP, keys.ALT), shown = "Super+Alt+Up", action = "snap_quarter_up",
      group = "windows", what = "The window in front in the upper quarter on its side",
      run = function() OUT.magnet_key("quarter_up") end },
    { key = keys.with(keys.DOWN, keys.ALT), shown = "Super+Alt+Down", action = "snap_quarter_down",
      group = "windows", what = "The window in front in the lower quarter on its side",
      run = function() OUT.magnet_key("quarter_down") end },
    { key = keys.with(keys.LEFT, keys.ALT), shown = "Super+Alt+Left", action = "snap_quarter_left",
      group = "windows", what = "The window in front in the left quarter of its half",
      run = function() OUT.magnet_key("quarter_left") end },
    { key = keys.with(keys.RIGHT, keys.ALT), shown = "Super+Alt+Right", action = "snap_quarter_right",
      group = "windows", what = "The window in front in the right quarter of its half",
      run = function() OUT.magnet_key("quarter_right") end },
    { key = keys.with(keys.UP, keys.SHIFT), shown = "Super+Shift+Up", action = "snap_top",
      group = "windows", what = "The window in front on the top half",
      run = function() OUT.magnet_key("top") end },
    { key = keys.with(keys.DOWN, keys.SHIFT), shown = "Super+Shift+Down", action = "snap_bottom",
      group = "windows", what = "The window in front on the bottom half",
      run = function() OUT.magnet_key("bottom") end },
    { key = keys.with(49, keys.ALT), shown = "Super+Alt+1", action = "snap_third_1", group = "windows",
      what = "The window in front in the left third",
      run = function() OUT.magnet_key("third_1") end },
    { key = keys.with(50, keys.ALT), shown = "Super+Alt+2", action = "snap_third_2", group = "windows",
      what = "The window in front in the middle third",
      run = function() OUT.magnet_key("third_2") end },
    { key = keys.with(51, keys.ALT), shown = "Super+Alt+3", action = "snap_third_3", group = "windows",
      what = "The window in front in the right third",
      run = function() OUT.magnet_key("third_3") end },
    { key = keys.with(52, keys.ALT), shown = "Super+Alt+4", action = "snap_two_thirds_left",
      group = "windows", what = "The window in front in the left two thirds",
      run = function() OUT.magnet_key("two_thirds_left") end },
    { key = keys.with(53, keys.ALT), shown = "Super+Alt+5", action = "snap_two_thirds_right",
      group = "windows", what = "The window in front in the right two thirds",
      run = function() OUT.magnet_key("two_thirds_right") end },
    { key = 99, shown = "Super+C", action = "snap_middle", group = "windows",
      what = "The window in front in the middle, its size kept",
      run = function() OUT.magnet_key("middle") end },
  }

  --
  -- And the prefix, which this process does not dispatch from a table - it is
  -- a chain of comparisons in `prefixed` - but which a person learning the
  -- keyboard needs in the same list. Named here rather than discovered,
  -- which is a copy and is admitted as one: the alternative is restructuring
  -- `prefixed` around a table to serve a window, and the four clipboard keys
  -- already live in `CLIP_KEYS`.
  --
  local PREFIX_BINDINGS = {
    -- And Alt+Tab, dispatched in `alt_tab` and named here for the same
    -- reason as the prefix's.
    { shown = "Alt+Tab",         group = "windows", what = "Go round the open windows" },
    { shown = "Alt+Shift+Tab",   group = "windows", what = "Back round the open windows" },
    { shown = "Ctrl+W,Q",        group = "desktop", what = "End the desktop" },
    { shown = "Ctrl+W,Tab",      group = "windows", what = "Go round the open windows" },
    { shown = "Ctrl+W,Arrows",   group = "windows", what = "Move the window in front" },
    { shown = "Ctrl+W,C",        group = "windows", what = "Send a real Control-C through" },
    { shown = "Ctrl+W,Ctrl+W",   group = "windows", what = "Send a real Control-W through" },

    -- And a capture's other two keys (`capture_key` in `wm.lua`): Print
    -- Screen, a full-size keyboard's, and Control Alt 1, Diego's of 22
    -- September for a keyboard without one.
    { shown = "PrtSc",           group = "screen",  what = "A picture of the whole screen, into Captures" },
    { shown = "Ctrl+Alt+1",      group = "screen",  what = "The same, for a keyboard without Print Screen" },
  }

  --
  -- And the ordinary ones, so the window says what the whole keyboard does
  -- rather than only the unusual half of it, are `EDIT_BINDINGS`, declared
  -- with `wm.lua`'s dispatch of them rather than copied here.
  --

  --------------------------------------------------------------------------
  -- **A person's shortcuts** (`docs/shortcuts.md`, K2): made and changed in
  -- Shortcuts and kept as the setting `/Home/Preferences/shortcuts`, which
  -- holds only what differs from the bindings above, one entry each, each
  -- saying in full what it does:
  --
  --   { keys = "Super+T", action = "open", program = "/Kosmos/Apps/terminal.lua" }
  --   { keys = "Super+M", action = "hide" }          a system key moved
  --   { keys = "",        action = "close" }         one left without keys
  --
  -- Only Super chords: Control is each application's own. Read when the
  -- window manager starts, and handed again by Shortcuts in a message when
  -- it changes (`handlers.keys`), never read from the disk in a key path.
  --------------------------------------------------------------------------
  local SHIFTED = {}                   -- a byte typed with Shift, to its key

  do
    local pairs_ = "1!2@3#4$5%6^7&8*9(0)-_=+[{]}\\|;:'\",<.>/?`~"

    for i = 1, #pairs_, 2 do SHIFTED[pairs_:byte(i + 1)] = pairs_:sub(i, i) end
  end

  -- The key after Super, as a chord's last word, and back. º is the key
  -- left of 1, which the one keymap (US) reads as the grave accent.
  local function key_word(byte)
    if byte == 32 then return "Space", false end
    if byte == 9 then return "Tab", false end
    if byte == 96 then return "º", false end
    if byte >= 65 and byte <= 90 then return string.char(byte), true end
    if byte >= 97 and byte <= 122 then return string.char(byte - 32), false end
    if SHIFTED[byte] then
      local k = SHIFTED[byte]

      return (k == "`") and "º" or k, true
    end
    if byte > 32 and byte < 127 then return string.char(byte), false end

    return nil
  end

  local ARROW_WORD = { [keys.UP] = "Up", [keys.DOWN] = "Down",
                       [keys.RIGHT] = "Right", [keys.LEFT] = "Left" }

  -- A chord's code (`super_chord`) as the chord a person pressed: a
  -- character, or an arrow, with Alt or Shift where they were held.
  local function byte_chord(code)
    if code == 126 then return "Super" end

    local key, mods = keys.parts(code)
    local alt = (mods & keys.ALT) ~= 0 and "Alt+" or ""
    local word, shift

    if ARROW_WORD[key] then
      word, shift = ARROW_WORD[key], (mods & keys.SHIFT) ~= 0
    elseif key >= 0 then
      word, shift = key_word(key)
    end

    if not word then return nil end

    return "Super+" .. (shift and "Shift+" or "") .. alt .. word
  end

  -- And a chord back into its code; nil for one that is not Super and one
  -- more key, with Shift, Alt or neither.
  local BYTE_OF = {}

  for byte = 32, 126 do
    for _, mods in ipairs({ 0, keys.ALT }) do
      local code = keys.with(byte, mods)
      local chord = byte_chord(code)

      if chord and not BYTE_OF[chord] then BYTE_OF[chord] = code end
    end
  end

  for arrow in pairs(ARROW_WORD) do
    for _, mods in ipairs({ 0, keys.ALT, keys.SHIFT }) do
      BYTE_OF[byte_chord(keys.with(arrow, mods))] = keys.with(arrow, mods)
    end
  end

  BYTE_OF["Super+Tab"] = 9

  local function chord_byte(chord)
    local steps = keys.caps(chord)

    if not steps or #steps ~= 1 then return nil end

    return BYTE_OF[table.concat(steps[1], "+")]
  end

  local ACTION = {}

  for _, b in ipairs(SUPER_BINDINGS) do ACTION[b.action] = b end

  -- What each action's keys are now, and the person's own: rebuilt whenever
  -- the setting changes, and read by the key path as one table lookup.
  local now_keys, theirs = {}, {}

  -- An entry as it may stand: fixed fields, a chord this can dispatch, an
  -- action that exists and may be moved, a program that is a path. Anything
  -- else is dropped and said, never half applied.
  local function checked(e)
    if type(e) ~= "table" or type(e.keys) ~= "string" or type(e.action) ~= "string" then
      return nil, "not an entry"
    end

    if e.keys ~= "" and not chord_byte(e.keys) then
      return nil, e.keys .. " is not Super and one key"
    end

    if e.action == "open" then
      if e.keys == "" or type(e.program) ~= "string" or e.program:sub(1, 1) ~= "/"
         or e.program:find("%c") then
        return nil, "an open needs keys and a program's path"
      end

      return { keys = e.keys, action = "open", program = e.program }
    end

    local b = ACTION[e.action]

    if not b or b.fixed then return nil, e.action .. " cannot be moved" end

    return { keys = e.keys, action = e.action }
  end

  local function build_super_keys(entries)
    local table_, by_chord = {}, {}

    now_keys, theirs = {}, {}

    for _, b in ipairs(SUPER_BINDINGS) do now_keys[b.action] = b.shown end

    for _, raw in ipairs(type(entries) == "table" and entries or {}) do
      local e, why = checked(raw)

      if not e then
        print("wm: a shortcut set aside: " .. tostring(why))
      elseif e.action == "open" then
        theirs[#theirs + 1] = e
      else
        now_keys[e.action] = e.keys
      end
    end

    local function claim(chord, run, name)
      local byte = chord_byte(chord)

      if not byte then return end

      if by_chord[byte] then
        print(("wm: %s is both %s and %s; %s has it"):format(chord, by_chord[byte], name, name))
      end

      table_[byte], by_chord[byte] = run, name
    end

    for _, b in ipairs(SUPER_BINDINGS) do
      if now_keys[b.action] ~= "" then claim(now_keys[b.action], b.run, b.action) end
    end

    for _, e in ipairs(theirs) do
      claim(e.keys, function() handlers.launch{ program = e.program } end,
            "open " .. e.program)
    end

    -- A letter without Shift answers with it too, where nobody has that.
    for byte, run in pairs(table_) do
      if byte >= 97 and byte <= 122 and not table_[byte - 32] then
        table_[byte - 32] = run
      end
    end

    SUPER_KEYS = table_
    print(("wm: shortcuts: %d keys, %d of them a person's own"):format(
          #SUPER_BINDINGS, #theirs))
  end

  OUT.set_shortcuts = build_super_keys
  build_super_keys(prefs.read("shortcuts"))

  --
  -- **Listening** (`docs/shortcuts.md`): a Super chord is the window
  -- manager's and never reaches a window, so Shortcuts, recording one, asks
  -- for the next to be posted to it rather than acted on. Only the window
  -- with the keyboard may ask, and listening ends the moment it does not
  -- have it - so no window in the background can read a person's chords.
  --
  OUT.listening = nil

  function OUT.listen(win)
    OUT.listening = win
    print(win and ("wm: listening for keys for " .. tostring(win.title))
              or "wm: not listening for keys")
  end

  local function listened(byte)
    local win = OUT.listening

    if not win then return false end

    if win ~= focused_window() then
      OUT.listen(nil)
      return false
    end

    local chord = byte_chord(byte)

    if chord then
      post(win, { type = "chord", keys = chord })
      print("wm: heard " .. chord .. " for " .. tostring(win.title))
    end

    return true
  end

  OUT.listened = listened

  --
  -- What the keyboard does, for whoever asks.
  --
  -- The window manager answers because the window manager is what decides:
  -- these keys are taken before any application sees them, so a list compiled
  -- anywhere else would be a guess about another process's behaviour. Each
  -- row with the keys in force, its group, and for a Super key its action,
  -- its default and whether it can be moved; then the person's own.
  --
  -- **In pages**, as the window list is (`wmproto.windows`): a row with its
  -- group, action and default is about two hundred bytes, and twenty-odd of
  -- them do not fit in one message - the first version said so as "value
  -- does not fit in a message" and the window drew nothing. A person's own
  -- shortcuts are rows of the group `yours`. `wmproto.shortcuts` gathers
  -- the pages.
  --
  local SHORTCUTS_PAGE = 6

  handlers.shortcuts = function(req)
    local rows = {}

    for _, b in ipairs(SUPER_BINDINGS) do
      rows[#rows + 1] = { keys = now_keys[b.action], what = b.what, note = b.note,
                          group = b.group, action = b.action, default = b.shown,
                          fixed = b.fixed == true }
    end

    -- Not a key's, so not in the table keys are looked up in: the pointer's.
    rows[#rows + 1] = { keys = "Super+Ctrl+Drag", what = "Move a window from anywhere in it",
                        group = "windows", fixed = true }

    for _, b in ipairs(PREFIX_BINDINGS) do
      rows[#rows + 1] = { keys = b.shown, what = b.what, group = b.group, fixed = true }
    end

    for _, b in ipairs(EDIT_BINDINGS) do
      rows[#rows + 1] = { keys = b.keys, what = b.what, group = "text", fixed = true }
    end

    for _, e in ipairs(theirs) do
      rows[#rows + 1] = { keys = e.keys, action = "open", program = e.program, group = "yours" }
    end

    local from = math.max(1, math.floor(tonumber(req and req.from) or 1))
    local last = math.min(#rows, from + SHORTCUTS_PAGE - 1)
    local page = {}

    for i = from, last do page[#page + 1] = rows[i] end

    return { ok = true, rows = page, more = (last < #rows) and (last + 1) or nil }
  end

  --
  -- **Alt+Tab** (`docs/switcher.html`): the windows stepped, with nothing
  -- shown - what Super+Tab did before the switcher. A character does not
  -- carry Alt, so Alt+Tab arrives as a Tab, and Alt+Shift+Tab as Shift+Tab's
  -- `ESC [ Z`: both kept from the window while the left Alt is held - not the
  -- right, which is AltGr on a Spanish keyboard and types @ - and acted on
  -- here.
  --
  -- Back one: the window in front under every other - each other raised in
  -- its order, which is the inverse of `cycle_windows` raising the bottom.
  local function cycle_back()
    local front = focused_window()
    local order = {}

    if not front then return end

    for _, other in ipairs(windows) do
      if other ~= front and not (other.backdrop or other.strip or other.kind == "menu"
                                 or other.popup or other.tip) then
        order[#order + 1] = other
      end
    end

    for _, other in ipairs(order) do raise(other) end
  end

  --
  -- Alt+Tab here, and Alt+Shift+Tab where every sequence is read whole
  -- (`ESC [ Z`, in `key` below): it used to be collected here too, which
  -- took any sequence typed with Alt held - Super+Alt with an arrow among
  -- them - and handed its first two bytes straight to the window.
  --
  local function alt_tab(c)
    if c == 9 and OUT.chord.held[56] then
      cycle_windows()
      print("wm: Alt+Tab")
      return true
    end

    return false
  end

  local function key(c)
    -- While the app switcher shows, every character is its - its keys are
    -- the events' - and none reaches the window under it.
    if OUT.switcher and OUT.switcher.showing then return end

    if c ~= nil and alt_tab(c) then return end


    --
    -- **Given nil, the input just read has ended** (`roadmap.md` 6zz l3):
    -- Super's sequence only begun was not Super - the board writes its
    -- bytes together (`hal/keys.c`), so the rest would have come with them -
    -- and what was held goes on to the window, an Escape pressed alone the
    -- commonest of it. Without this a lone Escape waited in here for the
    -- next key: the browser's Escape stopped nothing until another came.
    --
    if c == nil then
      if held_seq then flush_held(nil) end

      return
    end

    --
    -- Collecting a sequence: `ESC`, `[`, its parameters, and the byte that
    -- ends it. Super's is acted on; anything else goes on whole.
    --
    if held_seq then
      if #held_seq == 1 then
        if c == 91 then                                   -- [
          held_seq[2] = c
          return
        end

        flush_held(nil)
        -- fall through: an Escape that started nothing, and this byte
      elseif c >= 0x20 and c <= 0x3f and #held_seq < 18 then
        held_seq[#held_seq + 1] = c                       -- a parameter
        return
      elseif c >= 0x40 and c <= 0x7e then
        local params = string.char(table.unpack(held_seq, 3))
        local chord = super_chord(params, c)

        -- Alt+Shift+Tab: back round the windows.
        if params == "" and c == 90 and OUT.chord.held[56] then
          held_seq = nil
          cycle_back()
          print("wm: Alt+Shift+Tab")
          return
        end

        -- Shortcuts recording a chord has it, rather than its action; a
        -- binding acts; and what is neither was never ours to keep.
        if chord and (listened(chord) or super_command(chord)) then
          held_seq = nil
          return
        end

        flush_held(c)
        return
      else
        flush_held(nil)
        -- fall through: the sequence was cut short, and this byte is a key
      end
    end

    --
    -- Halfway through an escape sequence that began after the prefix: an
    -- arrow, to move the window in front. **Before Super's collecting**, and
    -- that order is the fix: Super's begins with the same Escape and took it
    -- first, so `Control-W` then an arrow handed the arrow to the application
    -- and moved nothing, from the day Super arrived - nothing tested it.
    --
    -- **Read whole**, parameters and all: an arrow held with Shift is
    -- `ESC [ 1 ; 2 C` since the board carries modifiers (`roadmap.md` 6n,
    -- step 0), and a sequence read as three bytes left `;2C` behind to be
    -- typed. Only a plain arrow moves the window; any other is dropped.
    --
    if #pending_escape > 0 then
      if #pending_escape == 1 then
        if c == 91 then                                   -- '['
          pending_escape[2] = c
          return
        end

        pending_escape = {}
        prefix = false
        to_focused(c)
        return
      end

      if c >= 0x20 and c <= 0x3f and #pending_escape < 16 then
        pending_escape[#pending_escape + 1] = c           -- a parameter
        return
      end

      local plain = (#pending_escape == 2)

      pending_escape = {}
      prefix = false

      if not plain then return end

      if c == 65 then move_focused(0, -STEP) return end   -- A, up
      if c == 66 then move_focused(0,  STEP) return end   -- B, down
      if c == 67 then move_focused( STEP, 0) return end   -- C, right
      if c == 68 then move_focused(-STEP, 0) return end   -- D, left
      return
    end

    if prefix then
      prefixed(c)
      return
    end

    if c == 27 and not held_seq then
      held_seq = { 27 }
      return
    end

    if c == KEY_PREFIX then
      prefix = true
      return
    end

    --
    -- Copy, cut, paste and select-all, after the prefix has had its say so
    -- that `Control-W c` can still mean something else.
    --
    -- **What crosses is the intent, not the key.** This process does not know
    -- what a selection is; a text field does. So it posts `{type = "copy"}`
    -- and the application answers with whatever it decided that meant - which
    -- is why an application not built on the widget kit at all gets these
    -- without a line of its own, if it reads the intent - as Lite XL did.
    --
    local edit = EDIT_KEYS[c]

    if edit then
      post(focused_window(), { type = edit })
      return
    end

    to_focused(c)
  end

  return key
end
