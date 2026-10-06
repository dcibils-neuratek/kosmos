/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_CLOCK_EPOCH_H
#define KOSMOS_CLOCK_EPOCH_H

#include <stdint.h>

/* The date now, in seconds since 1970, from `/Devices/clock` through the
 * devices server's endpoint; 0 when there is none to ask or no clock. A
 * server that dates things - the disk's writes, the keyring's entries -
 * asks here, so there is one way to. */
uint64_t clock_epoch(long devices);

#endif /* KOSMOS_CLOCK_EPOCH_H */
