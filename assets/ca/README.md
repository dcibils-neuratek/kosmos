# The roots this machine trusts

`cacert.pem`: Mozilla's root certificates, as curl publishes them converted
to PEM - 121 of them, "Certificate data from Mozilla as of: Fri Sep 25
03:12:01 2026 GMT". Fetched from <https://curl.se/ca/cacert.pem> on 30
September 2026 and held to the hash curl publishes beside it
(`cacert.pem.sha256`):

    a41b5d356aea97a529fe27e0f7316d2f9d946d75927476cf9cf1b90637d00505

**Unmodified.** Licence: the Mozilla Public License 2.0 - curl's page says
"The PEM file is only a converted version of the original one and thus it
is licensed under the same license as the Mozilla source file: MPL 2.0"
(<https://curl.se/docs/caextract.html>).

Turned into BearSSL's trust anchors when the image is built, by BearSSL's own
`brssl ta` (`runtime/upstream/bearssl/README.kosmos.md`), for the TLS Kit
(`user/kits/tls/`). Updating it is replacing this file with a newer one and
its hash here.
