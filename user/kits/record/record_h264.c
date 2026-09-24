/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * lieff's `minih264e`, compiled - and nothing else. A file of its own
 * because `minimp4` carries a copy of the same bitstream writer, and the
 * two implementations in one file are the same names defined twice.
 * `record_config.h` says how it is built.
 */

#include "record_config.h"

#define MINIH264_IMPLEMENTATION
#include "minih264e.h"
