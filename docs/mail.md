<!-- Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. -->
# Kosmos Mail, before it is built

Written on 7 October 2026, before any of it is built, under the rule of 5
October (`CLAUDE.md`, *And an app is designed before it is written*): its
feature set and its architecture here, its windows in `docs/mail.html`, and
what it stands on drawn in `docs/mail-architecture.png`.

Diego, 6 October, with a picture of Apple's Mail: "i want a native mail
client", "We can reuse a lot of what we did for kosmos write for the reading
and writing email component", "the client should be able to connect to imap,
pop3 and google accounts (google first) as its my personal email", "Kosmos
Email should publish notifications upon receiving emails". The same day, on
reading a message: "kosmos email should use the browser native html
rendering as most emails is html", "If the email is NOT html we can show
regular text using the kosmos write engine". And on 7 October: "for the mail
app, lets build it in kosmos ide!", "so the app itself is an kosmos app
example!", "we put it as an example of a serious system shipped app that the
user can even make a copy and modify on his own" - and "i need a mail client
to work so its necessary".

**Built since, through M8 and POP3** (9-12 October), and in daily use on
the M700. **On 17 November Diego asked for more** ("i want to work on the
email and office productivity tools as they are too basic now"). What Mail
does today, what it lacks next to Apple's Mail and Thunderbird, and the
next version are in *The next Mail* at the end of this document, before
*Yours to decide*. What it shares with Write, Sheets and Present is in
`docs/office.md`. Its HTML moving to WebKit is `docs/mailweb.md`, the main
session's, and is designed around here and not changed.

Every message, name and address in this document and in the mockup is made
up. A test never reaches a real mailbox: it reaches a mail server of its own,
on this Mac, holding made-up mail (*How it is tested*).

---

## What it does

**Apple's Mail, on Kosmos.** Mailboxes down the side, the messages in the
middle, the one chosen on the right; a message read as its sender drew it;
an answer written and sent; and a banner when something arrives, whether or
not the window is open.

### In the first version

- **A Google account, set up in one window**: the address and an **app
  password** - the sixteen letters Google makes for an application, which
  needs two-step verification on the account - with a sentence and the place
  in Google's settings where it is made. Checked before it is kept: the
  window says "Signed in" or exactly what Google answered.
- **Any IMAP account** the same way, with its server, port and security
  shown and changeable - IMAP over TLS (993) and SMTP over TLS (465) or with
  STARTTLS (587).
- **Mailboxes**: Inbox, Drafts, Sent, Archive, Junk and Trash - found by what
  the server says each one is (RFC 6154's special use, which Gmail speaks),
  not by their names - and every other folder of the account, or every
  label of a Google one, beneath them. Unread counts beside each.
- **The list**: the sender, the subject, when, and the first line, newest at
  the top; unread marked with a dot, flagged with a flag, an attachment with
  a clip; a conversation's messages kept together under its newest.
- **A message**: who it is from and to, when, its subject, and its body -
  **HTML drawn by the browser's engine** (Diego's decision), scripts never
  run, and **pictures from the network not fetched until asked** ("Load
  pictures", once, or always for that sender); pictures sent inside the
  message shown at once. **Plain text set by Write's engine** (Diego's
  decision): wrapped to the pane, its links pressable, its quoted lines shown
  as quotes. A message with both is shown as HTML, its text a switch away.
- **Attachments** listed under the message, with their size; opened in the
  application that opens their kind (`filetypes.lua`), or saved where the
  Save window says.
- **Reply, Reply All, Forward**: the composer opened with the right people,
  the subject, and the message quoted.
- **Writing a message**: To, Cc, Bcc - each address completed from the ones
  in the mail kept - the subject, and the body; attachments added with the
  Open window or by dropping a file from Tracker; **saved as a draft** while
  it is written; sent, and kept in Sent.
- **Mark** read or unread, **flag**, **archive**, **delete** (to Trash),
  **move** to a mailbox - from the tools, the keyboard and the right button,
  on one message or on several chosen together.
- **Search** the mail kept, by who, subject and the words of a message, as
  it is typed, in one mailbox or all.
- **New mail arrives as it is sent**: IMAP's IDLE, the server saying when
  mail arrives and nothing polling, and **a notification for each**
  (`/Notifications`, Diego's request) with who and the subject, pressing it
  opening the message - with the Do Not Disturb and per-application settings
  the notifications already have. With the window closed too.
- **Mail kept under `/Home/Mail`**, one file a message, so what has arrived
  can be read with no network, Tracker can find it, and a backup of `/Home`
  is a backup of the mail.
- **Several accounts**, each its own group of mailboxes, and a unified Inbox
  above them.

### Later, each its own step

- **Writing with style**: bold, italic, lists, links, pictures in the
  body - Write's typing, once it is moved out of `writer.lua` (*Writing a
  message*), sent as HTML with a plain-text part. The first version writes
  plain text.
- **Google's sign-in with OAuth 2** (XOAUTH2), the way Google prefers: a
  client registered with Google, and its sign-in page opened in the
  browser, which Google will not let a page inside an application show. A
  design of its own.
- **POP3**, for an account that has nothing else - mail fetched and kept
  here, as Diego asked; after IMAP, because IMAP is what Google and almost
  every provider speak. **Built 9 October** (*POP3*, below).
- **Rules**: mail from someone, or about something, filed or flagged as it
  arrives.
- **Signatures**, one an account.
- **Searching the server** for mail not kept here (IMAP SEARCH; Gmail's own
  search words through `X-GM-RAW`).
- **Calendar's invitations**, when Calendar is built - an `.ics` in a
  message answered from the message.
- **Encrypted and signed mail** (S/MIME, OpenPGP).

### Not here, and why

- **Scripts in a message.** Never, not as a setting: a mail client runs no
  sender's code. The browser's engine has none to run anyway.
- **Pictures from the network by default.** A picture fetched when a
  message is opened tells its sender the message was read, when, and from
  where. A mail client owes its reader better.

---

## The architecture

### At a glance

Every piece, and what supplies it. *Exists* is used as it is; *grows* gains
a thing; *new* is new, and a kit wherever another application could want it
- the premise of 4 October, *kits supply, applications orchestrate*. The
question was asked of every row: **does another application want this?**
Anything that sends a file by mail, shows a `.eml` or reads a vCard does.

| Piece | Supplied by | |
|---|---|---|
| **A message read: its headers unfolded, encoded words decoded, its parts as a tree, each part's transfer encoding undone, its text in UTF-8** | **the Mail Kit**, `user/kits/mail/`, `use("/Kosmos/Kits/mail")` | **new, C** |
| **A message written: headers, encoded words, parts, base64 and quoted-printable** | the Mail Kit | **new, C** |
| Character sets: ISO-8859-x, Windows' code pages | **libparserutils**, the browser's, which decodes them for a page today - built into the Mail Kit | **moved**, from the browser's build |
| Base64 | the Compression Kit, `compress.base64` and `unbase64` - **with line breaks allowed**, which a message's base64 has every 76 characters | **grows** |
| **IMAP: a session - log in, mailboxes, fetch, flags, move, append, IDLE** | **`imap.lua`**, `/Kosmos/Libraries/imap.lua` | **new, Lua** |
| **SMTP: a message sent** | **`smtp.lua`** | **new, Lua** |
| TCP, never blocking | the Network Kit: `fs.connect`, `conn:read`, `conn:wait`, `fs.poll` | exists |
| TLS, and STARTTLS on a connection already open | the TLS Kit: `tls.client(conn, host)` takes any connection with `read` and `write` | exists |
| **The account's password** | **the keyring**, a **mail door** of its own (`keyproto.h`'s `KEY_KIND_MAIL`, there and unused) | **grows** |
| **Fetching in the background, IDLE, new-mail notifications, and sending** | **`maild`**, Mail's own program, started with the desktop | **new, Lua** |
| A notification for each message | `notify.lua`, `notify.post{ title, body, open }` - `open` the message's file, which Mail opens | exists |
| Where mail is kept | `/Home/Mail`, one `.eml` a message with its facts as **attributes** (`fs.setattr`), `fs.query` to find them | exists |
| An HTML message drawn | **the Web Kit**, `use("/Kosmos/Kits/web")`: `web.parse`, `ns_layout`, `ns_paint` into the window's surface; no scripts; it fetches nothing | exists |
| A plain-text message set | `richtext.lua`, `pageset.lua`, `pagedraw.lua` - **a page as tall as its text**, which a page engine has not needed | **grows** |
| Writing a message's body | `docview.lua` and `textbuf.lua`, Text Editor's - plain text, the first version | exists |
| Writing with style, later | `textedit.lua`, out of `writer.lua` - the move Present's P0 makes first | moved |
| The window: tools, panes, lists, fields, menus | `pixelkit.lua`, `ui.lua` (`direct = true`), Write's look | exists |
| The mailboxes' column | `pixelkit`'s sidebar as Write and Tracker draw it | exists |
| Addresses completed as they are typed | **`addresses.lua`**: every address in the mail kept, by how often | **new, Lua** - Calendar will want it |
| Opening and saving an attachment | `filetypes.lua`, `panel.lua` | exists |
| A dropped file from Tracker | `ui.drag`, the window manager's drop | exists |
| `.eml` as a type Mail opens | `filetypes.lua`, the header's `-- kosmos: opens eml` | grows by a word |
| Dates in headers: `Tue, 6 Oct 2026 09:41:07 +0200` | the Mail Kit - `httpcache.date` reads HTTP's and drops the zone, which mail cannot | new, C |

**No new server and no new system call.** One protocol grows: the keyring's
door for mail. Mail is a kit in C, two protocol libraries and an address
book in Lua, a program that runs in the background, and the window.

### The Mail Kit, in C

The question `CLAUDE.md` asks first - is it a loop over bytes? - is yes for
everything the Mail Kit does: undoing base64 and quoted-printable, turning a
Windows-1252 body into UTF-8, splitting a multipart message at its
boundaries, decoding `=?UTF-8?B?...?=` in a header. A newsletter with three
pictures is a megabyte; an attachment, ten. That is where Lua's strings are
slowest and where a mistake is silent, and it is exactly what the Compression
Kit and the Crypto Kit are in C for.

**Bytes in a region, never a Lua string between two C stages.** A message
is read from its file into a region (`regions.read_file`), the kit reads it
there and writes each part it is asked for into another region, and the
part goes to the Web Kit (an HTML part), to `pageset` (a text part, as a
string, because it is a page of words), or to a file (an attachment, from
its region with `fs.write_from`).

The door (`use("/Kosmos/Kits/mail")`):

```
mail.parse(at, bytes)            -> message: its header fields and its parts
message:header(name)             -> the field, unfolded, encoded words decoded,
                                    in UTF-8; nil when there is none
message:addresses(name)          -> { { name =, address = }, ... } of From, To, Cc
message:date()                   -> seconds since 1970, the zone applied
message:parts()                  -> { { id =, type =, charset =, name =,
                                    disposition =, cid =, bytes = }, ... }
message:part_into(id, at, cap)   -> bytes: the part, transfer encoding undone,
                                    a text part in UTF-8
message:preview(chars)           -> the first words of its text, for the list

mail.build{ from =, to =, cc =, bcc =, subject =, text =,
            html =, attachments = { { path =, type =, name = } },
            reply_to_id =, references = }
                                 -> the message, into a region, with its
                                    Message-ID and Date
mail.encode_words(text)          -> a header's words, encoded where they need it
```

A message is parsed once, when it arrives, and what the list shows -
sender, subject, date, the first line, whether it has attachments - is kept
as the file's attributes, so opening a mailbox reads no message at all.

**Its tests are on the Mac**, as `test_pack.c` and `test_zrle.c` are: a
`tools/test_mail.c` holding the kit to messages made for it - every
transfer encoding, nested multiparts, a Windows-1252 body, encoded words in
every header that has them, a header folded over three lines, a boundary
that appears in the text, a message cut short - each answer checked to the
byte.

### IMAP and SMTP, in Lua

A session is a conversation: a command, lines back, a tagged answer. It is
policy and state - what to ask next, what an answer means, what to do when
the connection drops - which is what Lua is for here, and what `http.lua`
already is for the web. The bytes that are large, a message's body, are not
parsed here: an IMAP literal is copied from the connection to a file as it
arrives and handed to the Mail Kit from there.

`imap.lua` speaks IMAP4rev1 (RFC 3501) with the extensions Gmail and every
large provider have: `IDLE` (2177), `UIDPLUS` (4315), `MOVE` (6851),
`SPECIAL-USE` (6154), `CONDSTORE` (7162) to ask only what changed since the
last look, and `X-GM-EXT-1` for a Google account's labels. **Nothing in it
waits**: every step reads what has arrived (`conn:read`) and returns, and
whoever runs it - `maild` - waits on all its connections at once with
`fs.poll`, as `http.get_many` does for the browser.

```
local s = imap.open{ host =, port = 993, user =, password = }
s:step()                  -- what has arrived, read and answered; call when the
                          -- connection has something, or a timeout passed
s:mailboxes()             -- -> { { name =, use = "inbox" | "sent" | ..., unread = } }
s:select(name)
s:changes(since)          -- -> new, changed and gone since the last look
s:fetch(uid, into_path)   -- the whole message, into a file
s:flag(uids, "seen" | "flagged", on)
s:move(uids, to) ; s:append(name, path, flags) ; s:idle() ; s:done()
```

`smtp.lua` sends one message: `EHLO`, `STARTTLS` when the port is 587,
`AUTH PLAIN`, `MAIL FROM`, `RCPT TO` for each recipient, `DATA` from the
message's file, `QUIT`. Gmail keeps a copy of what is sent by itself; any
other server is given one with `APPEND` to Sent.

### POP3

**Built 9 October, after M8**, for an account that has nothing else - mail
fetched and kept here, as Diego asked; `pop.gmail.com` on 995 among them.
`pop3.lua` is `imap.lua`'s shape: a session of requests answered as the
conversation goes, stepped by `maild`'s one loop, nothing waiting.

```
local s = pop3.open{ host =, port = 995, user =, password = }
s.ready                   -- signed in: CAPA, STLS on a plain port, USER, PASS
s:list()                  -- -> { { n =, id =, size = } }: UIDL and LIST together
s:fetch(n, into_path)     -- RETR, into a file
s:delete(n) ; s:quit()    -- DELE, taken at QUIT
```

A password is never sent in the clear: TLS from the first byte on 995, and
on a plain port STLS first or no password at all. POP3 says no size before a
message's bytes, so RETR's lines go into a region of the size LIST gave,
which doubles when a server counted short - one counting a line's end as a
byte, as the test's peer does on purpose - and the dot that stuffs a line is
undone as each piece lands.

**POP3 has one maildrop and no flags**, so a POP3 account is two folders
here: its **Inbox**, fetched, and a **Sent** of what was sent from it - the
server has nowhere to keep that. Read and flagged are the files'
attributes and never leave this machine. `maild` signs in, fetches what it
has not seen by unique id (`INBOX/state`: each id and the number it is kept
under), and signs out again, because a POP3 server locks the maildrop while
somebody is in it; it looks again in two minutes, or when the window asks.
**Mail stays on the server** until it is deleted here, which deletes it
there at the next look; a message the server stops having is still kept
here, because the copy here is the account's mail. A first look keeps the
newest hundred, as IMAP's does, and leaves the older ones on the server.

Add Account's third choice is POP3: the incoming server guessed from the
address as `pop.<domain>` on 995, the outgoing as for IMAP, and the
password kept by the keyring under `pop3://<host>:<port>`.

### Where mail is kept

```
/Home/Mail/
  diego@example.com/              an account
    account                       its server, port, security, name - no password
    INBOX/
      18431.eml                   a message, by its IMAP UID
      18432.eml
      state                       UIDVALIDITY, the highest UID, CONDSTORE's mark
    [Gmail]/Sent Mail/ ...
  Drafts here/                    what is being written, until it is sent
```

**One file a message, its facts as attributes** - `from`, `subject`,
`date`, `preview`, `seen`, `flagged`, `attachments`, `thread` - which is how
BeOS's own mail worked: a message was a file, Tracker showed its fields as
columns, and a query found mail as it found anything. Here `fs.query` does
the same, so a search of the mail is the system's search rather than Mail's.

**What opening a mailbox costs** is the one number that decides whether this
holds. An Inbox of 10,000 messages is 10,000 files' attributes. `diskfs`
answers a query by reading every file's attributes (`op_query`, not
indexed), and the window needs the newest 40 first. So each mailbox keeps
**one more file, `list`**: a line a message, newest first, rewritten by
`maild` when the mailbox changes - and the attributes stay the truth, `list`
being rebuilt from them whenever it is missing or older than the mailbox.
That is a cache with an owner, not a second copy: if the measure says
`fs.query` is fast enough, `list` goes (*Yours to decide*).

### `maild`: the half that runs without the window

Mail arrives when the window is closed, and Diego asked for a notification
when it does. So Mail is two programs, as BeOS's was (its `mail_daemon`):

- **`maild`**, started with the desktop and running until the machine
  stops: one IMAP connection an account, held open with `IDLE`; new mail
  fetched into `/Home/Mail` and a notification posted for each; anything the
  window asks - fetch this mailbox now, flag these, move those, send this
  draft - done and answered. When the network goes, it waits for it to come
  back and catches up with `CONDSTORE` rather than fetching everything.
- **`mail`**, the window: reads `/Home/Mail`, shows it, and asks `maild` for
  anything that changes the server. It never speaks IMAP itself, so there is
  one connection an account however many windows are open, and the window
  never waits on the network - it draws what is kept and is told when more
  arrives: `maild` posts "this mailbox changed" to the window's name in
  `/Running`, a post rather than a call, so `maild` never waits on a window
  either. (`fs.watch` would say the same, but it blocks until the answer
  changes, which a window must never do.)

The window talks to `maild` at **`/Running/maild`**, in tables, as every
application's own name in `/Running` takes them - `maild` is Mail's, not a
system server. Whether it should become one, that any application sends
mail through, is *Yours to decide*.

`maild` is Lua. The layer rule says C for whatever another process's
correctness or timing depends on, and the window depends on `maild` only to
change the server, which is never on a frame's clock: it answers after the
network has, which is hundreds of milliseconds, and a collector's pause is
one. The bytes it moves go from the connection to a file, and the parsing
is the Mail Kit's.

### The password, and the keyring's door for mail

Google takes an app password, sixteen letters; any other server, the
account's own. Either is a secret, kept by the keyring and nowhere else -
not in `account`, not in `maild`'s memory longer than a login.

The keyring has a kind for mail already (`KEY_KIND_MAIL`, `keyproto.h`) and
no door that can use it: `smb` is smbfs's, and `manage` - Passwords' - may
list, rename and forget, but not keep or read. So the keyring grows **a door
for mail**, as `docs/keyring.md` planned ("each a kind and a door of its
own, designed with its user"): PUT and GET of `KEY_KIND_MAIL` entries only,
named by account, lent to a program that declares `-- kosmos: needs keyring
mail` and is part of the system - in `/Kosmos/Apps` or `/Kosmos/Programs`,
as `manage` is. Passwords shows mail's entries beside the shares', and
forgets one when asked.

**What that means for a copy of Mail somebody makes in the IDE**, which is
in `/Home` and not part of the system: it is not lent the door, so it cannot
read the passwords the shipped Mail keeps. It asks for the account's
password when it starts and keeps it in its memory only. That is the
keyring doing its job - a program from `/Home` is exactly what it keeps the
passwords from - and the per-launcher permissions in the roadmap are where a
person would one day lend it.

### Reading a message

**HTML**, in the Web Kit, as the browser draws a page: the HTML part handed
to `web.parse` with its charset, `ns_layout` at the pane's width, and
`ns_paint` into the window's surface - Mail's window is a direct one, as
Write's and the browser's are. The kit fetches nothing by itself; it lists
the stylesheets and pictures the page names (`ns_sheets`, `ns_objects`), and
Mail answers each:

- **`cid:`** - a picture sent inside the message - from the Mail Kit's part,
  at once;
- **`http:` and `https:`** - a picture or a stylesheet on the network -
  nothing, until "Load pictures" is pressed; then through `http.lua`, as the
  browser fetches, and kept in the browser's cache. A bar above the message
  says pictures were not loaded, with the button.

Links open in the browser. A form in a message does nothing.

**Plain text**, by Write's engine, as Diego decided: the text made a
`richtext` document of paragraphs - quoted lines (`>`) as a quote style,
indented and ruled, links as runs that press - and set by `pageset` on **a
page as wide as the pane and as tall as the text**, which is the one thing
the engine gains: a page that does not break. `pagedraw` draws it into the
same surface.

### Writing a message

The composer is a window of its own: To, Cc and Bcc, each a field whose
addresses become chips as they are typed, completed from `addresses.lua`;
the subject; the body; attachments in a row beneath, added with the Open
window or by dropping a file.

**The body is plain text in the first version**, in `docview.lua` - Text
Editor's page, wrapped, with its caret and undo - and sent as `text/plain`
in UTF-8. **Writing with style waits for `textedit.lua`**: Write's caret,
typing and selection are inside `writer.lua` today, and the roadmap already
moves them out for Present (its step P0). Mail writing rich text before that
move would be a second copy of Write's typing, which is the defect the
premise names. When it is moved, the body becomes a `richtext` document and
is sent as HTML with a plain-text part beside it (`mail.build`'s `html` and
`text`).

A draft is saved as it is written - to `Drafts here`, and to the account's
Drafts by `maild` - and sent by `maild`, so a message being sent when the
window closes is still sent.

### Region or message

| What | How it travels | Why |
|---|---|---|
| A message arriving | the TCP ring (a region) → `maild` → its file | a stream from the network; never a message payload |
| A message read by the window | its file → a region → the Mail Kit → a part in a region → the Web Kit or `pageset` | bytes between C stages stay in regions |
| An attachment saved | its part in a region → `fs.write_from` | the same |
| "Fetch this", "flag these", "send that" | a message to `/Running/maild` | a request, a few hundred bytes, because a person asked |
| "Mail arrived" | a post from `maild` to the window, and a notification | an event; posted, so neither waits on the other |

### C or Lua, and why

- **C, the Mail Kit**: MIME's byte work - transfer encodings, character
  sets, boundaries, encoded words, dates. Held to its tests on the Mac.
- **Lua, everything else**: the two protocols, `maild`, the address book,
  the window. None is a loop over bytes or on a frame's clock; the largest,
  a message's bytes, pass through them only from a connection to a file.
- **And no C of Mail's own.** The IDE builds an application's own C into
  an image of its own (`-- kosmos: image`), and that image carries the lean
  runtime, which has no Web Kit - so a Mail with C of its own could not draw
  an HTML message. Mail needs none: its C is the Mail Kit's, the system's,
  which every application can use. So **Mail is a Lua project**, as the
  premise has it - an application orchestrating kits - and a copy made in
  the IDE runs as the shipped one does.

### The busiest paths

**Opening an Inbox of 10,000 messages.** The window reads the mailbox's
`list` - one file, a line a message, about a megabyte - draws the newest 40
and the unread counts, and is done; the rest of the list is read as it is
scrolled to. No message file is opened and nothing asks the network. The
budget is a frame: what appears is the list as it was kept, and anything
`maild` brings in after arrives as a change.

**A message arriving.** The server says `* 18433 EXISTS` on the IDLE
connection; `maild` leaves IDLE, fetches the message into its file, parses it
once with the Mail Kit, writes its attributes and the mailbox's `list`,
posts the notification, tells the window, and goes back to IDLE. The
window adds one line. Most of the time is the network's.

**Showing a newsletter.** Its file into a region; the Mail Kit's parse
(microseconds for the headers, the parts found by their boundaries); the HTML
part into a region in UTF-8; `web.parse`, `ns_layout` at the pane's width -
the time a browser page takes - and `ns_paint` of what is in view. Its
pictures are not fetched, so nothing waits on the network.

### How it is tested

- **The Mail Kit on the Mac** (`tools/test_mail.c`), above.
- **A mail server of its own**, `tools/mailpeer.py`, on this Mac, as
  `tools/smbpeer.py` is Samba for the sharing suites: IMAP with IDLE and
  SMTP, Python's, holding a made-up mailbox of messages written for it -
  HTML with pictures sent inside, plain text with quotes, an attachment, a
  Windows-1252 subject. The suite boots Kosmos, sets up an account against
  it, and checks: the mailboxes listed by their use; the Inbox's list; a
  message read; a message sent arriving at the server; a message delivered
  to the server mid-test arriving in the window and as a notification; a
  flag and a move reaching the server; the network cut and the catch-up
  after.
- **Never a real mailbox.** Diego's Gmail is Diego's to try, on the M700.

### What is new, and the order to build it in

Each step leaves the system working and a test that stays.

1. **M0 - the keyring's door for mail**: PUT and GET of mail's entries, the
   header's `needs keyring mail`, Passwords listing them.
2. **M1 - the Mail Kit**: reading - parse, header, addresses, date, parts,
   part_into, preview - and its tests on the Mac; base64 with line breaks in
   the Compression Kit; libparserutils' charsets moved into the kit's build.
3. **M2 - `imap.lua` and `smtp.lua`**, against `tools/mailpeer.py`.
4. **M3 - `maild`**: an account kept, its mailboxes and Inbox fetched, IDLE,
   a notification for each new message.
5. **M4 - the window**: mailboxes, the list, a plain-text message set by
   Write's engine (`pageset` gains the page as tall as its text); read,
   flag, archive, delete, move.
6. **M5 - HTML messages** in the Web Kit, pictures inside the message
   shown, the network's on asking.
7. **M6 - writing**: the composer, plain text, Reply, Reply All, Forward,
   drafts, sending; `mail.build`; `addresses.lua`. **Done 9 October**
   (`testing.md` 18.490): the composer is `mailcompose.lua`, a library, so
   whatever wants a message written opens the same one; it writes into
   `/Home/Mail/Outbox` and `maild` sends, and closes on Send. Drafts are
   kept in `Drafts here` as typed, and on the server when closed.
8. **M7 - attachments**, opened, saved, added, dropped. **Done 9 October**
   (`testing.md` 18.493): saved into Downloads, which became a place for it;
   opened in what opens their kind; added by the paperclip or a drop from
   Tracker; carried by Forward.
9. **M8 - search**, several accounts, the unified Inbox. **Done 9 October**
   (`testing.md` 18.496): All Inboxes and Flagged; search in what a message
   says, and in all mail; From chosen when there are several accounts.
10. **M9 - shipped as an example**: `/Kosmos/Apps` and `/Kosmos/Examples/Mail`,
    a copy built and run in the IDE.

**Add Account goes into M4** (Diego, 8 October: "we need a way to add
accounts", and "IMAP first, POP3 later"): Google with an app password, or
any IMAP server, drawn in `mail.html`; the window keeps the password in the
keyring and `maild` signs in with it before the account is kept.

Then, each its own step: POP3 (`pop3.lua`, downloading without folders or
flags - Gmail's pop.gmail.com 995 among them), writing with style (after
`textedit.lua`), OAuth, rules, signatures.

---

**Drawn on 8 October**, when its turn came after one chrome's step 3: the
windows in `docs/mail.html` (the main window, the composer, a banner, in
Night and a light look), and `docs/mail-architecture.png` from
`mail-architecture.html`. Read again against what was built since it was
written - TLS, the network client and HTTP in C for Maps (M6) - none of it
changes a choice here: Mail's protocols are Lua over the TLS Kit's door,
as `http.lua` is, and its C is the Mail Kit's.

## The next Mail - 17 November, before it is built

### What it does today, read from the code

Read from `user/bin/apps/mail.lua`, `user/bin/programs/maild.lua`,
`user/lib/mailcompose.lua` and the Mail Kit as they are on 17 November
(0.11.461), not from this document's plan:

- **Accounts**: Google with an app password, any IMAP server, POP3. Add
  Account checks the password before keeping it. An account can be
  removed, but there is no window for changing one.
- **`maild`**: one connection for each account, IDLE, new mail fetched
  into `/Home/Mail` with its facts as attributes, a notification for each,
  an Outbox sent, drafts kept on the server, and a trigger for Automations.
- **The window**: mailboxes by their use, with unread counts, then the
  account's folders; All Inboxes and Flagged; the list with its dot, flag
  and clip; a message in HTML by NetSurf, with pictures from the network
  held and senders whose pictures always load; plain text set by Write's
  engine; attachments opened, saved and forwarded; search in a mailbox's
  words or in all mail; a status bar; its controls published by name for
  Automations.
- **The tools**: Reply, Reply All, Forward, Get Mail, Flag, Mark as Read,
  Archive and Delete, each on the one message chosen. The keys are the
  arrows, Delete, Space and Page Up and Down, and Control-B for the
  sidebar.
- **The composer**: To, Cc and Bcc as chips completed from the mail kept;
  plain text in `docview`; files added by the paperclip or dropped;
  the draft kept within two seconds; Control-Return sends.

### What makes it too basic

Each of these was checked in the code, not assumed:

- **No conversations.** The first version was to keep a conversation's
  messages together (decided 8 October, question 6), and nothing does:
  neither the window nor `maild` reads `References` or keeps a thread.
- **Nothing can be moved to a mailbox.** `maild` has `move`, but the window
  has no way to ask for it: there is no Move To, no drag onto a mailbox,
  and no right-button menu on a message. Mail can be archived or deleted
  and that is all.
- **One message at a time.** There is no choosing several, so
  tidying an Inbox is one press per message.
- **Hardly any keys.** No Control-N for a new message (asked for in
  October, `roadmap.md`), no Control-R, no keys to flag or mark.
- **Writing is plain text only**, with no signature, no spelling, no
  sending later and no taking a message back.
- **No junk handling.** The Junk mailbox is listed, but nothing marks a
  message as junk or not junk.
- **No settings.** An account cannot be edited, its password cannot be
  changed (and the one known bug is there: a wrong password typed again
  replaces the right one before the server has said yes, `roadmap.md`),
  and there are no signatures, rules or reading preferences.
- **Search is only of what is kept here.** Mail on the server and not
  here cannot be found.
- **No contacts.** Names come from the mail kept, and there is nowhere to
  keep a person.
- **No printing or PDF of a message**, no Unsubscribe, and no Calendar
  invitations (Calendar does not exist yet).

### The next version, in the order a person needs it

New is marked **new**. *Supplied by* names the kit, so a second copy is
caught here rather than in the code.

**N1 - Mail to live in: tidy an Inbox quickly.** The window's work over
what `maild` already does, and the cheapest step with the most gain.

- **new - Conversations**: a conversation is listed once, under its newest
  message, with how many it holds. Opened, its messages are stacked oldest
  to newest, the earlier ones folded to a line each (who, when, the first
  words) and opened by a press. Replies from a person's own Sent are part
  of it. A switch in the dots menu shows messages one by one instead.
- **new - Move to a mailbox**, by Move To in the dots menu and the right
  button, with the mailboxes listed by account, by dragging the chosen
  messages onto a mailbox in the sidebar, or by Control-Shift-M and typing
  a mailbox's name. Copy with Alt held.
- **new - Several chosen**: Shift and Control with a press, Shift with the
  arrows, Control-A. Every tool and menu acts on all of them, and the
  reading pane says "4 messages chosen" with what can be done to them.
- **new - The right button on a message**: Reply, Reply All, Forward; Mark
  as Read or Unread; Flag; Move To; Archive; Junk or Not Junk; Delete; Save
  as PDF; Copy the sender's address.
- **new - Keys, Apple's with Control**: Control-N new, Control-R reply,
  Control-Shift-R reply all, Control-Shift-F forward, Control-Shift-U
  read or unread, Control-Shift-L flag, Control-Shift-J junk,
  Control-Shift-A archive, Delete, Control-F search, and Control-Alt-F
  search on the server. All of them are listed in Shortcuts
  (`docs/shortcuts.md`).
- **new - Junk and Not Junk**: moved to the account's Junk and back, with
  the `$Junk` and `$NotJunk` keywords set, which is how Gmail and most
  servers learn. A sender can be blocked, which files their mail straight
  into Junk (a rule, N4).

**N2 - Writing well.** Waits for `textedit.lua` (`office.md` O1), the move
`mail.md` said rich text would wait for on 7 October.

- **new - Styled text**: bold, italic, underline, strike-through, lists,
  a quote, a link, the size and the colour, from a row of tools above the
  body and from the keys Write uses. **The body is a `richtext` document
  typed through `textedit`**, set on a page as wide as the pane by
  `pageset`, as a plain message is read today. It is sent as HTML with a
  plain-text part beside it (`richhtml.lua`, `office.md`), and pictures
  dropped into the body travel inside the message as `cid:` parts. Plain
  text stays a choice, for an account and for one message.
- **new - Signatures**: one or more for each account, chosen in the
  composer, written in the same styled text, and kept in the account's
  folder.
- **new - Spelling as it is typed** (the Spelling Kit, `office.md`), in
  the message's language, which is guessed from the account's last choice
  and changed in the composer.
- **new - Undo Send**: for ten seconds after Send the status bar offers
  Undo, and the message comes back to the composer. **new - Send Later**,
  from Send's arrow: today at a time, tomorrow morning, or a chosen day.
- **Replying to an HTML message** quotes its text as a quote, as today
  (`office.md`, question 5).

**N3 - People.** The People Kit (`office.md`).

- **new - Add to Contacts** from a sender's name; a contact's name shown
  for their address; completion that prefers people who are kept; a group
  typed as one recipient; a `.vcf` in a message opened as a card, and a
  card sent as one.
- **new - The sender's picture** beside a message, from their card, and
  their initials on a colour when there is none, as now.

**N4 - Mail's settings, rules and the server's search.**

- **new - The Settings window** (the dots, and Control-comma): *Accounts*,
  each one's name, address, servers and ports, its password changed only
  once the server has taken the new one (which fixes the known bug), and
  removed; *Signatures*; *Composing*, styled or plain by default and how a
  reply quotes; *Reading*, when a message counts as read, conversations on
  or off, and the senders whose pictures load; *Rules*; and *Notifications*,
  a press away in Preferences, which already holds Mail's.
- **new - Rules**: "mail from *Northbank Library* is moved to *Receipts*",
  "mail to *rides@* is marked read and makes no banner". Conditions are
  on from, to, subject, a mailing list's `List-Id` and attachments, matched
  all or any; the actions are move, flag, mark read, junk, and no banner.
  They run in `maild` as mail arrives, **before the banner**, so a rule can
  keep a banner from appearing at all. That is why they are Mail's and not
  Automations', which the mail trigger runs after arrival (`mailarrived.lua`).
  Anything beyond filing a message is an automation.
- **new - Search on the server**: the list's search goes on to "Search on
  the server" when what is kept here runs out. `maild` asks with IMAP's
  `SEARCH`, or Gmail's own search words through `X-GM-RAW`; it fetches
  the headers of what it finds into a mailbox of results, and each message
  is fetched whole when it is opened.

**N5 - The rest, each its own step.**

- **new - Save as PDF** of a message. Plain text through `pageset` and
  `pdf.write`, which exist; HTML through WebKit's printing, once JetSki's
  Print as a PDF exists (`docs/webkit.html`, the main session's).
- **new - Unsubscribe**, a button above a message that carries
  `List-Unsubscribe`, with RFC 8058's one-click POST where the sender
  offers it, and otherwise a message to the address the header gives.
- **Calendar's invitations**, once the Calendar Kit exists.
- **Google's OAuth 2**, then **S/MIME and OpenPGP**, as planned in
  *Later, each its own step*.

**The HTML message moving to WebKit** (`mailweb.md` W3-W6) is the main
session's work and happens beside all of this. It changes only the reading
pane, and nothing above depends on it except N5's Save as PDF of an HTML
message.

### What is new under it

| Piece | Supplied by | |
|---|---|---|
| A conversation's key, from `Message-ID`, `In-Reply-To` and `References` (and Gmail's `X-GM-THRID` when the account is Google's) | **`mailthreads.lua`**, pure Lua, held on the Mac. Kept by `maild` as the `thread` attribute when a message arrives, and in the mailbox's `list` | **new, Lua** |
| `References` and `List-Id` read | the Mail Kit's `message:header`, which exists | exists |
| Move, copy, junk keywords | `maild`'s `move` exists; `copy` and the `$Junk` flags are added to `imap.lua` (`COPY`, `STORE +FLAGS`) | **grows** |
| Several chosen, the right button, dragging onto a mailbox | the window; `win:open_menu` and `pk.menu`, which exist; a drag inside the window as Tracker drags | **window** |
| The body typed | **`textedit.lua`** (`office.md` O1), in place of `docview` | **moved** |
| A message built as HTML and text, with pictures inside | the Mail Kit's `mail.build`, **growing `multipart/alternative` and `multipart/related`** (`mailbuild.c` writes neither today) | **grows, C** |
| HTML written from the body | **`richhtml.lua`** (`office.md` O5) | **new, Lua** |
| Spelling | **the Spelling Kit** (`office.md` O3) | **new, C** |
| Contacts | **the People Kit** (`office.md` O6), with `addresses.lua` as its other half | **new, Lua** |
| Undo Send, Send Later | the Outbox, which exists: an entry's `send_at` attribute, which `maild` waits for on its own clock | **grows** |
| Rules | **`mailrules.lua`**, pure Lua, held on the Mac, run by `maild` on arrival | **new, Lua** |
| Server search | `imap.lua` grows `s:search(words)`; `maild` grows a `search` request | **grows** |
| Signatures, settings | the account's folder (`signatures`, `settings`) as `tabletext`, read by the window and `maild` | **new**, files |

**Still no new server and no new system call.** `maild` stays Mail's own
(decided 8 October, question 2). What it gains is a few kinds of request
from the window: `move`, `copy`, `junk`, `search`, `sendat`, `unsend`.
They go to `/Running/maild` as the others do, because `maild` is a program
and not a system server.

### Conversations: the one new thing that needs explaining

**Where the key comes from.** A reply carries the `Message-ID`s it
answers in `References`, oldest first. So a conversation's key is the
first of them, or `In-Reply-To` when there are no references, or the
message's own `Message-ID` when it answers nothing. Gmail gives every
message its thread's number (`X-GM-THRID`), and for a Google account that
number is used, because Gmail's own idea of a conversation is the one its
owner has seen in the browser. A subject alone never joins two messages:
"Hello" and "Hello" from two people are not one conversation.

**Kept, not worked out each time.** `maild` computes the key once, when a
message arrives, and keeps it as the `thread` attribute and a field in the
mailbox's `list`. The window reads `list`, as it does now, and groups its
lines by `thread` as it reads them: one table keyed by thread over a
mailbox's lines. For 10,000 lines that is a few milliseconds of Lua,
measured at N1, and done when a mailbox is opened rather than on a frame.
A conversation shown across mailboxes, with the person's own replies from
Sent, is one `fs.query` on `thread` when it is opened.

**Mail already kept** has no `thread` attribute. `maild` works it out the
first time it opens a mailbox after the update, from each file's headers
through the Mail Kit, a few hundred files a pass between its other work,
so the window never waits for it. The list is shown ungrouped until it is
done.

### The busiest paths, after

- **Opening an Inbox of 10,000 messages**: the same as before (one `list`
  file, the newest forty drawn), plus grouping by `thread` as the lines
  are read. The budget is still a frame for what shows first.
- **A key that finishes a word in the composer**: `textedit`'s edit, one
  paragraph set again by `pageset`, one word looked up by the Spelling
  Kit. The first is measured in Write at O1 and the rest is microseconds.
- **Moving 200 chosen messages**: the window takes them out of its list in
  the frame the drop lands, as it does for one message now, and asks
  `maild` once with 200 UIDs, which `imap.lua` sends as one `UID MOVE` of
  a set. One request each way, whatever the count.

### How it is tested

As M1-M8 are, against `tools/mailpeer.py` and never a real mailbox.
`mailpeer.py` grows `COPY`, keywords and `SEARCH`. `mailthreads.lua` and
`mailrules.lua` are held on the Mac with made-up headers: a conversation
of five messages across Inbox and Sent, a reply whose `References` were
cut short, two messages with the same subject that must stay apart, and a
Gmail thread number that wins over the headers. Every check has a control
that fails it alone.

### The order

1. **N1** - conversations, move, several chosen, the right button, the
   keys, junk. The window and `maild` only, nothing in `office.md`.
2. **N2** - after `office.md` O1, O3 and O5: the styled composer,
   signatures, spelling, Undo Send and Send Later.
3. **N3** - after O6: contacts.
4. **N4** - the Settings window, rules, search on the server.
5. **N5** - Save as PDF, Unsubscribe, then invitations, OAuth, encryption.

The windows are drawn in `docs/mail.html`, *The next Mail*, and what it
stands on in `docs/mail-architecture.png`.

### Yours to decide, 17 November

**Decided on 17 November 2026**: every recommendation below, as
recommended - Diego, "yes to all, start with write" (`roadmap.md`, *Mail
and the office applications, less basic*).


1. **N1 first, before anything in `office.md`.** It is window work over
   what `maild` already does, it is where Mail feels most basic every day,
   and it needs no new kit. *Recommended: yes.* It edits `mail.lua`'s list,
   sidebar, menus and keys, while `mailweb.md` W5 edits its message pane,
   so the two are to be ordered by the main session: N1 before W5 or
   after it, never at the same time.
2. **Conversations by the headers**, with Gmail's thread number for a
   Google account, on by default with a switch to turn them off.
   *Recommended: yes.*
3. **The styled composer on `textedit` and `richtext`**, Write's engine,
   rather than WebKit's editing, which Apple's Mail uses and which would
   come with `mailweb.md`'s image. *Recommended: `textedit`.* It is the one
   rich-text editor of four applications, it works without the WebKit
   image, and what it costs is a reply that quotes an HTML message's text
   rather than its layout.
4. **Rules in `maild`, run before the banner**, kept here rather than on
   the server (Sieve). *Recommended: here*, because Gmail does not speak
   Sieve, and the rules then work the same for every account.
5. **Junk by the server's Junk and its keywords only**, with no filter of
   Mail's own. *Recommended: yes for now*. Gmail and the large providers
   already filter, and a local filter is a project of its own.
6. **Undo Send of ten seconds, on by default.** *Recommended: yes*, with
   the length in Settings.
7. **The Settings window as drawn**, a window of its own rather than a page
   in Preferences. *Recommended: Mail's own window*, with Notifications a
   press away in Preferences, where every application's are.

## Yours to decide

**Decided by Diego, 8 October: "as recommended, go ahead and build it"** -
two programs; `maild` Mail's own; plain text first; the keyring's door
for mail lent only to the system's Mail; the list file measured at M4;
All Inboxes and conversations in the first version; the three columns as
drawn. The steps below, M0 first.


1. **`maild` beside the window.** Mail as two programs - the window, and
   `maild` running from when the desktop starts, holding the connections,
   fetching and notifying with the window closed - or the window alone,
   fetching only while it is open and notifying only then. **Recommended:
   two**, since a notification when the window is closed is what you asked
   for.
2. **Whether `maild` is Mail's or the system's.** Mail's own, in its project,
   talking tables with the window - or a system server that any
   application sends mail through, which by the layer rule is C and speaks a
   declared shape. **Recommended: Mail's own**, and the server only when a
   second application needs to send mail.
3. **Plain text first.** The composer writes plain text until Write's typing
   is moved out of `writer.lua` (the move Present's P0 makes), then rich text
   - or that move made now, as Mail's first step. **Recommended: plain text
   first**; reading HTML matters more than writing it, and the move is
   already planned once.
4. **The keyring's door for mail**, lent only to the system's own Mail, so a
   copy made in the IDE asks for its password each time it starts.
   **Recommended: yes**, until per-launcher permissions let a person lend it.
5. **The mailbox's `list` file** beside the attributes, to open 10,000
   messages within a frame - or the attributes alone through `fs.query`, if
   measured fast enough. **Recommended: measure at M4 and decide then**,
   `list` written only if the query is too slow.
6. **The unified Inbox and conversations** in the first version, as drawn,
   or after it. **Recommended: in it** - Apple's Mail has trained everybody
   to expect both.
7. **The icon**: Haiku's mail icon at the pinned commit, if it has one, as
   the other fifty came. **Recommended: yes.**
