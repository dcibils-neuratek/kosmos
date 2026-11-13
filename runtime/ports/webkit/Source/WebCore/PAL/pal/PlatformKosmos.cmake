# Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE.
#
# **PAL on Kosmos** (`docs/w6.md` W6b): the platform abstraction under
# WebCore, as PlayStation's port has it - digests through OpenSSL, the
# generic clock, and the kill ring editing keeps.

list(APPEND PAL_SOURCES
    crypto/openssl/CryptoDigestOpenSSL.cpp

    system/ClockGeneric.cpp
    system/Sound.cpp

    text/KillRing.cpp
)

list(APPEND PAL_LIBRARIES OpenSSL::Crypto)
