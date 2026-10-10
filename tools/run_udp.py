#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Datagrams and a multicast group (`docs/sharing.md` N11a).

QEMU's user network carries no multicast, so the machine is put on a wire
of its own instead - `-netdev dgram`, every frame a UDP datagram between two
ports on the Mac's loopback - given its address at boot
(`opt/kosmos/net_address`, since no DHCP answers there), and booted into
`udp echo 9000 239.255.0.1`: port 9000 bound, a group joined (5353 and
multicast DNS's group are `mdnsd`'s), a hello said to the group, and every datagram that arrives said and sent
back to its sender. This file is the other machine on the segment, speaking
its Ethernet frames - answering ARP, sending and reading IPv4 and UDP:

  - the group joined is reported (IGMPv2, to the group's own address);
  - the hello arrives, to the group, on the group's Ethernet address;
  - a datagram to the group arrives at the program, with who sent it, and
    the echo comes back to the sender's address - after an ARP question;
  - a datagram straight to the machine's address does the same;
  - one to a group the machine did not join is not taken.

Usage: run_udp.py IMAGE
"""

import os
import random
import re
import select
import socket
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import run_screenshot as R                                  # noqa: E402

GUEST_IP = bytes([10, 9, 0, 2])
PEER_IP = bytes([10, 9, 0, 1])
PEER_MAC = bytes([0x52, 0x54, 0x00, 0xaa, 0xbb, 0xcc])
MDNS = bytes([224, 0, 0, 251])        # multicast DNS's group (`run_mdns.py`)
GROUP = bytes([239, 255, 0, 1])       # this suite's: 5353 is mdnsd's since N11c
OTHER = bytes([239, 255, 0, 2])
PORT = 9000


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    s = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while s >> 16:
        s = (s & 0xffff) + (s >> 16)
    return (~s) & 0xffff


def group_mac(ip):
    return bytes([0x01, 0x00, 0x5e, ip[1] & 0x7f, ip[2], ip[3]])


class Segment:
    """The Mac's end of the wire: QEMU's frames in and out, one a datagram.

    `-netdev dgram`: QEMU sends each frame to `ours` and takes them at
    `theirs`, both on the loopback - a wire between two machines, which is
    all a segment needs to be here. (`-netdev socket,mcast=` was the first
    try, and on this Mac what QEMU sent never reached a listener.)
    """

    def __init__(self, ours, theirs):
        self.theirs = theirs
        self.rx = socket.socket(socket.AF_INET, socket.SOCK_DGRAM, socket.IPPROTO_UDP)
        self.rx.bind(("127.0.0.1", ours))
        self.tx = self.rx
        self.guest_mac = None
        self.frames = []            # every IPv4 frame the guest sent: (dst mac, ip packet)

    def send(self, frame):
        self.tx.sendto(frame, ("127.0.0.1", self.theirs))

    def ip(self, dst_mac, src, dst, proto, body, ttl=64):
        header = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(body), random.randint(0, 65535),
                             0x4000, ttl, proto, 0, src, dst)
        header = header[:10] + struct.pack("!H", checksum(header)) + header[12:]
        self.send(dst_mac + PEER_MAC + b"\x08\x00" + header + body)

    def udp(self, dst_mac, dst, sport, dport, data):
        body = struct.pack("!HHHH", sport, dport, 8 + len(data), 0) + data
        self.ip(dst_mac, PEER_IP, dst, 17, body, ttl=255)

    def pump(self, seconds):
        """Frames read for a while: ARP answered, the guest's IPv4 kept."""
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            ready, _, _ = select.select([self.rx], [], [], max(0.0, end - time.monotonic()))
            if not ready:
                continue
            frame, _ = self.rx.recvfrom(4096)
            if len(frame) < 14 or frame[6:12] == PEER_MAC:
                continue                    # our own, looped back
            self.guest_mac = frame[6:12]
            kind = frame[12:14]
            if kind == b"\x08\x06" and len(frame) >= 42:
                op = struct.unpack("!H", frame[20:22])[0]
                if op == 1 and frame[38:42] == PEER_IP:      # who has us?
                    reply = (frame[6:12] + PEER_MAC + b"\x08\x06"
                             + struct.pack("!HHBBH", 1, 0x0800, 6, 4, 2)
                             + PEER_MAC + PEER_IP + frame[22:28] + frame[28:32])
                    self.send(reply)
            elif kind == b"\x08\x00":
                self.frames.append((frame[0:6], frame[14:]))

    def wait(self, test, seconds):
        end = time.monotonic() + seconds
        while time.monotonic() < end:
            for dst, packet in self.frames:
                if test(dst, packet):
                    return dst, packet
            self.pump(0.2)
        return None


def udp_of(packet):
    """(src ip, dst ip, sport, dport, data) of a UDP packet, or None."""
    if len(packet) < 28 or packet[9] != 17:
        return None
    ihl = (packet[0] & 0x0f) * 4
    sport, dport, length = struct.unpack("!HHH", packet[ihl:ihl + 6])
    return packet[12:16], packet[16:20], sport, dport, packet[ihl + 8:ihl + length]


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/x86_64/kosmos.elf"
    ours, theirs = random.randint(20000, 40000), random.randint(40001, 60000)
    seg = Segment(ours, theirs)
    board = "X86_ARGS" if R.machine(image) == "x86_64" else "QEMU_ARGS"
    saved = getattr(R, board)
    setattr(R, board, saved + [
        "-netdev", "dgram,id=net0,local.type=inet,local.host=127.0.0.1,local.port=%d,"
                   "remote.type=inet,remote.host=127.0.0.1,remote.port=%d" % (theirs, ours),
        "-device", R.device(image, "net") + ",netdev=net0",
        "-fw_cfg", "name=opt/kosmos/net_address,string=10.9.0.2/24",
        "-fw_cfg", "name=opt/kosmos/boot,string=udp echo %d 239.255.0.1" % PORT,
    ])
    try:
        guest = R.Guest(image, 120)
    finally:
        setattr(R, board, saved)

    fails, said = [], {}

    try:
        guest.wait_for("udp: on port %d, in 239.255.0.1" % PORT, "the port bound")

        # The group joined, said to the network.
        report = seg.wait(lambda dst, p: p[9] == 2 and len(p) >= 28
                          and p[(p[0] & 15) * 4] == 0x16 and p[16:20] == GROUP
                          and dst == group_mac(GROUP), 10)
        if not report:
            fails.append("no IGMP report for 239.255.0.1 was sent")
        else:
            said["report"] = True

        # The hello, to the group's address on the wire.
        hello = seg.wait(lambda dst, p: (udp_of(p) or (0, 0, 0, 0, b""))[4] == b"hello from kosmos", 10)
        if not hello:
            fails.append("the hello to the group never came")
        elif hello[0] != group_mac(GROUP) or udp_of(hello[1])[1] != GROUP or udp_of(hello[1])[3] != PORT:
            fails.append("the hello went to %r, %r, not the group's" % (hello[0].hex(), udp_of(hello[1])[1]))
        elif hello[1][8] != 255:
            fails.append("the hello to the group left with a time to live of %d, not "
                         "255" % hello[1][8])

        guest_mac = seg.guest_mac

        # To the group: taken, and echoed to the sender.
        seg.udp(group_mac(GROUP), GROUP, PORT, PORT, b"peer to group")
        echo = seg.wait(lambda dst, p: (udp_of(p) or (0, 0, 0, 0, b""))[4] == b"echo: peer to group", 10)
        if not echo:
            fails.append("a datagram to the group was not echoed back")
        elif udp_of(echo[1])[1] != PEER_IP or echo[0] != PEER_MAC or udp_of(echo[1])[3] != PORT:
            fails.append("the echo went to %r port %d" % (udp_of(echo[1])[1], udp_of(echo[1])[3]))
        try:
            guest.wait_for("udp: from 10.9.0.1:%d, 13 bytes: peer to group" % PORT,
                           "the group's datagram said")
        except Exception:                       # noqa: BLE001 - a failure below
            fails.append("the program did not say the group's datagram arrived")

        # Straight to the machine, from another port.
        seg.udp(guest_mac or b"\xff" * 6, GUEST_IP, 7777, PORT, b"peer direct")
        direct = seg.wait(lambda dst, p: (udp_of(p) or (0, 0, 0, 0, b""))[4] == b"echo: peer direct", 10)
        if not direct or udp_of(direct[1])[3] != 7777:
            fails.append("a datagram to the machine's address was not echoed to port 7777")

        # A group it did not join: not taken.
        mark = len(guest.seen)
        seg.udp(group_mac(OTHER), OTHER, PORT, PORT, b"not yours")
        seg.pump(3)
        guest._read_available()
        if "not yours" in guest.seen[mark:]:
            fails.append("a datagram to a group the machine did not join was taken")
    except Exception as e:                      # noqa: BLE001 - said below
        fails.append("the machine stopped: %s: %s" % (type(e).__name__, str(e).splitlines()[0]))
    finally:
        seen = guest.seen
        guest.close()

    if " died: " in seen:
        fails.append("something died: " + seen[seen.find(" died: ") - 80:][:300])

    if fails:
        print("FAIL: %d of 6 checks on datagrams and a multicast group:" % len(fails))
        for f in fails:
            print("  " + f)
        tail = [l for l in seen.splitlines() if re.match(r"(udp|net):", l)]
        print("  the machine said: " + " | ".join(tail[-8:]))
        return 1

    print("PASS: 6 checks on datagrams and a multicast group (on a wire of its own, the "
          "address from the boot: 239.255.0.1 joined and reported; a hello to the group on "
          "its own Ethernet address with a time to live of 255; a datagram to the group said and echoed to its sender "
          "after an ARP question; one to the machine's address echoed to its port; one to a "
          "group not joined left alone)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
