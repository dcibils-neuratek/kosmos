# Remote Screen: another computer's screen in a Kosmos window

A VNC client. The other side of what `vncd` already does for the M700: a
window showing a VNC server's screen - Diego's Mac's, first - with this
machine's keyboard and pointer sent back to it.

Diego, 12 October 2026: "is there any way we can create a quick vnc client
for kosmos so i can access my mac over vnc from kosmos?", then "yes add the
vnc client to the queue after sata". Designed before it is built, as every
application is (`CLAUDE.md`, *How to work here*): this file is its feature
set and its architecture; `docs/screens.html` its mockup;
`docs/screens-architecture.png` its diagram, drawn from
`docs/screens-architecture.html`. **None of it is built until Diego has
agreed all four.** The name is a working one.

## What it does

- **Shows another computer's screen** as it changes, fitted to the window
  and kept to its shape, or at actual size and scrolled.
- **Sends this machine's keys and pointer to it**, or only watches
  (*Control* off). Super is Command on a Mac; Super-Tab and the launcher
  pad's key stay Kosmos's, so the way out of the window is never lost. A
  **Keys** menu sends what the desktop would otherwise keep: Command-Tab,
  Command-Space, Command-Q, Mission Control, force quit, lock.
- **Finds the machines that share their screen** on the network
  (`_rfb._tcp`), **remembers the ones used**, and takes an address and
  port.
- **Signs in with a VNC password** - the one macOS Screen Sharing sets
  under "VNC viewers may control screen with password" - kept in the
  keyring if asked, never in a settings file.
- **Says how it is going** at the window's foot: connected, the remote
  size and the scale, the encoding, frames a second, the rate.
- **Full screen**, with a bar from the top edge, and back.
- **Says what went wrong**, each as itself: a wrong password; a machine
  that offers only Apple's sign-in; nothing answering; a connection that
  ended, over its last picture with *Connect again*.

Not in the first version, and on the roadmap behind it: **Apple's sign-in**
with the Mac's own account (RFB security type 30 - Diffie-Hellman, then
AES-128 - which the Crypto Kit's primitives cover); **the clipboard** both
ways; **more than one screen** at once; **the pointer drawn here** (the
Cursor pseudo-encoding), which only matters on a slow link.

## What it is made of

The premise (`CLAUDE.md`, *Kits, servers and drivers supply; applications
orchestrate*): the application is the window and what a person does with
it. Everything about VNC itself - the protocol, the pixels, the
compression, the sign-in - is a kit `vncd` uses too, so there is one copy of
RFB in the system and not two.

| Piece | What supplies it | New? |
|---|---|---|
| The window, its header, the sheet, the Keys menu, full screen | `ui.lua`, `paint.lua`, `wm` | no |
| **RFB's messages** - the handshake, security types, ServerInit, SetPixelFormat, SetEncodings, FramebufferUpdateRequest, KeyEvent, PointerEvent, the rectangle headers - **both sides** | **`rfb.lua`, the RFB kit**, taken out of `vncd.lua`, which keeps only being a server | **new kit**, from `vncd`'s own code |
| **Keysyms**, both ways: Kosmos's keys to X keysyms here, keysyms to Kosmos's keys in `vncd` (its `translate` today) | `rfb.lua` | moved, and the client's half new |
| VNC's sign-in: the challenge under DES with the password's bits reversed | the Crypto Kit's `crypto.des` - `vncd`'s `expected()` moves to `rfb.lua` | no |
| **TCP's bytes into a region**, never a Lua string: `conn:read_into(at, cap)` | the network kit (`net_kosmos.c`); the ring is already shared memory (`tcpring.h`) | **new method** |
| **zlib, inflated as a stream** - ZRLE's one stream for the whole connection: `zstream:inflate(src, n, dst, cap)` | the Compression Kit (`compress.zstream`, which only deflates today, for `vncd`) | **new method** |
| **ZRLE's tiles decoded into a surface**: `surface:zrle_decode(x, y, w, h, format, src, n)` | the Graphics Kit, beside the encoder `vncd` uses (`zrle.c`), held to it by `tools/test_zrle.c` | **new, in C** |
| **Raw rectangles**: `surface:unpack(x, y, w, h, format, src)`, the reverse of `surface:pack` | the Graphics Kit (`pack.c`), held to `pack` by `tools/test_pack.c` | **new, in C** |
| CopyRect: a rectangle moved within the picture | `surface:copy`, as it is | no |
| The picture fitted to the window | the Graphics Kit's drawing at another size, as Pictures uses | no |
| Machines on the network | `mdns.lua`'s `browse("_rfb._tcp")` - `mdnsd` hears them already | no |
| The machines used | the settings kit, `prefs.open("screens")` | no |
| The password | the keyring, through a door of its own, `kosmos: needs keyring-screens`, as Mail has `keyring-mail` | **a new door** in `keyring` |

**What is C and what is Lua.** Lua: the window, the sheet, the menus, the
handshake (a few dozen bytes once), the rectangle headers (twelve bytes a
rectangle), choosing what to ask for next. C: every loop over the picture's
bytes - from the ring, through the inflater, through the tile decoder, into
the surface - in the three kits above, each held on the Mac to a reference.

**What travels where.** Control by message, data by shared memory
(`CLAUDE.md`): the picture never becomes a Lua value. The connection's ring
is a region the network server and this process share; `read_into` copies
from it into a region of this process's; `inflate` writes into a second;
`zrle_decode` writes into the remote screen's surface, a region of its own.
Lua carries offsets and lengths between them. Keys and the pointer are a
handful of bytes a message, written as RFB asks.

## Its busiest path, and where the time goes

A frame of the Mac at 3440 by 1440 - when a window is dragged across it, or
a video plays - is the one path that matters. In order:

1. **The network**: bytes arrive in the connection's ring (`net`, C), and
   the window's loop is woken on `conn`, as Mail's is.
2. **Into a region**: `conn:read_into` - a copy, C, about 64 KB a call.
3. **The headers**: Lua reads twelve bytes of each rectangle's header and
   four of ZRLE's length out of the region, through `sys.region_read`.
4. **Inflate**: `zstream:inflate` - the one loop that is long, C. Measured
   on the M700 when it is built; zlib inflates at hundreds of megabytes a
   second, and a desktop's ZRLE is mostly one-colour and small-palette
   tiles, which inflate to little.
5. **Decode**: `surface:zrle_decode` writes the tiles' pixels into the
   remote screen's surface - mostly fills of one colour, the rest short
   palette runs. C, vectorised where it can be, as `pack.c` is.
6. **Fitted**: the changed rectangle drawn scaled into the window's surface,
   C, then committed with that rectangle as damage - the window manager
   composes it like any other window.

Steps 4 and 6 are where the time is expected to go; both are C. Nothing on
the path allocates a Lua value larger than a header. The rate it runs at is
the Mac's to choose: the client asks for the next update as soon as one is
drawn (incremental FramebufferUpdateRequest), so a busy screen is as many
frames as the network and these steps allow, and an idle one is none.

## The steps, once agreed

1. **The RFB kit, from `vncd`**: `rfb.lua` holding the messages and the
   keysyms; `vncd` moved onto it with nothing changed in what it does, held
   by its suite (`x86-vncd`) and `tools/kosmos_vnc.py`.
2. **The three C pieces**, each held on the Mac: `zrle_decode` to the
   encoder and to `test_zrle.c`'s own decoder; `unpack` to `pack`;
   `zstream:inflate` to `deflate` - and `read_into`.
3. **Remote Screen, connected**: to `vncd` on another Kosmos under QEMU in the
   suite - two machines, one showing the other's screen, the picture held
   to the other's own screendump - and then to the Mac.
4. **The first window**: machines found, machines remembered, the keyring.
5. **Full screen, the Keys menu, and the foot's numbers.**
