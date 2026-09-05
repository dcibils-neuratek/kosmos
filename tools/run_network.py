#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The network, from a frame on the wire to a page fetched over TCP.

**Nothing inside the guest can establish this.** `sys.net_send` returning
true says the card took the bytes, which is a statement about a virtqueue
and not about a network; a driver that filled the ring correctly and never
kicked the device would pass that check for ever. So the witness is
outside: QEMU's `filter-dump` writes a pcap, and this reads it.

What it actually pins down, in order of how easy each is to get wrong:

  * The **twelve-byte virtio-net header** is right. It goes in front of every
    frame in both directions and is not part of the packet. The legacy layout
    is ten bytes, and a driver that used the wrong length hands over a frame
    two bytes out of alignment - which decodes as a different protocol
    rather than as an error, so nothing complains and everything is wrong.
    The destination address landing at byte 0 of the capture is what says
    the header was the length the driver thought.
  * The **descriptor flags** are the right way round. Transmit buffers are
    read by the device and receive buffers are written by it; getting that
    backwards is a ring that looks correct and moves nothing.
  * The **MAC came from the card** rather than being invented. The source
    address in the capture is what `VIRTIO_NET_F_MAC` reported, so a
    feature that was asked for and not granted shows up here.

And the second boot checks the branch this codebase has got wrong four
times: a machine with no card must still reach a prompt. Every device grant
in `init.lua` carries a comment about the time it did not.
"""

import http.server
import os
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import run_screenshot


ETHERTYPE = 0x88b5          # IEEE's local-experimental block; see netframe.lua
BROADCAST = "ff:ff:ff:ff:ff:ff"


class Failure(Exception):
    pass


def frames(path):
    """Every frame in a pcap, as (destination, source, ethertype, payload)."""
    with open(path, "rb") as f:
        data = f.read()

    if len(data) < 24:
        raise Failure("the capture has no header: QEMU wrote nothing at all")

    magic, _, _, _, _, _, link = struct.unpack("<IHHiIII", data[:24])

    if magic != 0xa1b2c3d4:
        raise Failure(f"not a little-endian pcap (magic {magic:08x})")

    if link != 1:
        raise Failure(f"link type {link}, expected 1 (Ethernet)")

    out = []
    at = 24

    while at + 16 <= len(data):
        _, _, caplen, wirelen = struct.unpack("<IIII", data[at:at + 16])
        at += 16
        frame = data[at:at + caplen]
        at += caplen

        if len(frame) < 14:
            continue                # too short to be a frame; not ours

        out.append((
            ":".join("%02x" % b for b in frame[0:6]),
            ":".join("%02x" % b for b in frame[6:12]),
            struct.unpack(">H", frame[12:14])[0],
            frame[14:],
            wirelen,
        ))

    return out


_served = {}


def _fetch(port):
    """Ask the guest for a page, from here.

    Retried, because the server is started by typing at a prompt and there
    is no moment this side can observe when it has finished listening. Five
    attempts a second apart is the difference between a test that is about
    the network and one that is about timing.
    """
    import http.client

    for _ in range(10):
        try:
            c = http.client.HTTPConnection("127.0.0.1", port, timeout=10)
            c.request("GET", "/")
            r = c.getresponse()
            _served["code"] = r.status
            _served["kind"] = r.getheader("Content-Type")
            _served["body"] = r.read().decode("utf-8", "replace")
            c.close()
            return
        except Exception as e:          # noqa: BLE001 - any of them means retry
            _served["error"] = repr(e)
            time.sleep(1)


def boot(image, extra, commands, seconds=90, then=None):
    """One run, with whatever QEMU arguments the caller wants added.

    `then` runs while the guest is still up, after the commands have been
    typed - which is what a server needs: the last command does not return,
    because the server is still serving.
    """
    saved = run_screenshot.QEMU_ARGS
    run_screenshot.QEMU_ARGS = saved + extra

    try:
        guest = run_screenshot.Guest(image, seconds)
    finally:
        run_screenshot.QEMU_ARGS = saved

    try:
        guest.wait_for(run_screenshot.PROMPT, "the prompt")
        guest.seen = ""

        for command in commands:
            guest.type(command + "\n")

            if then is not None and command is commands[-1]:
                time.sleep(3)       # let it reach its accept loop
                break

            guest.wait_for(run_screenshot.PROMPT, command)

        if then is not None:
            then()
            time.sleep(1)
            guest._read_available()

        return guest.seen.replace("\r", "")
    finally:
        guest.close()


def main():
    image = sys.argv[1] if len(sys.argv) > 1 else "build/kosmos.elf"
    checks = 0

    work = tempfile.mkdtemp()
    pcap = os.path.join(work, "frames.pcap")

    try:
        # ---- with a card: a frame goes out and is captured ----
        out = boot(image, [
            "-netdev", "user,id=net0",
            "-device", "virtio-net-device,netdev=net0",
            "-object", f"filter-dump,id=dump0,netdev=net0,file={pcap}",
        ], ["netframe 2"])

        if "no network card" in out:
            raise Failure("the machine did not find the card it was given.\n"
                          + out[-800:])

        checks += 1

        if "sent" not in out:
            raise Failure("the card would not take the frame.\n" + out[-800:])

        checks += 1

        mine = [f for f in frames(pcap) if f[2] == ETHERTYPE]

        if not mine:
            everything = frames(pcap)
            raise Failure(
                "the frame never reached the wire. The guest said it sent "
                f"one; the capture holds {len(everything)} frame(s), none "
                f"with ethertype 0x{ETHERTYPE:04x}."
            )

        checks += 1

        dst, src, _, payload, wirelen = mine[0]

        if dst != BROADCAST:
            raise Failure(
                f"the destination came out as {dst}, not {BROADCAST}. The "
                "frame is offset: almost certainly the virtio-net header is "
                "not the twelve bytes VERSION_1 requires."
            )

        checks += 1

        #
        # The source is the card's own address, which is only true if
        # VIRTIO_NET_F_MAC was granted and configuration space was read a
        # byte at a time. QEMU's default for virtio-net is 52:54:00:12:34:56
        # and this checks the shape rather than the value, because the value
        # is QEMU's to change.
        #
        if not src.startswith("52:54:00"):
            raise Failure(
                f"the source address is {src}, which is not the card's. "
                "Either NET_F_MAC was not granted or the MAC was read wrong."
            )

        checks += 1

        if b"kosmos" not in payload:
            raise Failure(f"the payload arrived as {payload!r}")

        checks += 1

        if wirelen != 14 + len(payload):
            raise Failure(
                f"the frame is {wirelen} bytes on the wire but its header "
                f"and payload are {14 + len(payload)}. The length handed to "
                "the descriptor counts the virtio header when it should not."
            )

        checks += 1

        # ---- and the whole path: ARP, IP, ICMP, and an answer ----
        #
        # **The gateway, not the internet.** slirp answers ARP for 10.0.2.2
        # and replies to an echo without a packet leaving this computer, so
        # this checks the entire stack - resolve, route, build, checksum,
        # match the reply to the caller - and still passes on a train. A test
        # that needs 8.8.8.8 is a test that fails for a reason that has
        # nothing to do with the code.
        #
        out = boot(image, [
            "-netdev", "user,id=net0",
            "-device", "virtio-net-device,netdev=net0",
        ], ["ping 10.0.2.2 2"])

        if "no network card" in out or "has no address" in out:
            raise Failure("the stack did not come up.\n" + out[-900:])

        checks += 1

        if "2 sent, 2 received" not in out:
            raise Failure(
                "the gateway did not answer both echoes. Everything from ARP "
                "to the checksum is on this path, and a wrong field produces "
                "no answer rather than a wrong one.\n" + out[-900:])

        checks += 1

        if "round trip min/avg/max" not in out:
            raise Failure("no round trip was reported.\n" + out[-900:])

        checks += 1

        # ---- and a whole TCP connection, to a server on this computer ----
        #
        # **The host, not the internet.** slirp maps this Mac as 10.0.2.2, so
        # a server started here is reachable from the guest without a packet
        # leaving the machine - which makes this deterministic, offline, and
        # about the stack rather than about somebody else's uptime.
        #
        # What it exercises is everything TCP has: the three-way handshake,
        # a segment out with data on it, sequence numbers and acknowledgement,
        # bytes arriving through the shared ring, and a close that the far
        # end starts. HTTP/1.0 is chosen because it *ends by ending* - the
        # server closes rather than keeping the connection open - so the ring's
        # `closed` flag is on the path rather than an extra.
        #
        body = b"the quick brown fox jumps over the lazy dog\n" * 8
        served = {"path": None}

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_GET(self):
                served["path"] = self.path
                self.send_response(200)
                self.send_header("Content-Type", "text/plain")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *a):
                pass

        httpd = http.server.HTTPServer(("0.0.0.0", 0), Handler)
        port = httpd.server_address[1]
        thread = threading.Thread(target=httpd.serve_forever, daemon=True)
        thread.start()

        try:
            out = boot(image, [
                "-netdev", "user,id=net0",
                "-device", "virtio-net-device,netdev=net0",
            ], [f"fetch 10.0.2.2 {port} /hello"], seconds=120)
        finally:
            httpd.shutdown()

        if "connection refused" in out or "timed out" in out:
            raise Failure(
                "the connection never opened. The handshake is SYN, SYN-ACK "
                f"and ACK, and the server on port {port} was listening.\n"
                + out[-900:])

        checks += 1

        if served["path"] != "/hello":
            raise Failure(
                "the request never arrived, or arrived wrong: the server saw "
                f"{served['path']!r}. That is a segment with data on it, so "
                "it is the sequence number, the checksum or the data offset."
            )

        checks += 1

        if "the quick brown fox" not in out:
            raise Failure(
                "the body did not come back through the ring.\n" + out[-900:])

        checks += 1

        #
        # And all of it, which is the check that catches a stack that loses
        # the last segment - the close and the final bytes can arrive
        # together, and a client that stopped at `closed` would drop them.
        #
        if out.count("the quick brown fox") != 8:
            raise Failure(
                "the body came back in pieces: %d of the 8 lines arrived. "
                "The close and the last bytes can be in one segment."
                % out.count("the quick brown fox"))

        checks += 1

        # ---- and serving: this machine asks the guest for a page ----
        #
        # The other direction, which needed the half of TCP that was left
        # out on purpose - LISTEN, SYN_RECEIVED, and a way to hand a caller a
        # connection it did not ask for. `roadmap.md` said an HTTP server was
        # the argument that would settle whether to build it.
        #
        # `hostfwd` is what makes it reachable: slirp drops everything
        # inbound until a port is forwarded, so without this the guest would
        # be listening where nothing can knock.
        #
        # **The image is the check that matters.** A page fits in one segment
        # and would pass with almost any bug; 150 KB does not fit in the ring,
        # so it exercises the client waiting for space, the acknowledgement
        # that frees it, and a FIN that must not go out until the last byte
        # has. Both of those were wrong when this was written and neither said
        # so - the file simply arrived short.
        #
        import random

        forward = random.randint(20000, 60000)
        big = bytes(range(256)) * 700          # 179,200 bytes, ring is 16 KB

        out = boot(image, [
            "-netdev", f"user,id=net0,hostfwd=tcp::{forward}-:80",
            "-device", "virtio-net-device,netdev=net0",
        ], [
            'fs.write("/data/w/index.html", "<h1>Kosmos</h1>")',
            "httpd 80 /data/w",
        ], seconds=120, then=lambda: _fetch(forward))

        if _served.get("code") != 200:
            raise Failure(
                "this computer could not fetch a page from the guest: "
                f"{_served}\n{out[-900:]}")

        checks += 1

        if "Kosmos" not in _served.get("body", ""):
            raise Failure(f"the page came back as {_served.get('body')!r}")

        checks += 1

        if _served.get("kind") != "text/html":
            raise Failure(
                f"served as {_served.get('kind')!r}, not text/html")

        checks += 1

        # ---- and with no card at all ----
        #
        # The branch this codebase has got wrong four times, each recorded in
        # a comment beside a device grant in `init.lua`: the kernel refuses a
        # spawn that passes on authority the parent does not hold, so asking
        # unconditionally kills the shell at boot on a machine without the
        # device. A machine with no network is a machine.
        #
        out = boot(image, [], ["netframe"])

        if "no network card" not in out:
            raise Failure(
                "a machine with no card did not say so.\n" + out[-800:])

        checks += 1

        print(f"PASS: {checks} checks on the network: a frame this computer "
              "read out of a capture, and a host that answered.")
        return 0
    except Failure as e:
        print(f"FAIL: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
