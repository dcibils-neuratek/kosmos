#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""The Mac's end of `netspeed` (user/bin/programs/netspeed.lua).

  netspeed_peer.py [MB]     serve MB (100) on 47701, swallow on 47702

On 47701 each connection is sent MB of bytes and closed; on 47702 each is
read to its end. Both say their own time, so the two ends' numbers can be
held beside each other. One connection at a time each; ^C ends it.
"""

import os
import socket
import sys
import threading
import time

MB = int(sys.argv[1]) if len(sys.argv) > 1 else 100


def serve():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 47701))
    s.listen(1)
    block = os.urandom(1 << 20)
    while True:
        c, peer = s.accept()
        t = time.monotonic()
        for _ in range(MB):
            c.sendall(block)
        c.close()
        took = time.monotonic() - t
        print("peer: sent %d MB to %s in %.2f s, %.1f MB/s" % (MB, peer[0], took, MB / took),
              flush=True)


def swallow():
    s = socket.socket()
    s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", 47702))
    s.listen(1)
    while True:
        c, peer = s.accept()
        t, n = time.monotonic(), 0
        while True:
            d = c.recv(1 << 20)
            if not d:
                break
            n += len(d)
        c.close()
        took = time.monotonic() - t
        print("peer: received %.1f MB from %s in %.2f s, %.1f MB/s"
              % (n / 1048576, peer[0], took, n / 1048576 / took), flush=True)


threading.Thread(target=swallow, daemon=True).start()
serve()
