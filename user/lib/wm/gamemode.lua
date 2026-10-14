-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
-- Game mode: one application at a time whose work comes before every other
-- program's while it is in front (`docs/scheduler.md` G3) - a part of
-- `wm.lua` in a file of its own, as `switcher.lua` is.
--
-- Diego, 11 October 2026: "Let's do per app game mode yes" - **game mode
-- belongs to an application, never to the machine**. It comes to one:
--
--   - **by its header**: an application whose `kosmos: kind` is `game` or
--     `emulator` - Doom, Quake and the Super Nintendo say so already - has
--     it when its window opens. The design said a new word, `kosmos: game`;
--     the kind already said it, and a second word for one fact is a copy;
--   - **by Super+G**, for whatever application is in front, game or not -
--     and off again; "Windows has this super+g key shortcut";
--   - **from the Scheduler**, which asks which application (`handlers.game`
--     in `wm.lua`).
--
-- **The band follows the window in front.** While a window of the game's is
-- in front its process is in the game band - above every normal thread,
-- below the screen, the sound and the keys - and when another window comes
-- forward it is given back (`sys.set_game`, which only a parent may call:
-- this process opened the application). Looked at once a pass, which is a
-- comparison, and asked of the kernel only when it changes.
--
-- A notification when it starts and when it ends. It ends by itself when
-- the application's last window has gone.

return function(ctx)
  local focused_window, windows = ctx.focused_window, ctx.windows

  local filetypes = use("/Kosmos/Libraries/filetypes.lua")

  local G = { pid = nil, program = nil, title = nil, granted = false, seen = false }
  local is_game = {}                    -- a program's path -> its header says game

  local function notify(title, body)
    pcall(function()
      use("/Kosmos/Libraries/notify.lua").post{ title = title, body = body }
    end)
  end

  -- Given or taken back, and asked of the kernel only when that changes.
  local function grant(on)
    if not G.pid or G.granted == on then return end

    local ok, core = sys.set_game(G.pid, on)

    G.granted = on and ok and true or false
    G.core = on and ok and math.type(core) == "integer" and core or nil
    print(("wm: game band %s for %s%s%s"):format(on and "given" or "taken back", tostring(G.title),
          G.core and (", core " .. G.core .. " its own") or "",
          ok and "" or (" - " .. tostring(core))))
  end

  function G.stop(quiet)
    if not G.pid then return end

    local title = G.title

    grant(false)
    G.pid, G.program, G.title, G.granted, G.seen = nil, nil, nil, false, false
    print("wm: game mode off for " .. tostring(title))

    if not quiet then
      notify("Game mode off", tostring(title) .. " runs as every program does again.")
    end
  end

  function G.start(pid, program, title)
    if not pid or G.pid == pid then return end

    G.stop(true)
    G.pid, G.program, G.title, G.granted, G.seen = pid, program, title or program, false, false
    print(("wm: game mode on for %s (%s)"):format(tostring(G.title), tostring(pid)))
    notify("Game mode on for " .. tostring(G.title),
           "It comes before every other program while it is in front. Super+G turns it off.")
  end

  -- Super+G: the application in front into game mode, or out of it.
  function G.toggle_front()
    local w = focused_window()

    if not w or not w.pid then return end

    if G.pid == w.pid then
      G.stop()
    else
      G.start(w.pid, w.program, w.title)
    end
  end

  -- A window opened: an application that says it is a game has game mode.
  function G.opened(win)
    local path = win.program

    if not path or not win.pid then return end

    if is_game[path] == nil then
      local source = fs.read(path)

      local kind = type(source) == "string" and filetypes.declared(source, "kind") or nil

      is_game[path] = kind == "game" or kind == "emulator"
    end

    if is_game[path] and G.pid ~= win.pid then
      G.start(win.pid, path, win.title)
    end
  end

  -- Once a pass: the band to the game while it is in front, and game mode
  -- over once its last window has gone.
  function G.pass()
    if not G.pid then return end

    local any = false

    for _, w in ipairs(windows) do
      if w.pid == G.pid then any = true break end
    end

    if any then
      G.seen = true
    elseif G.seen then
      G.stop()
      return
    end

    local w = focused_window()

    grant(w ~= nil and w.pid == G.pid)
  end

  -- For the Scheduler: which application has it, and whether it is in front.
  function G.state()
    return { pid = G.pid, program = G.program, title = G.title, in_front = G.granted, core = G.core }
  end

  return G
end
