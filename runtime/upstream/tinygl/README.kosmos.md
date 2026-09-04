# TinyGL, vendored

Upstream: <https://github.com/erysdren/TinyGL>
Commit:   `7f229c8a03b7134889ad7868af787857c3117feb` (2026-02-09)
Licence:  MIT — Fabrice Bellard 1997–2022, erysdren 2023–2025. See `COPYING`.

**Unmodified, byte for byte**, which is the rule every vendored thing here
follows: what is in the tree is what the author released, and everything done
to it is a build step somebody can read. No Kosmos copyright line is added to
these files — adding one would be modifying them, which is the one thing that
rule forbids.

## What was left out, and why

`examples/ui_sdl2.c`, `examples/ui_sdl3.c` and `examples/thirdparty/` are not
here. They need SDL, which this machine does not have and is not going to:
carrying them would be carrying a promise. `examples/ui_headless.c` *is*
here, and it is the interesting one — it shows the shape a backend takes, and
Kosmos's own backend is written against the same `ui.h`.

## Why TinyGL rather than a real GL

There is no GPU driver here and there is not going to be one soon. Mesa is
millions of lines and wants a hosted C++ toolchain; virgl needs a host GPU
and a guest driver that speaks it. TinyGL is a software rasteriser in seven
thousand lines of C that draws into a plain framebuffer, which is exactly the
machine this is.

By `CLAUDE.md`'s rule it is a **kit**: a finished algorithm, in C, reached
through the namespace as `use("/kits/gl")`. A finished algorithm has no
reload to lose, so the usual price of C is not paid — the OpenGL 1.x API is
not going to change.

## What it needs from the libc

`malloc`, `free`, `calloc`, `realloc`, `memcpy`, `memset`, `assert`, and from
`math.h`: `sin`, `cos`, `tan`, `sqrt`, `pow`, `floor`, `fabs`. Kosmos has all
of them; the trigonometric ones come from the toolchain's libm, which Lua
already uses.

`fprintf` and `printf` appear in its error paths only.
