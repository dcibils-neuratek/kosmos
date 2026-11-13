/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * curl's TLS settings for Kosmos (docs/w6.md W6b).
 *
 * Nothing is set yet, so curl keeps its own defaults - OpenSSL's ciphers,
 * and the certificate bundle curl was built to look for. W6 loads pages
 * from a folder and never opens a connection; the roots Kosmos trusts
 * are handed to curl here when JetSki first fetches a page (roadmap, W7).
 */

#include "config.h"
#include "CurlSSLHandle.h"

namespace WebCore {

void CurlSSLHandle::platformInitialize()
{
}

} // namespace WebCore
