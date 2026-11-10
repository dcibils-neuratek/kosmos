#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""A server far away, for a guest's TCP to be measured against
(`tools/run_tcpin.py --far`, 9 November 2026).

QEMU's user network answers a guest's TCP with a TCP of its own, slirp's,
which does not scale its window (RFC 7323) - so no suite behind it could see
whether Kosmos's does. This is the other end of the wire instead: QEMU's
`-netdev dgram` hands it every Ethernet frame as a UDP datagram, and it is
a machine at `PEER_IP` that answers ARP and one TCP connection, and sends a
file down it.

What it does as a sender, and nothing more:

- **its SYN-ACK offers a window scale** when the guest's SYN did, and takes
  the guest's scale from that SYN; every window the guest says after its
  SYN is read shifted by it;
- **it sends as much as that window allows**, a segment of the MSS at a
  time - no congestion window, so what bounds it is the guest's window and
  the distance, which is what is being measured;
- **every frame it sends is held for `delay` seconds first**, so a round
  trip is that long: a server that far away;
- **it sends again** from the oldest byte not acknowledged on three
  repeated acknowledgements, or when nothing has been acknowledged for
  three round trips;
- **its FIN** follows the last byte, and it answers the guest's.

It speaks to one connection, once, and says what it saw: the scale the
guest offered and the most window the guest said.
"""

import heapq
import select
import socket
import struct
import time

PEER_MAC = bytes.fromhex("525400fa7e01")
PEER_IP = bytes([10, 9, 0, 1])
GUEST_IP = bytes([10, 9, 0, 2])
MSS = 1460
OUR_SCALE = 7

ETH_IP, ETH_ARP = 0x0800, 0x0806
SYN, ACK, FIN, RST, PSH = 0x02, 0x10, 0x01, 0x04, 0x08


def checksum(data):
    if len(data) % 2:
        data += b"\0"
    total = sum(struct.unpack("!%dH" % (len(data) // 2), data))
    while total >> 16:
        total = (total & 0xFFFF) + (total >> 16)
    return (~total) & 0xFFFF


def tcp_options(raw):
    """MSS and window scale from a segment's options; None where absent."""
    mss = scale = None
    at = 0
    while at < len(raw):
        kind = raw[at]
        if kind == 0:
            break
        if kind == 1:
            at += 1
            continue
        if at + 1 >= len(raw) or raw[at + 1] < 2:
            break
        length = raw[at + 1]
        if kind == 2 and length == 4:
            mss = struct.unpack("!H", raw[at + 2:at + 4])[0]
        if kind == 3 and length == 3:
            scale = min(raw[at + 2], 14)
        at += length
    return mss, scale


class FarPeer:
    def __init__(self, data, port, delay, listen_port, qemu_port):
        self.data = data
        self.port = port
        self.delay = delay
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.sock.bind(("127.0.0.1", listen_port))
        self.qemu = ("127.0.0.1", qemu_port)
        self.guest_mac = None
        self.held = []                  # (when, n, frame): what is on the wire
        self.n = 0
        self.conn = None
        self.said = {"offered_scale": None, "most_window": 0, "resent": 0,
                     "done": False, "acked": 0, "frames": 0, "kinds": {}}

    # -- the wire, with its distance --------------------------------------

    def send_frame(self, frame):
        self.said["sent"] = self.said.get("sent", 0) + 1
        self.n += 1
        heapq.heappush(self.held, (time.monotonic() + self.delay, self.n, frame))

    def release(self):
        now = time.monotonic()
        while self.held and self.held[0][0] <= now:
            _, _, frame = heapq.heappop(self.held)
            self.sock.sendto(frame, self.qemu)

    def eth(self, kind, payload):
        frame = (self.guest_mac or b"\xff" * 6) + PEER_MAC + struct.pack("!H", kind) + payload
        return frame + b"\0" * max(0, 60 - len(frame))     # a wire's least frame

    # -- ARP ---------------------------------------------------------------

    def arp(self, body):
        op = struct.unpack("!H", body[6:8])[0]
        sender_mac, sender_ip, target_ip = body[8:14], body[14:18], body[24:28]
        if op == 1 and target_ip == PEER_IP:
            self.guest_mac = sender_mac
            reply = (struct.pack("!HHBBH", 1, ETH_IP, 6, 4, 2) + PEER_MAC + PEER_IP
                     + sender_mac + sender_ip)
            self.send_frame(self.eth(ETH_ARP, reply))

    # -- TCP ---------------------------------------------------------------

    def segment(self, flags, seq, ack, payload=b"", options=b""):
        c = self.conn
        window = min(0xFFFF, (8 << 20) >> (OUR_SCALE if c["scaled"] else 0))
        if flags & SYN:
            window = 0xFFFF
        header = struct.pack("!HHIIBBHHH", self.port, c["guest_port"], seq & 0xFFFFFFFF,
                             ack & 0xFFFFFFFF, ((20 + len(options)) // 4) << 4, flags,
                             window, 0, 0) + options
        pseudo = PEER_IP + GUEST_IP + struct.pack("!BBH", 0, 6, len(header) + len(payload))
        csum = checksum(pseudo + header + payload)
        header = header[:16] + struct.pack("!H", csum) + header[18:]
        tcp = header + payload
        ip = struct.pack("!BBHHHBBH4s4s", 0x45, 0, 20 + len(tcp), self.n & 0xFFFF, 0x4000,
                         64, 6, 0, PEER_IP, GUEST_IP)
        ip = ip[:10] + struct.pack("!H", checksum(ip)) + ip[12:]
        self.send_frame(self.eth(ETH_IP, ip + tcp))

    def tcp(self, ip_header, tcp):
        sport, dport, seq, ack, off, flags, window = struct.unpack("!HHIIBBH", tcp[:16])
        hlen = (off >> 4) * 4
        payload = tcp[hlen:]
        if dport != self.port:
            return

        if flags & SYN and not flags & ACK:
            mss, scale = tcp_options(tcp[20:hlen])
            self.said["offered_scale"] = scale
            self.conn = {
                "guest_port": sport, "rcv_nxt": seq + 1, "iss": 1000,
                "snd_una": 1001, "snd_nxt": 1001, "scaled": scale is not None,
                "their_scale": scale or 0, "window": window, "mss": min(mss or 536, MSS),
                "dupacks": 0, "progress_at": time.monotonic(), "fin_sent": False,
                "open": False,
            }
            options = struct.pack("!BBH", 2, 4, MSS)
            if scale is not None:
                options += bytes([1, 3, 3, OUR_SCALE])
            self.segment(SYN | ACK, 1000, seq + 1, options=options)
            return

        c = self.conn
        if c is None or sport != c["guest_port"]:
            return
        if flags & RST:
            self.said["done"] = True
            return

        c["open"] = True
        said_window = window << c["their_scale"]
        self.said["most_window"] = max(self.said["most_window"], said_window)

        if flags & ACK:
            if ack - c["snd_una"] > 0 and ack <= c["snd_nxt"] + 1:
                c["snd_una"] = ack
                c["dupacks"] = 0
                c["progress_at"] = time.monotonic()
                self.said["acked"] = ack - 1001
            elif ack == c["snd_una"] and not payload and c["snd_nxt"] != c["snd_una"]:
                c["dupacks"] += 1
                if c["dupacks"] == 3:
                    self.resend()
            c["window"] = said_window

        if payload:
            c["rcv_nxt"] = seq + len(payload)
            self.segment(ACK, c["snd_nxt"], c["rcv_nxt"])

        if flags & FIN:
            c["rcv_nxt"] = seq + len(payload) + 1
            self.segment(ACK, c["snd_nxt"], c["rcv_nxt"])
            if c["fin_sent"]:
                self.said["done"] = True

        if c["fin_sent"] and c["snd_una"] == c["snd_nxt"]:
            self.said["done"] = self.said["done"] or bool(flags & FIN)

    def resend(self):
        c = self.conn
        self.said["resent"] += 1
        c["snd_nxt"] = c["snd_una"]
        c["fin_sent"] = c["fin_sent"] and c["snd_una"] - 1001 > len(self.data)

    def pump(self):
        c = self.conn
        if c is None or not c["open"]:
            return
        end = 1001 + len(self.data)
        while c["snd_nxt"] < end and c["snd_nxt"] - c["snd_una"] + c["mss"] <= c["window"]:
            at = c["snd_nxt"] - 1001
            piece = self.data[at:at + c["mss"]]
            self.segment(ACK | PSH, c["snd_nxt"], c["rcv_nxt"], piece)
            c["snd_nxt"] += len(piece)
        if c["snd_nxt"] == end and not c["fin_sent"]:
            self.segment(FIN | ACK, c["snd_nxt"], c["rcv_nxt"])
            c["snd_nxt"] += 1
            c["fin_sent"] = True
        if (c["snd_una"] != c["snd_nxt"]
                and time.monotonic() - c["progress_at"] > max(1.0, 6 * self.delay)):
            c["progress_at"] = time.monotonic()
            self.resend()

    # -- the loop ----------------------------------------------------------

    def run(self, seconds):
        deadline = time.monotonic() + seconds
        while time.monotonic() < deadline and not self.said["done"]:
            wait = 0.005
            if self.held:
                wait = max(0.0, min(wait, self.held[0][0] - time.monotonic()))
            ready, _, _ = select.select([self.sock], [], [], wait)
            if ready:
                frame, _ = self.sock.recvfrom(65536)
                kind = struct.unpack("!H", frame[12:14])[0]
                self.said["frames"] += 1
                self.said["kinds"][kind] = self.said["kinds"].get(kind, 0) + 1
                if kind == ETH_ARP:
                    self.arp(frame[14:])
                elif kind == ETH_IP and frame[14 + 9] == 6 and frame[14 + 16:14 + 20] == PEER_IP:
                    self.guest_mac = frame[6:12]
                    ihl = (frame[14] & 15) * 4
                    total = struct.unpack("!H", frame[16:18])[0]
                    self.tcp(frame[14:14 + ihl], frame[14 + ihl:14 + total])
            self.pump()
            self.release()
        return self.said
