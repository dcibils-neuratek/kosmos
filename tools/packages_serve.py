#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Serves this build's packages to a Kosmos machine, over HTTP.

    python3 tools/packages_serve.py [port]       build/packages, on 47701

What `install` on the machine asks (`tools/packages.py` makes the folder;
`roadmap.md`, Kosmos App Store): `index.txt`, then a package's zip. Only that
folder, and only reading - no listing of anything else on this Mac, and no
uploads. No password and no sudo: a port above 1024, on the home network
the netboot is on.
"""

import functools
import http.server
import os
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FOLDER = os.path.join(ROOT, "build", "packages")


class Packages(http.server.SimpleHTTPRequestHandler):
    def list_directory(self, path):
        self.send_error(404, "the index is index.txt")
        return None

    def log_message(self, fmt, *args):
        sys.stdout.write("packages: %s %s\n" % (self.address_string(), fmt % args))
        sys.stdout.flush()


def main():
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 47701
    os.makedirs(FOLDER, exist_ok=True)
    handler = functools.partial(Packages, directory=FOLDER)
    server = http.server.ThreadingHTTPServer(("0.0.0.0", port), handler)
    print("packages: serving %s on port %d" % (FOLDER, port), flush=True)
    server.serve_forever()


if __name__ == "__main__":
    main()
