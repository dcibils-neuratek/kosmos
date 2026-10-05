/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_BYTES_H
#define KOSMOS_BYTES_H

/*
 * A number written as bytes, read or written in the order a format says:
 * little-endian for gzip, GPT, a WAV and ChaCha20's words, big-endian for
 * PNG's chunks, a TrueType font's tables and an MP3's Xing header.
 *
 * A byte at a time, so it is right whatever order the machine keeps its own
 * words in and wherever the bytes sit - a chunk's length lands on any byte.
 * The compiler turns each into one load or store where the machine allows.
 *
 * These were written out again in each file that needed them - `be32` three
 * times, `le32` twice, a big-endian `put32` twice and a little-endian one
 * once - and this is the one copy (`CLAUDE.md`, *Kits, servers and drivers
 * supply*). The review before 0.11 found six more files with their own -
 * the FAT reader, the disk's format, the network stack, the Intel card,
 * a camera's and a MIDI device's decoders - and they read and write here
 * too: kfs's 64-bit fields and the card's descriptor addresses are what
 * `put_le64` is for.
 */

#include <stdint.h>

static inline uint16_t get_le16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] | (uint32_t)p[1] << 8);
}

static inline uint32_t get_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16
           | (uint32_t)p[3] << 24;
}

static inline uint64_t get_le64(const uint8_t *p)
{
    return (uint64_t)get_le32(p) | (uint64_t)get_le32(p + 4) << 32;
}

static inline uint16_t get_be16(const uint8_t *p)
{
    return (uint16_t)((uint32_t)p[0] << 8 | (uint32_t)p[1]);
}

static inline uint32_t get_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8
           | (uint32_t)p[3];
}

static inline void put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void put_le32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static inline void put_le64(uint8_t *p, uint64_t v)
{
    put_le32(p, (uint32_t)v);
    put_le32(p + 4, (uint32_t)(v >> 32));
}

static inline void put_be16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v >> 8);
    p[1] = (uint8_t)v;
}

static inline void put_be32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

#endif /* KOSMOS_BYTES_H */
