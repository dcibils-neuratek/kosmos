# luacheck, vendored

Upstream: <https://github.com/lunarmodules/luacheck>, tag `v1.2.0`, commit
`cc089e3f65acdd1ef8716cc73a3eca24a6b845e4`, downloaded on 27 September 2026
as the tag's source archive:

    8efe62a7da4fdb32c0c22ec1f7c9306cbc397d7d40493c29988221a059636e25  luacheck-1.2.0.tar.gz

Licence:  MIT, Peter Melnichenko and contributors. See `LICENSE`, taken
          byte for byte from the archive.

The archive's `src/` and `LICENSE`, **unmodified** - 55 Lua files under
`src/luacheck/`, the command line's and the checker's alike, and the licence
of the SHA-1 luacheck itself vendors, `src/luacheck/vendor/sha1/LICENSE`
(MIT; used by its cache, which the IDE never loads).

## What it is for

The Kosmos IDE's checking (`roadmap.md` 6n, step 4): what would go wrong
once a Lua file runs - a name used and never set, a local set and never
used, one that hides another. Diego chose it on 26 September ("luacheck").
It is carried in the image as `/lib/luacheck/` and loaded by
`user/lib/lint.lua`.

## How it runs, as steps rather than edits

- **In an environment of its own.** It is written for a Lua with
  `require`, `package`, `io` and `os`, and a Kosmos program has none of
  them. `lint.lua` gives it a `require` that finds its modules under
  `/lib/luacheck/`, and answers the three questions it asks of the others
  as it loads - `package.config` for the path separator, `os.getenv` for
  colour, `io.type` - harmlessly. Nothing it is given reaches the program
  being checked.
- **Told what a Kosmos program is**: Lua 5.4's standard names less `io`,
  `os`, `debug`, `package`, `require`, `dofile` and `loadfile`, and
  Kosmos's own - `tools/luaglobals.py`'s list - so `use` and `fs` are known
  and `io` is "a Kosmos program has no io; files are fs".
- Only `luacheck.check_strings` and `luacheck.get_message` are called. The
  command line (`main`, `runner`, `fs`, `cache`, the rest) is carried and
  never loaded.

## What was left out

The archive's documentation, specs, rockspecs, scripts and build files.

## The other luacheck

The host build has checked that every Lua file parses since August with a
tool of its own, which was `tools/luacheck.c` - named before this arrived,
and only a parser. It is `tools/luaparse.c` now, which is what it does.
