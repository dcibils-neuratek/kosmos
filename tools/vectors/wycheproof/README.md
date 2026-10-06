# Wycheproof's test vectors

Project Wycheproof's vectors (<https://github.com/C2SP/wycheproof>), as
shipped, at commit `e0df04e0c033f2d25c5051dd06230336c7822358`
(`testvectors_v1/`), under the Apache License 2.0 in `LICENSE` beside them.
Not modified: `tools/wycheproof2c.py` turns a file into a C header during
the build, and that is the only thing done to it.

| File | SHA-256 | Held to it |
|---|---|---|
| `aes_ccm_test.json` | `e713a981df1f261098245f4a1031a34a611df93e0d83f4a6a2c1a13e9ba62d7b` | `crypto_aes_ccm_seal`/`_open`, in `tools/test_crypto.c` (`docs/keyring.md`, K1) |
