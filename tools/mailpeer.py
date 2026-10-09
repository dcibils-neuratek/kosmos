#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A mail server of Kosmos's own, on this Mac: IMAP and SMTP for the suites.

`docs/mail.md`, *How it is tested* - as `smbpeer.py` is Samba for the
sharing suites. Never a real mailbox: every message here is written for it,
every address is example.com, example.org or example.net.

IMAP4rev1 over TLS, with what `imap.lua` uses - CONDSTORE, MOVE, UIDPLUS,
SPECIAL-USE, IDLE - and SMTP with STARTTLS and AUTH PLAIN, which keeps what
it is sent for the suite to look at. Enough of each to hold a client to the
protocol, not a server for anybody to use.

As a module, `Peer(work, cert, key)` and `peer.start()`; the suite reads and
changes `peer.boxes` and `peer.sent` directly. As a program, it serves the
same mailbox until stopped:

    mailpeer.py WORK            (WORK holding server.pem and server.key)
"""

import base64
import os
import re
import socket
import ssl
import sys
import threading
import time

USER = "lena@example.com"
PASSWORD = "test-only-word"

def png(width, height, rgb):
    """A real PNG of one colour, for a picture a message carries."""
    import struct
    import zlib

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data
                + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff))

    raw = b"".join(b"\0" + bytes(rgb) * width for _ in range(height))
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
            + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


# A picture sent inside a message: the route's map, 40 by 20, green.
PICTURE = base64.b64encode(png(40, 20, (0x0f, 0x5c, 0x4d))).decode()

# An attachment large enough that its literal comes in many reads: 300 KB
# of bytes, base64 in lines of 76.
BIG = bytes((i * 7) & 0xFF for i in range(300 * 1024))


def lines76(data):
    text = base64.b64encode(data).decode()
    return "\r\n".join(text[i:i + 76] for i in range(0, len(text), 76))


SEED = [
    ("From: =?windows-1252?Q?Tom=E1s_Ferreira?= <tomas@example.org>\r\n"
     "To: Lena Moreau <lena@example.com>\r\n"
     "Subject: =?windows-1252?Q?Caf=E9_on_Saturday?=\r\n"
     "Date: Tue, 6 Oct 2026 09:41:07 +0200\r\n"
     "Message-ID: <one@example.org>\r\n"
     "Content-Type: text/plain; charset=utf-8\r\n"
     "\r\n"
     "Lena,\r\n\r\n> shall we?\r\nYes - at ten, by the mill.\r\n").encode(),
    ("From: Greenway Rides <rides@example.net>\r\n"
     "To: lena@example.com\r\n"
     "Subject: This week's route\r\n"
     "Date: Wed, 7 Oct 2026 18:00:00 +0000\r\n"
     "Message-ID: <two@example.net>\r\n"
     "MIME-Version: 1.0\r\n"
     "Content-Type: multipart/related; boundary=rel\r\n"
     "\r\n"
     "--rel\r\n"
     "Content-Type: text/html; charset=utf-8\r\n"
     "\r\n"
     "<p>Forty kilometres. <img src=\"cid:map@example.net\"></p>\r\n"
     "--rel\r\n"
     "Content-Type: image/png\r\n"
     "Content-ID: <map@example.net>\r\n"
     "Content-Transfer-Encoding: base64\r\n"
     "\r\n" + PICTURE + "\r\n"
     "--rel--\r\n").encode(),
    ("From: Ana Ruiz <ana@example.com>\r\n"
     "To: lena@example.com\r\n"
     "Subject: The photos\r\n"
     "Date: Thu, 8 Oct 2026 08:15:00 +0100\r\n"
     "Message-ID: <three@example.com>\r\n"
     "MIME-Version: 1.0\r\n"
     "Content-Type: multipart/mixed; boundary=mix\r\n"
     "\r\n"
     "--mix\r\n"
     "Content-Type: text/plain\r\n"
     "\r\n"
     "Here they are.\r\n"
     "--mix\r\n"
     "Content-Type: application/octet-stream; name=\"photos.bin\"\r\n"
     "Content-Disposition: attachment; filename=\"photos.bin\"\r\n"
     "Content-Transfer-Encoding: base64\r\n"
     "\r\n" + lines76(BIG) + "\r\n"
     "--mix--\r\n").encode(),
]

# What IDLE brings: delivered while a client is idling.
ARRIVING = ("From: Bob <bob@example.net>\r\n"
            "To: lena@example.com\r\n"
            "Subject: While you were idling\r\n"
            "Date: Thu, 8 Oct 2026 12:00:00 +0000\r\n"
            "\r\n"
            "Hello.\r\n").encode()

# Mailboxes, their SPECIAL-USE, and a name outside ASCII (`Café`, in
# IMAP's modified UTF-7).
BOXES = [("INBOX", ""), ("Sent", "\\Sent"), ("Drafts", "\\Drafts"),
         ("Trash", "\\Trash"), ("Archive", "\\Archive"), ("Caf&AOk-", ""),
         # A folder inside a folder, and Gmail's own parent, which holds
         # mailboxes and cannot be opened.
         ("Projects", ""), ("Projects/Kosmos", ""), ("[Gmail]", "\\Noselect")]


class Box:
    def __init__(self, name, use, validity):
        self.name, self.use, self.validity = name, use, validity
        self.messages = []          # [{uid, flags, data, modseq}]
        self.next_uid = 1


class Peer:
    # `user` and `password`: whom this server takes, Lena by default - a
    # second account is a second Peer with another (Mail M8).
    def __init__(self, work, cert="server.pem", key="server.key", user=USER, password=PASSWORD):
        self.work = work
        self.user, self.password = user, password
        self.lock = threading.RLock()
        self.modseq = 1
        self.boxes = {}
        self.sent = []              # [{from, to, data}] what SMTP was given
        self.logins = []            # [(user, ok)]
        self.idle_delivers = True
        self.listeners = []

        for i, (name, use) in enumerate(BOXES):
            self.boxes[name] = Box(name, use, 1000 + i)

        for data in SEED:
            self.deliver("INBOX", data)

        self.context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        self.context.load_cert_chain(os.path.join(work, cert), os.path.join(work, key))

    # -- the mailbox ----------------------------------------------------

    def deliver(self, box, data, flags=()):
        with self.lock:
            b = self.boxes[box]
            self.modseq += 1
            m = {"uid": b.next_uid, "flags": set(flags), "data": data, "modseq": self.modseq}
            b.next_uid += 1
            b.messages.append(m)
            return m

    def find(self, box, subject):
        with self.lock:
            for m in self.boxes[box].messages:
                if ("Subject: " + subject).encode() in m["data"]:
                    return m
        return None

    # -- serving --------------------------------------------------------

    def listen(self, port, handler):
        s = socket.socket()
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("127.0.0.1", port))
        s.listen(8)
        self.listeners.append(s)

        def loop():
            while True:
                try:
                    conn, _ = s.accept()
                except OSError:
                    return
                threading.Thread(target=handler, args=(conn,), daemon=True).start()

        threading.Thread(target=loop, daemon=True).start()
        return s.getsockname()[1]

    def start(self, imap_port=0, smtp_port=0):
        self.imap_port = self.listen(imap_port, self.imap)
        self.smtp_port = self.listen(smtp_port, self.smtp)
        return self.imap_port, self.smtp_port

    def stop(self):
        for s in self.listeners:
            s.close()

    # -- IMAP -----------------------------------------------------------

    def imap(self, raw):
        try:
            conn = self.context.wrap_socket(raw, server_side=True)
        except (ssl.SSLError, OSError):
            raw.close()
            return
        Session(self, conn).run()

    # -- SMTP -----------------------------------------------------------

    def smtp(self, conn):
        f = Lines(conn)
        user = None
        mail_from, rcpts = None, []

        def say(text):
            f.write((text + "\r\n").encode())

        say("220 mailpeer.example.com ESMTP")

        try:
            while True:
                line = f.line()
                if line is None:
                    return
                word = line.split(" ", 1)[0].upper()

                if word in ("EHLO", "HELO"):
                    extras = ["AUTH PLAIN"] if f.secure else ["STARTTLS"]
                    for e in ["mailpeer.example.com", "SIZE 35882577"] + extras[:-1]:
                        say("250-" + e)
                    say("250 " + extras[-1])
                elif word == "STARTTLS" and not f.secure:
                    say("220 go ahead")
                    f.start_tls(self.context)
                elif word == "AUTH":
                    parts = line.split()
                    if not f.secure:
                        say("530 STARTTLS first")
                        continue
                    blob = base64.b64decode(parts[2]) if len(parts) > 2 else b""
                    fields = blob.split(b"\0")
                    ok = len(fields) == 3 and fields[1].decode() == self.user \
                        and fields[2].decode() == self.password
                    self.logins.append((fields[1].decode() if len(fields) > 1 else "", ok))
                    if ok:
                        user = fields[1].decode()
                        say("235 2.7.0 accepted")
                    else:
                        say("535 5.7.8 those are not the right name and password")
                elif word == "MAIL":
                    if not user:
                        say("530 5.7.0 sign in first")
                        continue
                    mail_from, rcpts = re.search(r"<(.*?)>", line).group(1), []
                    say("250 2.1.0 ok")
                elif word == "RCPT":
                    to = re.search(r"<(.*?)>", line).group(1)
                    if to.endswith("@refused.example.com"):
                        say("550 5.1.1 no such person")
                        continue
                    rcpts.append(to)
                    say("250 2.1.5 ok")
                elif word == "DATA":
                    say("354 go on, end with a dot")
                    body = []
                    while True:
                        l = f.line()
                        if l is None:
                            return
                        if l == ".":
                            break
                        body.append(l[1:] if l.startswith("..") else l)
                    with self.lock:
                        self.sent.append({"from": mail_from, "to": rcpts,
                                          "data": "\r\n".join(body) + "\r\n"})
                    say("250 2.0.0 queued as %d" % len(self.sent))
                elif word == "QUIT":
                    say("221 2.0.0 goodbye")
                    return
                else:
                    say("502 5.5.2 not understood")
        except (OSError, ssl.SSLError):
            pass
        finally:
            f.close()


class Lines:
    """A connection read a line at a time, and literals by size."""

    def __init__(self, conn):
        self.conn, self.buf, self.secure = conn, b"", isinstance(conn, ssl.SSLSocket)

    def fill(self):
        try:
            got = self.conn.recv(65536)
        except (OSError, ssl.SSLError):
            got = b""
        if not got:
            return False
        self.buf += got
        return True

    def line(self):
        while b"\r\n" not in self.buf:
            if not self.fill():
                return None
        line, self.buf = self.buf.split(b"\r\n", 1)
        return line.decode("utf-8", "replace")

    def bytes(self, n):
        while len(self.buf) < n:
            if not self.fill():
                return None
        out, self.buf = self.buf[:n], self.buf[n:]
        return out

    def write(self, data):
        self.conn.sendall(data)

    def start_tls(self, context):
        self.conn = context.wrap_socket(self.conn, server_side=True)
        self.secure = True

    def close(self):
        try:
            self.conn.close()
        except OSError:
            pass


def tokens(text):
    """IMAP arguments: atoms, quoted strings, parenthesised lists."""
    out, stack, i = [], [], 0
    cur = out
    while i < len(text):
        c = text[i]
        if c == " ":
            i += 1
        elif c == "(":
            stack.append(cur)
            new = []
            cur.append(new)
            cur = new
            i += 1
        elif c == ")":
            cur = stack.pop() if stack else out
            i += 1
        elif c == '"':
            j, s = i + 1, []
            while j < len(text) and text[j] != '"':
                if text[j] == "\\":
                    j += 1
                s.append(text[j])
                j += 1
            cur.append("".join(s))
            i = j + 1
        else:
            j, depth = i, 0
            while j < len(text):
                if text[j] == "[":
                    depth += 1
                elif text[j] == "]":
                    depth -= 1
                elif depth <= 0 and text[j] in " ()":
                    break
                j += 1
            cur.append(text[i:j])
            i = j
    return out


def uid_set(text, top):
    out = set()
    for part in text.split(","):
        if ":" in part:
            a, b = part.split(":")
            a = top if a == "*" else int(a)
            b = top if b == "*" else int(b)
            lo, hi = min(a, b), max(a, b)
            out.update(range(lo, hi + 1))
        else:
            out.add(top if part == "*" else int(part))
    return out


class Session:
    def __init__(self, peer, conn):
        self.peer, self.f = peer, Lines(conn)
        self.user, self.box, self.condstore = None, None, False

    def say(self, text):
        self.f.write((text + "\r\n").encode())

    def caps(self):
        return ("IMAP4rev1 IDLE UIDPLUS MOVE SPECIAL-USE CONDSTORE ENABLE"
                + ("" if self.user else " AUTH=PLAIN"))

    def read_command(self):
        """A command's line with its literals put in as quoted text."""
        line = self.f.line()
        if line is None:
            return None
        while True:
            m = re.search(r"\{(\d+)(\+?)\}$", line)
            if not m:
                return line
            if not m.group(2):
                self.say("+ go on")
            data = self.f.bytes(int(m.group(1)))
            if data is None:
                return None
            rest = self.f.line()
            if rest is None:
                return None
            self.literal = data
            line = line[:m.start()] + '"\0LIT\0"' + rest

    def run(self):
        self.say("* OK [CAPABILITY %s] mailpeer ready" % self.caps())
        try:
            while True:
                self.literal = None
                line = self.read_command()
                if line is None:
                    return
                tag, _, rest = line.partition(" ")
                cmd, _, args = rest.partition(" ")
                cmd = cmd.upper()
                if cmd == "UID":
                    sub, _, args = args.partition(" ")
                    cmd = "UID " + sub.upper()
                handler = getattr(self, "c_" + cmd.replace(" ", "_"), None)
                if handler is None:
                    self.say(tag + " BAD unknown command")
                    continue
                if cmd not in ("CAPABILITY", "LOGIN", "LOGOUT", "NOOP") and not self.user:
                    self.say(tag + " NO sign in first")
                    continue
                with self.peer.lock:
                    pass
                if handler(tag, args) is False:
                    return
        except (OSError, ssl.SSLError):
            pass
        finally:
            self.f.close()

    # -- commands -------------------------------------------------------

    def c_CAPABILITY(self, tag, args):
        self.say("* CAPABILITY " + self.caps())
        self.say(tag + " OK done")

    def c_NOOP(self, tag, args):
        self.say(tag + " OK done")

    def c_LOGOUT(self, tag, args):
        self.say("* BYE see you")
        self.say(tag + " OK done")
        return False

    def c_LOGIN(self, tag, args):
        t = tokens(args)
        user = t[0] if t else ""
        word = t[1] if len(t) > 1 else ""
        if word == "\0LIT\0":
            word = self.literal.decode("utf-8", "replace")
        ok = user == self.peer.user and word == self.peer.password
        self.peer.logins.append((user, ok))
        if not ok:
            self.say(tag + " NO [AUTHENTICATIONFAILED] those are not the right name and password")
            return
        self.user = user
        self.say(tag + " OK [CAPABILITY %s] signed in" % self.caps())

    def c_ENABLE(self, tag, args):
        self.condstore = True
        self.say("* ENABLED CONDSTORE")
        self.say(tag + " OK done")

    def c_LIST(self, tag, args):
        with self.peer.lock:
            for name, b in self.peer.boxes.items():
                attrs = "\\HasNoChildren" + (" " + b.use if b.use else "")
                if b.use == "\\Noselect":
                    attrs = "\\Noselect \\HasChildren"
                self.say('* LIST (%s) "/" "%s"' % (attrs, name))
        self.say(tag + " OK done")

    def c_STATUS(self, tag, args):
        t = tokens(args)
        with self.peer.lock:
            b = self.peer.boxes.get(t[0])
            if not b:
                self.say(tag + " NO no such mailbox")
                return
            unseen = sum(1 for m in b.messages if "\\Seen" not in m["flags"])
            self.say('* STATUS "%s" (MESSAGES %d UNSEEN %d UIDNEXT %d)'
                     % (b.name, len(b.messages), unseen, b.next_uid))
        self.say(tag + " OK done")

    def c_SELECT(self, tag, args):
        t = tokens(args)
        with self.peer.lock:
            b = self.peer.boxes.get(t[0])
            if not b:
                self.say(tag + " NO no such mailbox")
                return
            if len(t) > 1 and isinstance(t[1], list) and "CONDSTORE" in [x.upper() for x in t[1]]:
                self.condstore = True
            self.box = b
            self.say("* FLAGS (\\Seen \\Flagged \\Answered \\Deleted \\Draft)")
            self.say("* %d EXISTS" % len(b.messages))
            self.say("* OK [UIDVALIDITY %d] valid" % b.validity)
            self.say("* OK [UIDNEXT %d] next" % b.next_uid)
            self.say("* OK [HIGHESTMODSEQ %d] modseq" % self.peer.modseq)
        self.say(tag + " OK [READ-WRITE] selected")

    def fetch_line(self, seq, m, items, body):
        parts = ["UID %d" % m["uid"], "FLAGS (%s)" % " ".join(sorted(m["flags"]))]
        if self.condstore:
            parts.append("MODSEQ (%d)" % m["modseq"])
        if "RFC822.SIZE" in items:
            parts.append("RFC822.SIZE %d" % len(m["data"]))
        head = "* %d FETCH (%s" % (seq, " ".join(parts))
        if body:
            self.f.write((head + " BODY[] {%d}\r\n" % len(m["data"])).encode()
                         + m["data"] + b")\r\n")
        else:
            self.say(head + ")")

    def c_UID_FETCH(self, tag, args):
        t = tokens(args)
        items = " ".join(x if isinstance(x, str) else "" for x in (t[1] if isinstance(t[1], list) else [t[1]])).upper()
        since = None
        if len(t) > 2 and isinstance(t[2], list) and t[2][0].upper() == "CHANGEDSINCE":
            since = int(t[2][1])
            self.condstore = True
        with self.peer.lock:
            msgs = self.box.messages
            top = msgs[-1]["uid"] if msgs else 0
            want = uid_set(t[0], top)
            # `n:*` names the last message even when its UID is below n.
            if "*" in t[0] and msgs:
                want.add(top)
            for seq, m in enumerate(msgs, 1):
                if m["uid"] in want and (since is None or m["modseq"] > since):
                    self.fetch_line(seq, m, items, "BODY.PEEK[]" in items or "BODY[]" in items)
                    if "BODY[]" in items and "BODY.PEEK" not in items:
                        m["flags"].add("\\Seen")
        self.say(tag + " OK done")

    def c_FETCH(self, tag, args):
        """By place in the mailbox, as a first look asks for the newest."""
        t = tokens(args)
        items = " ".join(x if isinstance(x, str) else "" for x in (t[1] if isinstance(t[1], list) else [t[1]])).upper()
        with self.peer.lock:
            msgs = self.box.messages
            want = uid_set(t[0], len(msgs))
            for seq, m in enumerate(msgs, 1):
                if seq in want:
                    self.fetch_line(seq, m, items, "BODY.PEEK[]" in items)
        self.say(tag + " OK done")

    def c_UID_SEARCH(self, tag, args):
        t = tokens(args)
        with self.peer.lock:
            msgs = self.box.messages
            top = msgs[-1]["uid"] if msgs else 0
            want = uid_set(t[1], top) if len(t) > 1 and t[0].upper() == "UID" else None
            found = [str(m["uid"]) for m in msgs if want is None or m["uid"] in want]
        self.say("* SEARCH" + ("".join(" " + u for u in found)))
        self.say(tag + " OK done")

    def c_UID_STORE(self, tag, args):
        t = tokens(args)
        how = t[1].upper()
        flags = set(t[2])
        with self.peer.lock:
            msgs = self.box.messages
            want = uid_set(t[0], msgs[-1]["uid"] if msgs else 0)
            for seq, m in enumerate(msgs, 1):
                if m["uid"] not in want:
                    continue
                before = set(m["flags"])
                if how.startswith("+"):
                    m["flags"] |= flags
                elif how.startswith("-"):
                    m["flags"] -= flags
                else:
                    m["flags"] = set(flags)
                if m["flags"] != before:
                    self.peer.modseq += 1
                    m["modseq"] = self.peer.modseq
                if ".SILENT" not in how:
                    self.fetch_line(seq, m, "", False)
        self.say(tag + " OK done")

    def take(self, uids_text, to_name, remove):
        with self.peer.lock:
            dest = self.peer.boxes.get(to_name)
            if not dest:
                return None
            msgs = self.box.messages
            want = uid_set(uids_text, msgs[-1]["uid"] if msgs else 0)
            src, dst, gone = [], [], []
            for seq, m in enumerate(msgs, 1):
                if m["uid"] in want:
                    n = self.peer.deliver(dest.name, m["data"], m["flags"])
                    src.append(str(m["uid"]))
                    dst.append(str(n["uid"]))
                    gone.append(seq)
            if remove:
                self.box.messages = [m for m in msgs if m["uid"] not in want]
            return dest, src, dst, gone

    def c_UID_MOVE(self, tag, args):
        t = tokens(args)
        got = self.take(t[0], t[1], True)
        if not got:
            self.say(tag + " NO [TRYCREATE] no such mailbox")
            return
        dest, src, dst, gone = got
        self.say("* OK [COPYUID %d %s %s] moved" % (dest.validity, ",".join(src), ",".join(dst)))
        for seq in sorted(gone, reverse=True):
            self.say("* %d EXPUNGE" % seq)
        self.say(tag + " OK done")

    def c_UID_COPY(self, tag, args):
        t = tokens(args)
        got = self.take(t[0], t[1], False)
        if not got:
            self.say(tag + " NO [TRYCREATE] no such mailbox")
            return
        dest, src, dst, _ = got
        self.say(tag + " OK [COPYUID %d %s %s] copied" % (dest.validity, ",".join(src), ",".join(dst)))

    def c_UID_EXPUNGE(self, tag, args):
        t = tokens(args)
        with self.peer.lock:
            msgs = self.box.messages
            want = uid_set(t[0], msgs[-1]["uid"] if msgs else 0)
            gone = [seq for seq, m in enumerate(msgs, 1)
                    if m["uid"] in want and "\\Deleted" in m["flags"]]
            self.box.messages = [m for seq, m in enumerate(msgs, 1) if seq not in gone]
        for seq in sorted(gone, reverse=True):
            self.say("* %d EXPUNGE" % seq)
        self.say(tag + " OK done")

    def c_APPEND(self, tag, args):
        t = tokens(args)
        name = t[0]
        flags = t[1] if len(t) > 2 and isinstance(t[1], list) else []
        with self.peer.lock:
            b = self.peer.boxes.get(name)
            if not b or self.literal is None:
                self.say(tag + " NO [TRYCREATE] no such mailbox")
                return
            m = self.peer.deliver(name, self.literal, flags)
        self.say(tag + " OK [APPENDUID %d %d] appended" % (b.validity, m["uid"]))

    def c_IDLE(self, tag, args):
        self.say("+ idling")
        if self.peer.idle_delivers and self.box is not None and self.box.name == "INBOX":
            self.peer.idle_delivers = False
            time.sleep(0.3)
            self.peer.deliver("INBOX", ARRIVING)
            self.say("* %d EXISTS" % len(self.box.messages))
        line = self.f.line()
        if line is None:
            return False
        self.say(tag + " OK IDLE done" if line.upper() == "DONE" else tag + " BAD expected DONE")


def main():
    work = sys.argv[1] if len(sys.argv) > 1 else "."
    peer = Peer(work)
    imap_port, smtp_port = peer.start(int(os.environ.get("IMAP_PORT", "9930")),
                                      int(os.environ.get("SMTP_PORT", "5870")))
    print("mailpeer: IMAP over TLS on %d, SMTP on %d, %s" % (imap_port, smtp_port, USER))
    try:
        while True:
            time.sleep(3600)
    except KeyboardInterrupt:
        peer.stop()


if __name__ == "__main__":
    main()
