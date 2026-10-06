/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/* The fixed-width integers of a 64-bit LP64 machine, for TinyCC: GCC supplies
 * this to Kosmos's own build, and TinyCC does not carry one (`docs/tinycc.md`). */
#ifndef KOSMOS_TCC_STDINT_H
#define KOSMOS_TCC_STDINT_H
typedef signed char int8_t; typedef unsigned char uint8_t;
typedef short int16_t; typedef unsigned short uint16_t;
typedef int int32_t; typedef unsigned int uint32_t;
typedef long int64_t; typedef unsigned long uint64_t;
typedef long intptr_t; typedef unsigned long uintptr_t;
typedef long intmax_t; typedef unsigned long uintmax_t;
#define INT64_MAX 9223372036854775807L
#define UINT64_MAX 18446744073709551615UL
#define SIZE_MAX UINT64_MAX
#endif
