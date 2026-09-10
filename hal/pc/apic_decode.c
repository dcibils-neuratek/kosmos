/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* See `apic_decode.h`. */

#include <stdint.h>

#include "apic_decode.h"

#define APIC_BASE_EXTD  (1ULL << 10)    /* x2APIC mode */
#define APIC_BASE_EN    (1ULL << 11)    /* the APIC is on at all */

unsigned ioapic_inputs(uint32_t version)
{
    if (version == 0xffffffffu) {
        return 0;
    }

    return ((version >> 16) & 0xffu) + 1u;
}

enum lapic_mode lapic_mode_of(uint64_t apic_base)
{
    int enabled = (apic_base & APIC_BASE_EN) != 0;
    int x2 = (apic_base & APIC_BASE_EXTD) != 0;

    if (!enabled) {
        return x2 ? LAPIC_INVALID : LAPIC_DISABLED;
    }

    return x2 ? LAPIC_X2APIC : LAPIC_XAPIC;
}
