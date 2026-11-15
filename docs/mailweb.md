<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Mail's HTML on WebKit, before it is built

**Decided, 15 November 2026.** JetSki replaces the old browser ("we should
delete our old browser code and app as now jetski is much better
overall"). WebKit becomes a kit Mail uses too ("jetski in the system image,
and webkit for mail"). On how Mail reaches it, Diego chose **one WebKit
image**, over a page server and over text until later. The image JetSki
runs in becomes Kosmos's WebKit image, and Mail runs inside it as JetSki
does.

This is the piece of `docs/mail.md` that changes: *Reading a message*, HTML
(M5). Everything else in Mail stays as it is. The window is drawn in
`docs/mailweb.html`, and the pieces in `docs/mailweb-architecture.png`.

---

## What it does

### An HTML message, as its sender drew it

- **Laid out by WebKit**, the engine JetSki and Safari use: the sender's
  CSS, web fonts sent inside the message, tables, media queries for a
  narrow pane, `prefers-color-scheme`. NetSurf, which draws it today,
  stopped at CSS 2.1, which is why newsletters needed Mail's own fixes
  (`MAIL_SHEET`, `widths_as_wishes`).
- **Nothing in it runs.** Scripts are off for every message view, forms do
  nothing, and there are no plugins. A message is a document, never a
  program.
- **Text can be selected and copied**: drag, double-click a word,
  Control-C. NetSurf's pane could not do this.
- **Scrolled as JetSki scrolls**, by the wheel, the keys and the
  scroll bar.
- **Fitted when wider than the pane**, as today: a newsletter's fixed 600
  table in a narrower pane is shown smaller, down to half its size and no
  further, by WebKit's page zoom.

### Pictures

- **A picture sent inside the message** (`cid:`) is shown at once. Mail
  serves it from the Mail Kit's part, and nothing touches the network.
- **A picture on the network is held** until Load Pictures is pressed,
  because fetching one tells its sender the message was read. The bar above
  the message is the one Mail has now. A sender whose pictures always load
  is still a sender's setting.
- **Held means held**: no request leaves the machine for the message's
  stylesheets, fonts, pictures or tracking pixels until it is allowed.

### Links

- **Pressing a link opens it in JetSki**, in a new tab of the JetSki that
  is open, or in a new JetSki when none is. The message pane never
  navigates itself.
- **Hovering a link** shows its address in Mail's status line, so a person
  can see where it goes before pressing.

### When there is no WebKit

A machine without the WebKit image (`FULL=0`, or a disk that has not
received it yet) shows an HTML message as its text, as it does now, and
says so. Plain-text messages are unchanged: Write's engine sets them.

### Not here, and why

- **Web Inspector in Mail**: a message is not something to debug. JetSki
  has it.
- **Printing**: Mail has no printing yet. When Kosmos prints, WebKit's own
  page layout is the way to do it.
- **Inverting a message for the dark look**: a sender's colours are the
  sender's. Apple Mail inverts only messages that declare a dark scheme,
  and so would this, through `prefers-color-scheme`.

---

## The architecture

### At a glance

```
Mail (Lua, /Kosmos/Apps/mail.lua)            JetSki (Lua, /Kosmos/Apps/JetSki)
        \                                       /
         `---- run inside the WebKit image ----'
               /Kosmos/WebKit/webkit.elf  (Kosmos's own, on the disk)
                         |
               the WebKit Kit, /Kosmos/Kits/webkit   (C++, one door)
                 views - a page each, painted into the caller's surface
                         |  WebKit's own IPC
         a web process per view  ...  one network process
```

### The WebKit image

**JetSki's image becomes Kosmos's WebKit image.** `jetski.elf` is the
runtime, WebKit, and the kit that drives it; nothing in it is JetSki's
except its name. It is renamed `webkit.elf` and moves to a system folder of
its own, beside Web Inspector's front end, which is WebKit's too:

```
/Home/System/WebKit/webkit.elf          seen as /Kosmos/WebKit/webkit.elf
/Home/System/WebKit/Inspector/...       seen as /Kosmos/WebKit/Inspector/...
/Home/System/Apps/JetSki/jetski.lua     seen as /Kosmos/Apps/JetSki/jetski.lua
```

It is a package of kind `system`, so a stick's build writes it and
`systemapps` brings it at a netboot, as it brings JetSki now. Mail's own
file stays in the system image, in `/Kosmos/Apps`.

**A program names it by name**: `-- kosmos: image webkit`. Today an image
must be a file beside its program, so that a header cannot point a program
at an image somebody else put somewhere. That rule stays for every image
except one kind. **A system image is named, never given as a path**: the
name is looked up in `/Kosmos`'s own list, read only, served from the disk
the build wrote. Nothing a person writes can be named that way.

**What it costs, measured.** On the M700, JetSki's start, from launch to
its window, is 2.3 s, most of it reading 208 MB off the stick. The window
manager keeps an image it has read (`IMAGES.made`), so Mail after JetSki,
or JetSki after Mail, does not read it again. JetSki with one page open
took 272 MB more than the desktop without it (394 → 666 MB of 8049).
Mail with a message open will take about the same, and Mail with no HTML
message open has no web process at all.

### The WebKit Kit

**One door**, `/Kosmos/Kits/webkit`, in C++ in the image: JetSki's door
(`jetski.h`) made general. What it already has stays: a view of a size,
WebKit's run loop pumped a while at a time, the page painted into pixels
the caller owns, the pointer, the wheel and keys, and a link reported
rather than followed (`JETSKI_ASKED`). What changes:

- **Views rather than one page with tabs.** `webkit.view(w, h, options)`
  makes one, and a JetSki tab becomes a view. Mail makes one for its
  message pane.
- **A view's options**, fixed when it is made: `scripts` (off for Mail),
  `remote` (whether a page's own loads may reach the network), `schemes`
  (the URL schemes this process answers, `cid` for Mail), `follow` (whether
  a link is followed or only reported).
- **`view:html(text, base)`**: a page given as a string, not fetched. A
  message's HTML is in Mail already.
- **A scheme answered by the caller**: when the page asks for
  `cid:part3@example`, the kit reports it and Mail answers with the bytes
  and their type. WebKit's core has this (`WebURLSchemeHandler`, which
  GTK's `register_uri_scheme` stands on). Step W3 checks the C API
  reaches it.
- **Remote loads held**: WebKit's content rule lists, a rule that blocks
  every `http:` and `https:` load, taken off when Load Pictures is
  pressed. The option is compiled out of this build today
  (`ENABLE_CONTENT_EXTENSIONS 0`), and W4 turns it on. It is WebKit's own
  mechanism, the one Safari's content blockers use, rather than one of
  Kosmos's in the network process.

JetSki's tabs, its Web Inspector and its developer switch stay JetSki's,
as calls on its views. **No second copy of anything**: there is one WebKit
in Kosmos and one door to it, and both applications stand on that door.

### Region or message

The message's HTML goes from the Mail Kit's region to the kit as a string,
once, when it is opened: a one-shot payload, which a message may carry.
Pictures cross from the web process as WebKit's shared bitmaps, which the
kit copies into Mail's surface, as JetSki's page does. A `cid:` picture is
handed to WebKit once, when the page asks. No stream travels as messages.

### C or Lua, and why

The kit is C++ because WebKit is. Mail stays Lua: what it adds is which
message, which pictures and where a link goes, and none of that is a loop
over bytes. NetSurf's parts in Mail go: the HTML path (`web.parse`,
`ns_layout`, `webpictures.lua`) and the fixes NetSurf needed.

### The busiest paths

- **Opening a message**: the HTML handed to the view, then parsing, styling,
  layout and painting in the web process, then one copy of the picture
  into Mail's surface. JetSki shows this on Wikipedia, a far larger page
  than a newsletter, in under a second after its bytes arrive.
- **Scrolling**: WebKit scrolls in the web process and paints the newly
  shown strip, and the kit copies the picture, the same path as JetSki's
  wheel.
- **Opening Mail**: the image read once a boot, 2.3 s on the M700's stick,
  then kept by the window manager.

### How it is tested

`x86-mail` already opens HTML messages from a test server. The WebKit
version keeps its checks and adds:

- a message's colours and layout drawn as WebKit draws them;
- a `cid:` picture shown;
- a network picture **never requested** before Load Pictures, checked at
  the server, and requested after;
- a script in a message that would change the page, which does not;
- a link pressed, which opens JetSki at its address.

Each check has a control that fails it alone. `x86-jetski` must pass
unchanged once JetSki stands on views.

### What is new, and the order to build it in

1. **W1, the WebKit image.** `jetski.elf` becomes `/Kosmos/WebKit/webkit.elf`
   with Inspector beside it, a `system` package of its own. `-- kosmos:
   image webkit` names a system image. JetSki runs from it unchanged.
2. **W2, views.** The kit's door becomes views with options. JetSki's tabs
   become views, and `x86-jetski` passes unchanged.
3. **W3, a page from a string, without scripts, with `cid:`.**
4. **W4, remote loads held**: content rule lists turned on in the build,
   and the network rule.
5. **W5, Mail's pane on it**: the message pane a view, Load Pictures, links
   to JetSki, the status line, selection and copying.
6. **W6, NetSurf out of Mail.** Then step two of the browser change, the
   old browser removed, has nothing left standing on NetSurf.

## Yours to decide

1. **Where the image lives**: `/Kosmos/WebKit`, a folder of its own beside
   Apps, Kits and Libraries (recommended). Or JetSki's folder, with Mail
   naming JetSki's image, which makes Mail depend on an application.
2. **Where a link opens**: a new tab of the JetSki that is open, or a new
   JetSki when none is (recommended). Or always a new JetSki window.
