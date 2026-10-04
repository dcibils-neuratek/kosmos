-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Keys the window manager keeps: Super and its bindings, Control-W and
-- what follows it, the windows round with the keyboard - a part of
-- `wm.lua` in a file of its own (`roadmap.md` 6zn). The section as it
-- stood there, made by a function handed what it reads of the rest; the
-- loop's `running` is `OUT`'s, since Control-W Q is what ends it. It
-- answers `handlers.shortcuts` itself, and returns `key`.
--

return function(ctx)
  local EDIT_KEYS, KEY_CTRL_C, KEY_ESC, KEY_PREFIX =
    ctx.EDIT_KEYS, ctx.KEY_CTRL_C, ctx.KEY_ESC, ctx.KEY_PREFIX
  local KEY_TAB, OUT, focused_window, handlers =
    ctx.KEY_TAB, ctx.OUT, ctx.focused_window, ctx.handlers
  local move_window, post, raise, to_focused =
    ctx.move_window, ctx.post, ctx.raise, ctx.to_focused
  local windows =
    ctx.windows

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
  -- into `ESC [ 1 ; 9 x` for a combination and `ESC [ 1 ; 9 ~` for a tap,
  -- because this system's input language is characters and escape sequences
  -- and a *held* modifier has nowhere else to go.
  --
  -- Which means six bytes arrive one at a time, and five of them look exactly
  -- like the beginning of something an application was entitled to receive.
  -- So they are **buffered and flushed through** when they turn out not to be
  -- ours: an editor pressing Escape and then a bracket must still get both,
  -- in order, or the window manager has quietly eaten somebody's keystroke.
  --
  -- That is the whole reason this is a state machine and not `if c == 27`.
  --
  local SUPER_HEAD = { 27, 91, 49, 59, 57 }       -- ESC [ 1 ; 9
  local super_at = 0

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

  local function flush_super(upto, extra)
    local i

    for i = 1, upto do
      to_focused(SUPER_HEAD[i])
    end

    if extra then to_focused(extra) end

    super_at = 0
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
  -- `also` is the other case of a letter. Both dispatch; only one is shown,
  -- because "Super + Q or Super + Shift + Q" is noise in a list somebody is
  -- reading to learn one thing.
  --------------------------------------------------------------------------
  local SUPER_BINDINGS = {
    {
      key = 126, shown = "Super",
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

    { key = 32, shown = "Super + Space",
      what = "Start something by typing part of its name",
      run = function() open_launchpad() end },

    { key = 47, shown = "Super + /",
      what = "This window",
      run = function() handlers.launch{ program = "shortcuts" } end },

    --
    -- **Super and º** (Diego, 3 October 2026): every shortcut, modal. The
    -- key left of 1 - º on his Spanish keyboard - which the one keymap Kosmos
    -- has (`hal/keys.c`, US) reads as the grave accent, so it is bound by
    -- that: the same key whatever is printed on it.
    --
    { key = 96, shown = "Super + º",
      what = "Every shortcut, over everything until Escape",
      run = function() handlers.launch{ program = "shortcuts", args = "--modal" } end },

    { key = 9, shown = "Super + Tab",
      what = "Go round the open windows",
      run = function() cycle_windows() end },

    { key = 113, also = 81, shown = "Super + Q",
      what = "Close the window in front",
      run = function() close_focused() end },

    { key = 104, also = 72, shown = "Super + H",
      what = "Get the window in front out of the way",
      run = function() minimise_focused() end },
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
    { shown = "Control-W Q",      what = "End the desktop" },
    { shown = "Control-W Tab",    what = "Go round the open windows" },
    { shown = "Control-W arrows", what = "Move the window in front" },
    { shown = "Control-W C",      what = "Send a real Control-C through" },
    { shown = "Control-W Control-W", what = "Send a real Control-W through" },
  }

  --
  -- And the ordinary ones, which are here so the window says what the whole
  -- keyboard does rather than only the unusual half of it.
  --
  local EDIT_BINDINGS = {
    { shown = "Control-A", what = "Select everything" },
    { shown = "Control-C", what = "Copy" },
    { shown = "Control-X", what = "Cut" },
    { shown = "Control-V", what = "Paste" },
  }

  SUPER_KEYS = {}

  for _, b in ipairs(SUPER_BINDINGS) do
    SUPER_KEYS[b.key] = b.run

    if b.also then SUPER_KEYS[b.also] = b.run end
  end

  --
  -- What the keyboard does, for whoever asks.
  --
  -- The window manager answers because the window manager is what decides:
  -- these keys are taken before any application sees them, so a list compiled
  -- anywhere else would be a guess about another process's behaviour.
  --
  handlers.shortcuts = function()
    local keys, prefixes, edits = {}, {}, {}

    for _, b in ipairs(SUPER_BINDINGS) do
      keys[#keys + 1] = { shown = b.shown, what = b.what }
    end

    -- Not a key's, so not in the table keys are looked up in: the pointer's.
    keys[#keys + 1] = { shown = "Super + Control + drag",
                        what = "Move a window from anywhere in it" }

    for _, b in ipairs(PREFIX_BINDINGS) do
      prefixes[#prefixes + 1] = { shown = b.shown, what = b.what }
    end

    for _, b in ipairs(EDIT_BINDINGS) do
      edits[#edits + 1] = { shown = b.shown, what = b.what }
    end

    return { ok = true, super = keys, prefix = prefixes, edit = edits }
  end

  local function key(c)
    --
    -- **Given nil, the input just read has ended** (`roadmap.md` 6zz l3):
    -- Super's sequence only begun was not Super - the board writes its
    -- bytes together (`hal/keys.c`), so the rest would have come with them -
    -- and what was held goes on to the window, an Escape pressed alone the
    -- commonest of it. Without this a lone Escape waited in here for the
    -- next key: the browser's Escape stopped nothing until another came.
    --
    if c == nil then
      if super_at > 0 then
        local was = super_at

        super_at = 0
        flush_super(was, nil)
      end

      return
    end

    --
    -- Collecting `ESC [ 1 ; 9`. Each byte either continues the sequence or
    -- ends the attempt, and ending it hands back everything taken so far.
    --
    if super_at > 0 then
      if super_at == #SUPER_HEAD then
        local was = super_at

        super_at = 0

        if super_command(c) then
          return
        end

        -- Not a binding we have, so it was never ours to keep.
        flush_super(was, c)
        return
      end

      if c == SUPER_HEAD[super_at + 1] then
        super_at = super_at + 1
        return
      end

      -- A different sequence - an arrow, or a person pressing Escape. Give
      -- back what was taken and let this byte be handled on its own merits.
      local was = super_at

      super_at = 0
      flush_super(was, nil)
      -- fall through, so this byte is dispatched normally
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

    if c == SUPER_HEAD[1] and super_at == 0 then
      super_at = 1
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
