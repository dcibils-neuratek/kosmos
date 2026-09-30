# BearSSL, vendored

Upstream: <https://bearssl.org/> - release 0.6, `bearssl-0.6.tar.gz`.
Licence:  MIT. See `LICENSE.txt`.

Fetched from bearssl.org on 30 September 2026. The site publishes no hash
for it, so it was held to the one nixpkgs records for the same file
(`pkgs/by-name/be/bearssl/package.nix`, `057zhgy9...hvn1b7` in Nix's
base32), which is this SHA-256:

    6705bba1714961b41a728dfc5debbe348d2966c117649392f8c8139efc83ff14

`src/`, `inc/`, `tools/`, `LICENSE.txt` and `README.txt`, taken byte for
byte and checked so by `diff -r` against a fresh unpacking. **Unmodified**,
the rule every vendored thing here follows; no Kosmos copyright line is
added.

## Why BearSSL

**HTTPS for the browser** (`roadmap.md`, the browser: TLS) - Diego, 29 and
30 September: "can we reuse all these openssl, tls libraries for our browser
so it can access https?", then "Go for it". BearSSL was written for small
machines: no `malloc`, no system calls, an engine its caller feeds bytes
into and takes bytes out of - the shape of a TCP ring here. OpenSSL is half
a million lines built on sockets, threads, files and `/dev/urandom`. It
speaks TLS 1.0 to 1.2, not 1.3.

## How it is built

`src/` into every userland image, with `-w` as vendored code is, and the
system parts turned off by `-D` rather than by an edit: `BR_USE_URANDOM`,
`BR_USE_GETENTROPY`, `BR_USE_UNIX_TIME`, `BR_USE_WIN32_RAND`,
`BR_USE_WIN32_TIME` and `BR_RDRAND` are 0. Its randomness is injected from
the kernel's (`SYS_ENTROPY`), and a certificate's dates are held to the
machine's clock, by the TLS Kit (`user/kits/tls/`).

`tools/` is built for the Mac, as `brssl`, for one command: `brssl ta`,
which turns the roots in `assets/ca/cacert.pem` into the trust anchors the
kit is compiled with (`build/gen/tls_anchors.c`).

## What was left out

`samples/`, `test/`, `T0/`, `T0Comp.exe`, `build/`, `conf/`, `mk/`, the
`Makefile` and `Doxyfile` - the release's own build and its tests, which
the Makefile here replaces.
