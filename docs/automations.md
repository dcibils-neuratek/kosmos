# Automations

Tasks strung together and run on a trigger - open an application, press one
of its buttons, send a mail, look at a web page, copy a file - by a person,
for themselves. Diego, 12 October 2026: "It's a new app that allows us to
automate certain tasks in Kosmos, like open an app, send an email, check a
website, copy a file. So let's design an app called Automations, which is
intended for a user like me to automate certain tasks in the OS. I'm basing
my premise that every app is scriptable, meaning we can press a button, we
can click an item, we can do whatever we want right now with the UI kit,
right? From a script."

This document is the feature set and the architecture; `automations.html`
is the window and `automations-architecture.png` the picture of what it
stands on. Written before any of it is built, for Diego to agree.

## Is every application scriptable? Half.

**What is true today.** Every window the UI kit makes publishes itself at
`/Running/<name>` and answers reads and writes there like files - `title`,
`x`, `y`, `width`, `height` - with nothing written by its author
(`ui.lua`, `window:publish`; `docs/beos.md`, "a scripting architecture, not
a scripting solution"). `cat /Running/gallery/title` works. The Deskbar and
Music publish a few properties of their own, and nothing else does.

**What is not.** A button cannot be pressed from a script, nor a menu item
chosen, nor a row in a list clicked, nor a field typed into. The suites and
the Mac's VNC tool do those by moving a pointer to a place on the screen -
which works for a test, and is no foundation for a person's automation: a
window moved, a different size, a list scrolled, and the click lands on
something else.

**So the first piece is the kit's, and it is the larger idea.** BeOS made
every application scriptable because every application was built from the
same kits, which named their parts - "the Save item of the File menu",
"the selected row" - and answered messages about them. Kosmos has the same
position: every window is a tree of the kit's views. If the kit publishes
**every control by name** - its label, where it sits in the tree, what it
can be asked to do - then every application is scriptable the day it is
written, and Automations is the first program that uses it, not the only
one: the IDE, a Terminal, `make test` and an AI agent driving Kosmos all
want the same door.

## What a person can do

- **See their automations** in a list: each one's name, what starts it, when
  it last ran and whether that went well, and a switch to turn it off.
- **Make one** from a trigger and a list of steps, chosen from menus - never
  typed as code - each step a card that says in a sentence what it does.
- **Triggers** - what starts it:
  - at a time, on chosen days, or every so many minutes or hours;
  - when Kosmos starts;
  - when a file appears in, or changes in, a folder;
  - when a drive or a stick appears, by its name;
  - when a mail arrives, from someone or with words in its subject;
  - by hand: a button in the window, an icon in the Deskbar's Automations
    menu, or a key in Shortcuts.
- **Steps** - what it does:
  - **open** an application, or a file with its application;
  - **press**, **choose**, **type into** or **read** a control in an open
    application - picked from that application's published controls, so
    "Mail - Send", never a place on the screen;
  - **send a mail**, through Mail's account and Outbox;
  - **check a web page**: fetch it, and go on only if it has - or has stopped
    having - some words, or has changed since last time;
  - **copy** or **move** files, to a folder, a drive or a share;
  - **notify**: a banner, with what the earlier steps found;
  - **wait** a while, or until an application is open;
  - **run** a Lua program or one of Kosmos's programs;
  - **stop unless** something is true - the only branch, to keep a list a
    list.
- **Pass what a step found to the next**: a page's title, a file's name, a
  mail's sender - named and offered in the next step's menus as `{page
  title}`, `{file}`.
- **Run it now** and watch each step light as it runs.
- **See its history**: every run, each step's outcome and how long it took,
  and the reason for any that failed, in words.
- **Keep them as files** in `/Home/Automations`, one each, which Tracker
  shows and a double-click runs, and which can be copied to another machine.
- **Write one as a Lua script** instead of cards - Diego, 12 October 2026,
  agreeing the design: "I would add is a way to script those automations
  via Lua scripts as well ... like we do with Cafesa 3D, which we have like
  a design, but also you can actually do that by script", "a script that
  automates things and a script that you can use to open programs, click
  buttons, open menus". The editor has **Steps** and **Script**; a script is
  run on a trigger like any other automation, with the same history.
- **Turn cards into a script** - "Copy as script" writes the Lua the cards
  were doing, so an automation can start as cards and grow into code.
- **Drive Kosmos from any Lua program**: the same verbs, `use`d by a program
  at the prompt, the IDE or a test, to open applications, press buttons and
  choose menu items.

## What it is made of

**The application is the thinnest layer there is** (`CLAUDE.md`): it shows
and edits automations. Running them belongs to a process that is there
whether the window is open or not, and every verb a step uses belongs to
something another program could use.

| Piece | Supplied by | New or as it is |
|---|---|---|
| The window, the list, the cards, the menus | the UI kit | as it is |
| **Every control published by name** - `press`, `choose`, `type`, `read` | **the UI kit** (`ui.lua`), at `/Running/<app>/controls/...`, a name from the control's label and its place in the window | **new, and the larger part**: every application scriptable without its author writing anything |
| **The verbs** - open, press, mail, fetch, copy, notify, wait, run | **a library**, `automate.lua`: each verb a function taking a table and returning what it found, used by the runner, the window's "Run now", the IDE and any program | **new**, mostly calls into what exists |
| Opening an application | the window manager's `launch`, as `open` asks it | as it is |
| Sending a mail | the Outbox and `maild` | **moved**: the Outbox writing comes out of Mail's compose window into a library both use (`mailout.lua`) - not copied |
| Checking a web page | `http.lua` | as it is |
| Copying and moving | `files.copy`, `files.move` | as it is |
| A banner | `notify.lua`, `/Notifications` | as it is |
| **A script's editor** | the IDE's editor (`ui.editor`), Lua's colours, errors by line | as it is |
| **A script run on its own** | `autod` starting a process with the script and `auto`, as the IDE's Run starts one | new, in `autod` |
| **Running them on their triggers** | **`autod`**, a background program the desktop starts, as it starts `maild`: reads `/Home/Automations`, waits on every trigger at once, runs each automation's steps, writes its history | **new** |
| A file appearing or a drive arriving | `autod` looking every few seconds at the folder's listing and at `/Drives` - a small cost, and nothing new in a server | new, in `autod` |
| A mail arriving | `maild`, telling `autod` - its `/Running/maild` name gains one request, "tell me what arrives" | **new**, small |
| At a time | the scheduler's clock, `sys.sleep` until the next | as it is |
| An automation's file | `/Home/Automations/<name>.automation`, a table in text - its trigger, its steps, nothing executable | **new**, and a file type Tracker knows |

**Asked, not polled where a server can tell**: a mail's arrival is told to
`autod`; a folder or a drive is looked at, because nothing in the file
system can say "this changed" today and adding that to every filesystem
server is a project of its own, written down rather than started here.

**C or Lua.** Lua, all of it. Nothing here is a loop over bytes or sits on a
frame's or a period's deadline: an automation runs a few steps a minute at
most. The one cost worth watching is `autod` looking at folders - a listing
every few seconds - and that is a number to measure on the M700, not a
reason for C.

## Scripts

A script is the same automation written as Lua, and it calls the same verbs
the cards do - the cards are a script whose lines are chosen from menus:

```lua
-- Good morning, as a script.
return automation {
  when = auto.at("8:00", { "mon", "tue", "wed", "thu", "fri" }),

  run = function()
    local mail = auto.open("Mail")
    mail:press("Get Mail")

    local page = auto.check("https://kosmos-os.org/builds", { has = "-stable" })
    if not page then return end              -- "stop unless", in Lua

    mail:menu("Message", "New Message")
    mail:type("To", "dcibils@gmail.com")
    mail:type("Subject", "A stable build: " .. page.title)
    mail:press("Send")

    auto.notify("Good morning - " .. page.title)
  end,
}
```

- **What a script is handed**: `auto` - its verbs and the applications it
  opens or finds (`auto.open`, `auto.app`), each with `press`, `menu`,
  `type`, `choose` and `read` on its published controls - and Lua's own
  `string`, `table` and `math`. Not `fs`, `sys` or `use`: as a Cafesa3D
  script is handed the scene and nothing else (`roadmap.md`, Cafesa3D
  scripting), what it was not handed it cannot reach. A step that copies a
  file goes through `auto.copy`, which is what lets the history say what a
  script did and the consent step mean something.
- **Each run is a process of its own**, started by `autod` with the script
  and `auto`: a script that loops for ever or faults ends itself and is
  written in the history with its line, and `autod` and every other
  automation go on.
- **Errors come back with their line**, in the history and under the editor,
  as the IDE shows them.
- **Copy as script**: every card is one line of a script, so the cards can
  always be written as one; a script cannot always be shown as cards - a
  loop has no card - so the editor shows a script as a script from then on.
- **The same verbs outside an automation**: `use("/Kosmos/Libraries/
  automate.lua")` in a program gives the same `auto`, with the program's
  own powers beside it - so a program at the prompt, the IDE's console or a
  suite can open Mail and press Send by name. A program pressing another
  application's controls asks as an automation does, the first time.

## Publishing every control

The kit names a control from what a person sees:

```
/Running/mail/controls/Send                 a button, by its label
/Running/mail/controls/menu/File/Save       a menu item, by its path
/Running/mail/controls/list/Inbox/selected  a list's selected row
/Running/mail/controls/field/To             a field, by its label
```

Each answers what it can be asked: `read` says what it shows; `write`
types into a field or chooses a row; `press` - a new request on the same
name - presses a button or chooses a menu item, **exactly as a click
would**: the same handler, the same instant feedback, so an automation can
do nothing a person could not. A control with no label is named by its kind
and place (`button 3 of the header`), and an application may give one a
name of its own, as it may give its window a title.

**Who may press them.** Today any process may write a window's title. A
control that sends mail is not a title, and *what you were not handed, you
cannot reach* is the rule this system keeps. So pressing and typing need
the capability to the application's `/Running` name, which `autod` holds
because the person gave it: **the first time an automation would press a
control in an application, Kosmos asks** - "Let Automations use Mail?" -
and remembers the answer, the consent step `roadmap.md` already wants for
launchers. Reading stays open, as it is today.

## The busiest path

**A trigger firing**: `autod` wakes - at a time, or from a listing that
changed, or told by `maild` - reads nothing from disk it has not already
read, and runs the steps in turn, each a call into `automate.lua`, writing
one line of history a step. A press is one message to the application,
answered when its handler has run. Nothing here is on anybody's frame.

**`autod` idle**: asleep until the nearest time, and looking at the folders
and drives it was asked to watch every five seconds - a listing each. Ten
automations watching folders is ten listings every five seconds, which is
nothing beside a Tracker window that is open.

## Steps

1. **A1 - every control published**: the kit names buttons, menu items,
   fields and lists and answers `read`, `write` and `press` at
   `/Running/<app>/controls`; a guest suite presses Calculator's buttons and
   reads its display, and chooses a Text Editor menu item, by name. The
   largest step, and useful the day it lands without Automations.
2. **A2 - the verbs**: `automate.lua`, and `mailout.lua` moved out of Mail;
   a program, `automate`, runs one step at the prompt.
3. **A3 - `autod`**: triggers, history, `/Home/Automations`; started by the
   desktop.
4. **A4 - the window**: as drawn - the list, the editor, the controls
   picker, the history.
5. **A5 - consent**: the first press in an application asks, and remembers.
6. **A6 - scripts**: the Script side of the editor, a script run as a
   process of its own on its trigger, Copy as script; and `automate.lua`
   documented as the way any program drives an application, with the IDE's
   tutorial gaining a lesson that opens Calculator and presses its buttons.

## For Diego to decide

- **The name of the background program** - `autod` here, by `maild`'s
  pattern.
- **Watching folders by looking every five seconds**, or a change
  notification in the filesystem servers first - the second is right and
  is a project of its own.
- **Recording**: an automation made by watching a person do it, as macOS's
  Automator had - every control a person presses becomes a step. A1 makes it
  possible; it is not in A1-A5.
- **Whether an automation may run while nobody is signed in** - today there
  is one person and the question does not arise.
