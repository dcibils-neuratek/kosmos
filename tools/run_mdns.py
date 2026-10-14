#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""What is on the network, by name: `mdnsd` (`docs/sharing.md` N11c).

The machine on a wire of its own, as `run_udp.py` puts it, with this file
as the other machine - here a file server called Studio, answering
multicast DNS (RFC 6762) as a Mac or a NAS does: for `_smb._tcp.local` a
PTR to `Studio._smb._tcp.local`, its SRV (port 445, host `studio.local`),
an empty TXT and the host's A, names compressed against each other; and for
`studio.local` its A. Three boots, each running one command at the prompt:

  - `mdns browse _smb._tcp`: the question goes to the group, at a time to
    live of 255, and the program lists Studio with its host, port and
    address;
  - `mdns resolve studio.local`: its address;
  - `mdns resolve stranger.local`, after the peer has sent an A for that
    name nobody asked about, and will not answer for it: nobody answered -
    what nobody here asked for was not kept.

And the other way (N13), as a Mac's Finder sees a machine: sharing started
at the prompt as Studio2, and announced to the group - its PTR, SRV, TXT,
`_device-info._tcp` and address; asked for `_smb._tcp.local`, answered with
`Studio2._smb._tcp.local` on port 445 at `studio2.local`, 10.9.0.2; asked
what kind of machine it is, `model=Kosmos`; asked about a name it does not
hold, silent, and asked before sharing started, silent (the controls); and
sharing stopped, a goodbye, a time to live of 0.

Usage: run_mdns.py IMAGE
"""

import os
import random
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402
import run_udp as U                                         # noqa: E402

MDNS = U.MDNS


def name(text):
    out = b""
    for label in text.split("."):
        out += bytes([len(label)]) + label.encode()
    return out + b"\0"


def record(owner, kind, data, ttl=120, flush=True):
    cls = 0x8001 if flush else 0x0001
    return owner + struct.pack("!HHIH", kind, cls, ttl, len(data)) + data


def studio_answer():
    """PTR, SRV, TXT and A, as one response, compressed the way encoders do."""
    head = struct.pack("!HHHHHH", 0, 0x8400, 0, 4, 0, 0)
    msg = bytearray(head)
    type_at = len(msg)
    type_name = name("_smb._tcp.local")
    inst_label = bytes([6]) + b"Studio" + struct.pack("!H", 0xc000 | type_at)
    # The PTR's data starts after its owner (the type) and ten bytes.
    ptr = record(type_name, 12, inst_label, ttl=4500, flush=False)
    msg += ptr
    inst_at = type_at + len(type_name) + 10          # where the instance's name starts
    host = name("studio.local")
    srv_data = struct.pack("!HHH", 0, 0, 445) + host
    srv_owner = struct.pack("!H", 0xc000 | inst_at)
    srv_at = len(msg)
    msg += record(srv_owner, 33, srv_data)
    host_at = srv_at + len(srv_owner) + 10 + 6          # the SRV's target, in the message
    msg += record(srv_owner, 16, b"\0", ttl=4500)
    msg += record(struct.pack("!H", 0xc000 | host_at), 1, U.PEER_IP)
    return bytes(msg)


def screens_answer():
    """Two machines offering their screens (`_rfb._tcp`): Studio, which offers
    its files too, and Pixel, which offers only its screen."""
    recs = []
    for inst, host, ip in (("Studio", "studio.local", U.PEER_IP),
                           ("Pixel", "pixel.local", bytes([10, 9, 0, 5]))):
        full = inst + "._rfb._tcp.local"
        recs.append(record(name("_rfb._tcp.local"), 12, name(full), ttl=4500, flush=False))
        recs.append(record(name(full), 33, struct.pack("!HHH", 0, 0, 5900) + name(host)))
        recs.append(record(name(host), 1, ip))
    return struct.pack("!HHHHHH", 0, 0x8400, 0, len(recs), 0, 0) + b"".join(recs)


def device_answer(inst, model):
    """What kind of machine `inst` is: its `_device-info._tcp` TXT."""
    text = ("model=" + model).encode()
    return (struct.pack("!HHHHHH", 0, 0x8400, 0, 1, 0, 0)
            + record(name(inst + "._device-info._tcp.local"), 16, bytes([len(text)]) + text))


def a_answer(host, address):
    head = struct.pack("!HHHHHH", 0, 0x8400, 0, 1, 0, 0)
    return head + record(name(host), 1, address)


def questions(packet):
    """The names asked in a multicast DNS question to the group, with types."""
    u = U.udp_of(packet)
    if not u or u[3] != 5353 or u[1] != MDNS:
        return []
    data = u[4]
    if len(data) < 12 or (struct.unpack("!H", data[2:4])[0] & 0x8000):
        return []
    count, at, out = struct.unpack("!H", data[4:6])[0], 12, []
    for _ in range(count):
        labels = []
        while at < len(data) and data[at] != 0:
            n = data[at]
            labels.append(data[at + 1:at + 1 + n].decode(errors="replace"))
            at += 1 + n
        at += 1
        kind = struct.unpack("!H", data[at:at + 2])[0]
        at += 4
        out.append((".".join(labels), kind))
    return out


def read_name(data, at):
    """A name at `at`, pointers followed; and where it ends."""
    labels, end, jumps = [], None, 0
    while at < len(data):
        n = data[at]
        if n == 0:
            at += 1
            break
        if n & 0xc0 == 0xc0:
            if end is None:
                end = at + 2
            at = ((n & 0x3f) << 8) | data[at + 1]
            jumps += 1
            if jumps > 20:
                break
            continue
        labels.append(data[at + 1:at + 1 + n].decode(errors="replace"))
        at += 1 + n
    return ".".join(labels), (end if end is not None else at)


def answers(packet):
    """The records of a multicast DNS response from the guest: (name, type,
    ttl, data, the message), or [] for anything else."""
    u = U.udp_of(packet)
    if not u or u[2] != 5353 or u[1] != MDNS:
        return []
    data = u[4]
    if len(data) < 12 or not (struct.unpack("!H", data[2:4])[0] & 0x8000):
        return []
    qd, an, ns, ar = struct.unpack("!HHHH", data[4:12])
    at, out = 12, []
    for _ in range(qd):
        _, at = read_name(data, at)
        at += 4
    for _ in range(an + ns + ar):
        owner, at = read_name(data, at)
        if at + 10 > len(data):
            break
        kind, _, ttl, length = struct.unpack("!HHIH", data[at:at + 10])
        at += 10
        out.append((owner, kind, ttl, data[at:at + length], data, at))
        at += length
    return out


def query(qname, kind):
    head = struct.pack("!HHHHHH", 0, 0, 1, 0, 0, 0)
    return head + name(qname) + struct.pack("!HH", kind, 1)


def boot(image, command, setup=None, done=None, shot=None):
    """One boot running `command`, the peer answering, until `done(seen)` -
    by default, until the `mdns` program has said something."""
    done = done or (lambda seen: "mdns: " in seen.split("mdnsd: listening")[-1])

    ours, theirs = random.randint(20000, 40000), random.randint(40001, 60000)
    seg = U.Segment(ours, theirs)
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + [
        "-netdev", "dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=%d,"
                   "remote.type=inet,remote.host=127.0.0.1,remote.port=%d" % (theirs, ours),
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/net_address,string=10.9.0.2/24",
        # A comma in a QEMU option's value is written twice.
        "-fw_cfg", "name=opt/kosmos/boot,string=" + command.replace(",", ",,"),
    ])
    try:
        guest = R.Guest(image, 120)
    finally:
        setattr(R, board, saved)

    asked, seen = [], ""
    answered = set()

    try:
        guest.wait_for("mdnsd: listening on 224.0.0.251 port 5353", "mdnsd listening")
        if setup:
            setup(seg)
        # Two minutes: the desktop's boot under the whole gate's load is
        # slow to start Tracker, and its network pass is every five seconds.
        end = R.time.monotonic() + 120
        while R.time.monotonic() < end and not done(guest.seen):
            seg.pump(0.2)
            for dst, packet in seg.frames:
                for q, kind in questions(packet):
                    asked.append((q, kind, packet[8], dst))
                    key = (q.lower(), kind)
                    if key in answered:
                        continue
                    answered.add(key)
                    if q.lower() == "_smb._tcp.local" and kind == 12:
                        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, studio_answer())
                    elif q.lower() == "_rfb._tcp.local" and kind == 12:
                        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, screens_answer())
                    elif q.lower() == "studio._device-info._tcp.local" and kind == 16:
                        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353,
                                device_answer("Studio", "Macmini9,1"))
                    elif q.lower() == "pixel.local" and kind == 1:
                        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353,
                                a_answer("pixel.local", bytes([10, 9, 0, 5])))
                    elif q.lower() == "studio.local" and kind == 1:
                        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, a_answer("studio.local", U.PEER_IP))
            seg.frames.clear()
            guest._read_available()
        R.time.sleep(1)
        guest._read_available()
        if shot:
            with open(shot, "wb") as f:
                f.write(guest.screendump())
    finally:
        seen = guest.seen
        guest.close()

    return seen, asked


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    fails = []

    seen, asked = boot(image, "mdns browse _smb._tcp 4")
    ptr = [a for a in asked if a[0].lower() == "_smb._tcp.local" and a[1] == 12]
    if not ptr:
        fails.append("no question for _smb._tcp.local went to the group: %r" % asked[:4])
    elif ptr[0][2] != 255 or ptr[0][3] != U.group_mac(MDNS):
        fails.append("the question left with a time to live of %d to %s" % (ptr[0][2], ptr[0][3].hex()))
    if "mdns: 1 of _smb._tcp" not in seen or "mdns: Studio  studio.local:445  10.9.0.1" not in seen:
        fails.append("browsing did not list Studio with its host, port and address: %r"
                     % [l for l in seen.splitlines() if l.startswith("mdns")][:4])

    seen, _ = boot(image, "mdns resolve studio.local")
    if "mdns: studio.local is 10.9.0.1" not in seen:
        fails.append("studio.local did not resolve: %r"
                     % [l for l in seen.splitlines() if l.startswith("mdns")][:4])

    # An A nobody asked for, sent first; then asked, and not answered.
    def unasked(seg):
        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, a_answer("stranger.local", bytes([10, 9, 0, 66])))
        seg.pump(1)

    # Asked three seconds on, so the record nobody asked for is certainly
    # there first: asked for before it came, keeping it would be right.
    seen, _ = boot(image, "mdns after 3 resolve stranger.local", unasked)
    if "mdns: stranger.local: nobody answered for that name" not in seen:
        fails.append("a name nobody asked about was kept, or the wait did not end: %r"
                     % [l for l in seen.splitlines() if l.startswith("mdns")][:4])

    if " died: " in seen:
        fails.append("something died")

    # **Seen in Tracker, and reached by name** (N11d): the desktop, its
    # Tracker listing Studio in the Network group as seen and not signed
    # into, and Connect given `smb://studio.local`, finding its address.
    def desktop_done(seen):
        return (re.search(r"tracker: network [^\n]*Studio seen", seen) is not None
                and "connect: studio.local is 10.9.0.1" in seen)

    seen, _ = boot(image, "wm notifications,desktop,deskbar,connect:smb://studio.local",
                   done=desktop_done)
    if not re.search(r"tracker: network [^\n]*Studio seen", seen):
        fails.append("Tracker's Network group did not list Studio as seen: %r"
                     % re.findall(r"tracker: network [^\n]*", seen)[-3:])
        # What the desktop was doing instead, for whoever reads the failure.
        print("--- the desktop's last words ---\n" + "\n".join(
            [l for l in seen.splitlines()
             if l.startswith(("tracker", "desktop", "connect", "wm: started", "wm: launching", "process", "mdns"))
             or "already starting" in l][-25:]))
    if "connect: studio.local is 10.9.0.1" not in seen:
        fails.append("Connect did not find studio.local: %r"
                     % re.findall(r"connect: [^\n]*", seen)[-3:])

    # **All of the network, as computers** (`docs/network.html`): Tracker on
    # /Network, the cards - Studio a Mac offering files and its screen, one
    # card for both, and Pixel offering only its screen.
    def cards_done(seen):
        m = re.findall(r"tracker: network view, (\d+) computers?: ([^\n]*)", seen)
        return bool(m) and "Pixel" in m[-1][1] and "Mac \u00b7 files, screen" in m[-1][1]

    seen, _ = boot(image, "wm notifications,desktop,tracker:/Network", done=cards_done)
    views = re.findall(r"tracker: network view, (\d+) computers?: ([^\n]*)", seen)
    last = views[-1] if views else ("0", "")
    if not (last[0] == "2"
            and re.search(r"Studio \(Mac \u00b7 files, screen; Not signed in\)", last[1])
            and re.search(r"Pixel \(Computer \u00b7 screen; Screen sharing\)", last[1])):
        fails.append("All of the network did not show Studio (a Mac, files and screen) and "
                     "Pixel (its screen) as two cards: %r" % (views[-3:],))

    # **This machine, seen as a Mac sees it** (N13).
    fails += announced(image)

    if fails:
        print("FAIL: %d of 15 checks on finding what is on the network by name:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 15 checks on finding what is on the network by name (mdnsd on a wire with a "
          "file server called Studio: _smb._tcp.local asked of the group at a time to live of "
          "255; Studio listed with studio.local:445 and 10.9.0.1 from its PTR, SRV and A, "
          "compressed; studio.local resolved; an A nobody asked for not kept; Studio in "
          "Tracker's Network group, seen and not signed into; and Connect given "
          "smb://studio.local finding 10.9.0.1; and this machine sharing as Studio2, announced, "
          "answering for _smb._tcp with 445 at studio2.local and 10.9.0.2 and model=Kosmos, "
          "silent before it shares and for a name it does not hold, and saying goodbye at 0 when "
          "it stops; and Tracker's All of the network showing Studio and Pixel as two cards)")
    return 0


def announced(image):
    """Sharing started as Studio2, and the network told (N13)."""
    fails = []
    ours, theirs = random.randint(20000, 40000), random.randint(40001, 60000)
    seg = U.Segment(ours, theirs)

    # A disk for /Home: a shared folder is a folder on one, and the keyring
    # keeps the account's password on it (as `run_smbd.py` gives its own).
    import scratch
    import subprocess
    disk = os.path.join(scratch.directory("mdns"), "home.img")
    note = os.path.join(scratch.directory("mdns"), "note.txt")
    with open(note, "w") as f:
        f.write("a note\n")
    if os.path.exists(disk):
        os.remove(disk)
    root = os.path.dirname(HERE)
    subprocess.run([os.path.join(root, "build", "host", "lua"), os.path.join(HERE, "kfs.lua"),
                    "create", disk, "16", note + ":/Home/Notes/note.txt"],
                   check=True, capture_output=True, cwd=root)

    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    blk = "virtio-blk-pci" if board == "X86_ARGS" else "virtio-blk-device"
    saved = getattr(R, board)
    setattr(R, board, saved + [
        "-netdev", "dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=%d,"
                   "remote.type=inet,remote.host=127.0.0.1,remote.port=%d" % (theirs, ours),
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/net_address,string=10.9.0.2/24",
        "-drive", "file=%s,format=raw,if=none,id=mdnsdisk" % disk,
        "-device", "%s,drive=mdnsdisk" % blk,
    ])
    try:
        guest = R.Guest(image, 120)
    finally:
        setattr(R, board, saved)

    def records(seconds, test):
        """Every record the guest sent to the group for `seconds`, until
        `test(records)` is satisfied."""
        got = []
        end = R.time.monotonic() + seconds
        while R.time.monotonic() < end:
            seg.pump(0.2)
            for _, packet in seg.frames:
                got += answers(packet)
            seg.frames.clear()
            guest._read_available()
            if test(got):
                break
        return got

    INST = "Studio2._smb._tcp.local"

    try:
        guest.wait_for("mdnsd: listening on 224.0.0.251 port 5353", "mdnsd listening")
        seg.pump(1)

        # Not sharing yet: asked, silent - it says it shares only while it does.
        seg.frames.clear()
        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, query("_smb._tcp.local", 12))
        got = records(4, lambda g: False)
        if any(r[0] == "_smb._tcp.local" for r in got):
            fails.append("before sharing started, _smb._tcp was answered")

        for line in ("sharing account kosmos n13-password",
                     "sharing folders Notes:/Home/Notes",
                     "sharing start kosmos name Studio2"):
            guest.type(line)
            seg.pump(1.0)
            seg.frames.clear()

        # Announced, unasked: a PTR to Studio2, sent twice.
        got = records(20, lambda g: sum(1 for r in g if r[1] == 12 and r[2] > 0
                                        and read_name(r[4], r[5])[0] == INST) >= 1)
        ptrs = [r for r in got if r[1] == 12 and r[0] == "_smb._tcp.local"
                and read_name(r[4], r[5])[0] == INST]
        if not ptrs:
            fails.append("sharing started, Studio2 was not announced: %r"
                         % [(r[0], r[1]) for r in got][:8])

        # Asked as Finder asks: the type.
        seg.frames.clear()
        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, query("_smb._tcp.local", 12))
        got = records(10, lambda g: any(r[1] == 33 for r in g) and any(r[1] == 1 for r in g))
        srv = [r for r in got if r[1] == 33 and r[0] == INST]
        addr = [r for r in got if r[1] == 1 and r[0] == "studio2.local"]
        if not srv or struct.unpack("!H", srv[0][3][4:6])[0] != 445 \
                or read_name(srv[0][4], srv[0][5] + 6)[0] != "studio2.local":
            fails.append("asked for _smb._tcp, no SRV for Studio2 on 445 at studio2.local: %r"
                         % [(r[0], r[1]) for r in got][:8])
        if not addr or addr[0][3] != bytes([10, 9, 0, 2]):
            fails.append("studio2.local's address was not 10.9.0.2: %r" % [a[3] for a in addr])

        # What kind of machine.
        seg.frames.clear()
        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, query("Studio2._device-info._tcp.local", 16))
        got = records(10, lambda g: any(r[1] == 16 and "device-info" in r[0] for r in g))
        dev = [r for r in got if r[1] == 16 and r[0] == "Studio2._device-info._tcp.local"]
        if not dev or b"model=Kosmos" not in dev[0][3]:
            fails.append("asked what Studio2 is, no model=Kosmos: %r" % [d[3] for d in dev])

        # The control: a name it does not hold, no answer at all.
        seg.frames.clear()
        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, query("nobody-here.local", 1))
        got = records(4, lambda g: False)
        if any(r[0] == "nobody-here.local" for r in got):
            fails.append("a name this machine does not hold was answered")

        # Stopped: a goodbye, the PTR at 0.
        seg.frames.clear()
        guest.type("sharing stop")
        got = records(15, lambda g: any(r[1] == 12 and r[2] == 0 for r in g))
        if not any(r[1] == 12 and r[2] == 0 and r[0] == "_smb._tcp.local" for r in got):
            fails.append("sharing stopped, no goodbye at a time to live of 0: %r"
                         % [(r[0], r[1], r[2]) for r in got][:8])
    except Exception as e:                  # noqa: BLE001 - said below
        fails.append("the announcing boot stopped: %s: %s" % (type(e).__name__, str(e)[:300]))
    finally:
        seen = guest.seen
        guest.close()

    if fails:
        print("--- the announcing boot said ---\n" + "\n".join(
            l for l in seen.splitlines() if l.startswith(("mdnsd", "smbd", "sharing")))[-2000:])
    return fails


if __name__ == "__main__":
    sys.exit(main())
