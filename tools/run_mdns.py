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

Usage: run_mdns.py IMAGE
"""

import os
import random
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


def boot(image, command, setup=None):
    ours, theirs = random.randint(20000, 40000), random.randint(40001, 60000)
    seg = U.Segment(ours, theirs)
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + [
        "-netdev", "dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=%d,"
                   "remote.type=inet,remote.host=127.0.0.1,remote.port=%d" % (theirs, ours),
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/net_address,string=10.9.0.2/24",
        "-fw_cfg", "name=opt/kosmos/boot,string=" + command,
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
        end = R.time.monotonic() + 25
        while R.time.monotonic() < end and "mdns: " not in guest.seen.split("mdnsd: listening")[-1]:
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
                    elif q.lower() == "studio.local" and kind == 1:
                        seg.udp(U.group_mac(MDNS), MDNS, 5353, 5353, a_answer("studio.local", U.PEER_IP))
            seg.frames.clear()
            guest._read_available()
        R.time.sleep(1)
        guest._read_available()
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

    seen, _ = boot(image, "mdns resolve stranger.local", unasked)
    if "mdns: stranger.local: nobody answered for that name" not in seen:
        fails.append("a name nobody asked about was kept, or the wait did not end: %r"
                     % [l for l in seen.splitlines() if l.startswith("mdns")][:4])

    if " died: " in seen:
        fails.append("something died")

    if fails:
        print("FAIL: %d of 5 checks on finding what is on the network by name:" % len(fails))
        for f in fails:
            print("  " + f)
        return 1

    print("PASS: 5 checks on finding what is on the network by name (mdnsd on a wire with a "
          "file server called Studio: _smb._tcp.local asked of the group at a time to live of "
          "255; Studio listed with studio.local:445 and 10.9.0.1 from its PTR, SRV and A, "
          "compressed; studio.local resolved; an A nobody asked for not kept)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
