/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Where this board's randomness comes from: RDRAND, then virtio-rng.
 *
 * **The processor's own first**, because it is the one a real PC has - the
 * M700's Skylake does, and CPUID.1:ECX bit 30 says so - and it needs no
 * device at all. QEMU's default processor model offers none, so under QEMU
 * the harness gives the machine a virtio-rng device and that answers
 * instead. The same shape as `blk_bind.c`: the real thing asked first, the
 * emulated one when there is no real one.
 *
 * RDRAND is a generator the processor reseeds from its own noise source,
 * and Intel's guidance is to retry a few times when it says it has nothing
 * ready (the carry flag clear); ten, as that guidance has it. What comes out
 * is held to a health test by the kernel either way (`kernel/entropy.c`).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"
#include "cpu.h"
#include "rng.h"

#define CPUID1_ECX_RDRAND   (1u << 30)

static enum { NONE, RDRAND, VIRTIO } source;

static bool rdrand64(uint64_t *out)
{
    unsigned tries;

    for (tries = 0; tries < 10; tries++) {
        uint64_t v;
        uint8_t ok;

        __asm__ volatile ("rdrand %0; setc %1" : "=r"(v), "=qm"(ok) : : "cc");

        if (ok) {
            *out = v;
            return true;
        }
    }

    return false;
}

bool hal_entropy_init(void)
{
    struct cpu_info cpu;

    cpu_identify(&cpu);

    if ((cpu.feat1_ecx & CPUID1_ECX_RDRAND) != 0) {
        uint64_t probe;

        /* Asked once here: a processor that advertises it and never has a
         * value ready is no source at all. */
        if (rdrand64(&probe)) {
            source = RDRAND;
            return true;
        }
    }

    if (virtio_rng_init()) {
        source = VIRTIO;
        return true;
    }

    source = NONE;
    return false;
}

size_t hal_entropy(void *buf, size_t bytes)
{
    uint8_t *out = buf;
    size_t done = 0;

    if (source == VIRTIO) {
        return virtio_rng_read(buf, bytes);
    }

    if (source != RDRAND || buf == NULL) {
        return 0;
    }

    while (done < bytes) {
        uint64_t v;
        size_t take = (bytes - done < sizeof(v)) ? bytes - done : sizeof(v);

        if (!rdrand64(&v)) {
            break;
        }

        memcpy(out + done, &v, take);
        done += take;
    }

    return done;
}

const char *hal_entropy_describe(void)
{
    switch (source) {
    case RDRAND: return "RDRAND";
    case VIRTIO: return "virtio-rng";
    default:     return "no RDRAND on this processor and no virtio-rng device";
    }
}
