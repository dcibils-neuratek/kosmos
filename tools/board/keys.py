#!/usr/bin/env python3
#  Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
"""Keys for the Kosmos Board's API: one a device, one for Claude.

    keys.py make NAME --to FILE         a key into FILE (made private), never printed
    keys.py make NAME --show            a key printed once, for a person to paste
    keys.py revoke NAME                 every key of that name withdrawn
    keys.py list                        names, when made, when last used

`--local` (and `--persist-to DIR`) works on the copy `wrangler dev` uses;
without it, on the database on Cloudflare. Only a key's SHA-256 goes into
the database: the key itself exists in the one place it is given to, and
a lost key is withdrawn and made again, never recovered.
"""

import argparse
import hashlib
import os
import secrets
import subprocess
import sys
from datetime import datetime, timezone

HERE = os.path.dirname(os.path.abspath(__file__))


def d1(args, sql):
    """One statement on the board's database, through wrangler; its output."""
    cmd = ["wrangler", "d1", "execute", "kosmos-board", "--command", sql]
    cmd.append("--local" if args.local else "--remote")
    if args.persist_to:
        cmd += ["--persist-to", args.persist_to]
    out = subprocess.run(cmd, cwd=HERE, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("keys: wrangler refused: " + (out.stderr or out.stdout)[-800:])
    return out.stdout


def quoted(s):
    return "'" + s.replace("'", "''") + "'"


def make(args):
    key = "kb_" + secrets.token_hex(20)
    digest = hashlib.sha256(key.encode()).hexdigest()
    at = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
    d1(args, "INSERT INTO keys (name, hash, created) VALUES (%s, %s, %s)" % (quoted(args.name), quoted(digest), quoted(at)))

    if args.to:
        path = os.path.expanduser(args.to)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, "w") as f:
            f.write(key + "\n")
        print("keys: a key named %s, kept in %s" % (args.name, path))
    else:
        print("keys: a key named %s - shown once, paste it where it is used:" % args.name)
        print(key)


def main():
    p = argparse.ArgumentParser(description="Keys for the Kosmos Board's API.")
    p.add_argument("--local", action="store_true", help="the local copy wrangler dev uses")
    p.add_argument("--persist-to", help="where that local copy is kept")
    sub = p.add_subparsers(dest="verb", required=True)
    m = sub.add_parser("make")
    m.add_argument("name")
    where = m.add_mutually_exclusive_group(required=True)
    where.add_argument("--to", help="a file to keep it in, made readable by its owner only")
    where.add_argument("--show", action="store_true", help="print it once")
    r = sub.add_parser("revoke")
    r.add_argument("name")
    sub.add_parser("list")
    args = p.parse_args()

    if args.verb == "make":
        make(args)
    elif args.verb == "revoke":
        at = datetime.now(timezone.utc).isoformat(timespec="seconds").replace("+00:00", "Z")
        d1(args, "UPDATE keys SET revoked = %s WHERE name = %s AND revoked IS NULL" % (quoted(at), quoted(args.name)))
        print("keys: every key named %s withdrawn" % args.name)
    else:
        print(d1(args, "SELECT name, created, last_used, revoked FROM keys ORDER BY created"))


if __name__ == "__main__":
    main()
