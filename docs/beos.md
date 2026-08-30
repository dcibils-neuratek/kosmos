# BeOS lineage

What Kosmos takes from BeOS, what it corrects, and where it departs on purpose.

Source: *The BeOS Bible* (Scot Hacker, Peachpit Press, 1999). References by chapter.

---

## 17.1 What Kosmos inherits

### Client/server architecture (ch. 1)

BeOS split system functions into servers by category: app_server, media server, storage, net server, Tracker. Clients ask for the service instead of each solving it on their own.

The argument the book makes is one of economy: clients do not spend cycles or memory doing redundant work. The canonical example is a text editor that does not know how to draw windows; it asks the app_server for them.

Kosmos takes this further. In BeOS the servers shared a monolithic kernel with the drivers and the filesystem inside it. In Kosmos they are processes with separate address spaces talking over kernel IPC.

### Pervasive multithreading (ch. 1)

The book's metaphor: most systems push big rocks through the processor bottleneck, and BeOS pushes sand. Nobody computes faster, but everything responds better because everything gets constant attention in small pieces.

Concretely: in BeOS you could not write a windowed app using fewer than two threads even if you wanted to. Creating a window spawned two: one to talk to the app_server and draw the frame, another for what happens inside. Threads were grouped into "teams" and communicated by messages.

Kosmos keeps the property (a busy window does not freeze the others) and changes the mechanism: coroutines instead of system threads. See `ui.md` §16.5.

### Database-like filesystem (ch. 1, Giampaolo interview)

History that matters: **BeOS used an actual relational database as its filesystem through DR8, and they pulled it.** Maintaining it was too complex and it cost too much performance. At DR9 they rewrote the filesystem from scratch and replaced it with a filesystem *shaped like* a database.

They lost very little functionality and gained a lot of speed. It is the most useful warning in the book for M8.

How it ended up: a traditional directory hierarchy, with any number of attributes hanging off each file. Queries do not walk file by file; they consult an indexed attribute pool.

### BMessages (ch. 1)

Discrete packets of information passed from one app to another. The receiving app decides what to do with the message.

The book's example is a good one: drag a color from a color picker onto the desktop and the desktop changes color; drag the same color into a text editor and the editor inserts the hex value. Neither app was programmed to talk to the color picker. Each knows how to receive color information and decides what to do with it.

In Kosmos these are Lua tables with a `type` field, and they are the same thing that travels between servers.

### Direct Graphics Access / BDirectWindow (ch. 1)

BeOS gave apps direct access to video hardware, bypassing the system, for the case where compositor overhead was unacceptable (30fps video).

It is the direct ancestor of Kosmos **surfaces**: the shared-memory exception to the drawing-command model, used by Paint, the 3D demo and Doom.

A design detail worth copying: in BeOS it was opt-in and the programmer had to do extra work to use it. It was not the default path. Same in Kosmos.

---

## 17.2 Ideas from the book that enter the design

Three things that came out of rereading the book and were not in the original design.

### A scripting architecture, not a scripting solution (ch. 1)

**The most valuable idea in the book for Kosmos, and the one that was missing.**

Be decided not to provide a scripting language (à la AppleScript or REXX). It provided an architecture: named "hooks" on every aspect of the system, with apps able to define their own. A hook represents an aspect of the program: the names of a menu's entries, a spreadsheet's selected cell. Scripts send and receive BMessages against those hooks.

The consequence: **every BeOS app had basic scripting without having been written with scripting in mind.** It inherited it from the Kits. And the scripting language and the app needed to know nothing about each other.

In Kosmos this turns out better, because the mechanism already exists. **An app exposes its hooks as nodes in its own namespace:**

```
/app/paint/tool         -> "brush"
/app/paint/color        -> 0x000000
/app/paint/width        -> 3
/app/paint/actions/save
```

Another process with the matching capability does `fs.write("/app/paint/color", 0xff0000)` and the brush changes. It is the same protocol as everything else: no scripting API, no hooks as a separate concept, no new language.

And since Kosmos already has the REPL, this makes any app manipulable from the command line without its author having done anything.

It goes in the UI kit: `ui.window` publishes a default namespace with the window's properties, and the app adds its own declaratively. **M7.**

### The three built-in attributes (Giampaolo interview)

BFS indexed three attributes of every file, always: **name, size and modification date.**

That is why a query by name was extremely fast regardless of how many files were on the disk. It is a targeted lookup against an index, not a walk.

Decision for M8: those three are always indexed, without anyone asking. Other attributes get indexed when they are declared.

### Entity files (Giampaolo interview)

BeOS "People" files were files with **no content**. Just a name and attributes: email, phone, web address. A named entity, nothing more.

What is interesting is what that enabled: drag a People file into a mail client and it understands you want to send that person mail; drag it into a fax program and it goes out over the fax number. Functionality that on other systems lives duplicated inside every program.

It fits Kosmos perfectly, because a namespace node already returns a table. An entity file is a node whose `read` returns only attributes and whose content is empty. **M7.**

---

## 17.3 Where Kosmos departs from BeOS on purpose

Worth writing down, because the book is explicit on these points and it would be dishonest to present Kosmos as "BeOS done right".

### BeOS was POSIX-compatible. Kosmos is not.

Chapter 1 is clear: BeOS shipped with hundreds of ported Unix command-line tools, and porting more was a matter of recompiling. The system implemented POSIX fairly thoroughly, though it never passed full certification.

**Kosmos rejects that deliberately.** See `design.md` §17. BeOS could afford it because its resource model was the same old global tree; Kosmos has per-process namespaces and capabilities, and a POSIX personality would destroy both.

The cost is real: BeOS had an ecosystem of ported software and Kosmos will have none. That is accepted in exchange for the result of the experiment.

### BeOS was monolithic

Drivers, BFS and eventually networking lived inside the kernel. The userland servers were app_server, media, registrar and net_server, and none of them could be restarted: killing the app_server took the desktop with it.

Kosmos is a real microkernel. Hot reload and supervised restart are properties BeOS did not have.

### Threads with shared memory

BeOS pushed threads and shared memory all the way into userland, and paid for it with the locking API: `Lock()` and `Unlock()` on every window, and an entire class of bugs. Kosmos uses coroutines and share-nothing, and that class of bug does not exist.

### The desktop did not start from the command line

The book mentions this as a difference from Unix: BeOS could not boot into console mode without a GUI, and several preferences were only configurable through a graphical panel, not through a file.

In Kosmos the opposite holds: the system is fully usable from the REPL before the app server exists, and anything configurable through a graphical interface can be done from the shell, because both speak the same protocol.

---

## 17.4 What is not copied

**The Translation Kit.** BeOS's abstraction over image and sound formats: a central library of translators, where downloading a new translator extended the capabilities of many apps at once. Good idea, out of scope.

**The C++ class hierarchy.** See `ui.md` §16.9.

**The media focus.** BeOS was the "MediaOS": the whole design was oriented toward real-time video and audio. Kosmos does not have that goal, and the decisions BeOS made for that reason (direct DMA to video, fine-grained scheduling priorities for audio) enter only where they earn their place for another reason.
