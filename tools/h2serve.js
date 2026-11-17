/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * An HTTPS server speaking HTTP/2, for `run_curl.py`'s HTTP/2 check: Node's
 * own `http2`, since Python's standard library has none. HTTP/1.1 is still
 * allowed, so a client without HTTP/2 is answered rather than refused - and
 * says "1.1", which is what the check looks for.
 *
 *   node h2serve.js CERT KEY PAGE      the port it listens on, on a line
 *   node h2serve.js CERT KEY PAGE h1   the same over HTTP/1.1 alone: the
 *                                      control's server
 *
 * Every path is the page: what is checked is how it came, not what it was.
 */

"use strict";

const fs = require("fs");
const http2 = require("http2");

const [cert, key, page, only] = process.argv.slice(2);
const body = fs.readFileSync(page);
const options = { cert: fs.readFileSync(cert), key: fs.readFileSync(key), allowHTTP1: true };

const server = only === "h1" ? require("https").createServer(options)
                             : http2.createSecureServer(options);

server.on("request", (req, res) => {
    res.writeHead(200, { "content-type": "text/plain", "content-length": body.length });
    res.end(body);
});

server.listen(0, "127.0.0.1", () => {
    process.stdout.write(server.address().port + "\n");
});
