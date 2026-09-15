/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a stick is sent and what it answers, laid out and read. `storage_decode.h`
 * says why this is a file of its own.
 *
 * The wrappers are Bulk-Only 1.0's Tables 5.1 and 5.2, little-endian. The
 * command blocks and their answers are SCSI's, big-endian, as Seagate's SCSI
 * Commands Reference Manual, Rev. J, gives them: INQUIRY Table 58, READ (10)
 * Table 97, READ CAPACITY (10) Tables 119 and 120, REQUEST SENSE Table 164,
 * SYNCHRONIZE CACHE (10) Table 199, TEST UNIT READY Table 202, WRITE (10)
 * Table 216, fixed-format sense data Table 27, and the sense keys Table 28.
 */

#include <string.h>

#include "storage_decode.h"

#define CBW_SIGNATURE   0x43425355u
#define CSW_SIGNATURE   0x53425355u
#define CBW_DATA_IN     0x80u           /* bmCBWFlags, bit 7 */
#define CBW_BLOCK       15u             /* where CBWCB begins */

static void put32le(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

static uint32_t get32le(const uint8_t *at)
{
    return at[0] | (uint32_t)at[1] << 8 | (uint32_t)at[2] << 16
         | (uint32_t)at[3] << 24;
}

static uint64_t get64le(const uint8_t *at)
{
    return get32le(at) | (uint64_t)get32le(at + 4) << 32;
}

static uint32_t get32be(const uint8_t *at)
{
    return (uint32_t)at[0] << 24 | (uint32_t)at[1] << 16
         | (uint32_t)at[2] << 8 | at[3];
}

bool bot_wrap(uint8_t *cbw, uint32_t tag, uint32_t length, bool in,
              uint8_t lun, const uint8_t *cdb, unsigned cdb_length)
{
    if (cdb_length < 1u || cdb_length > 16u || lun > 15u) {
        return false;
    }

    memset(cbw, 0, BOT_CBW_LENGTH);
    put32le(cbw, CBW_SIGNATURE);
    put32le(cbw + 4, tag);
    put32le(cbw + 8, length);
    cbw[12] = in ? CBW_DATA_IN : 0u;    /* ignored when the length is 0 */
    cbw[13] = lun;
    cbw[14] = (uint8_t)cdb_length;
    memcpy(cbw + CBW_BLOCK, cdb, cdb_length);
    return true;
}

/*
 * **Valid, then meaningful** (6.3): thirteen bytes, the signature and the tag
 * this command went out with, or the wrapper is not one; then a status of 00h
 * or 01h with a residue no larger than the length, or 02h with any residue at
 * all, because a phase error is the stick saying it lost count.
 */
enum bot_status bot_status_of(const uint8_t *csw, unsigned got, uint32_t tag,
                              uint32_t length, uint32_t *residue)
{
    *residue = 0;

    if (got != BOT_CSW_LENGTH || get32le(csw) != CSW_SIGNATURE
        || get32le(csw + 4) != tag) {
        return BOT_NOT_VALID;
    }

    *residue = get32le(csw + 8);

    switch (csw[12]) {
    case 0x00u:
        return *residue <= length ? BOT_PASSED : BOT_NOT_MEANINGFUL;
    case 0x01u:
        return *residue <= length ? BOT_FAILED : BOT_NOT_MEANINGFUL;
    case 0x02u:
        return BOT_PHASE_ERROR;
    default:
        return BOT_NOT_MEANINGFUL;
    }
}

unsigned scsi_test_unit_ready(uint8_t *cdb)
{
    memset(cdb, 0, 6u);
    cdb[0] = SCSI_TEST_UNIT_READY;
    return 6u;
}

/* DESC, byte 1 bit 0, left clear: fixed-format sense data. */
unsigned scsi_request_sense(uint8_t *cdb, uint8_t length)
{
    memset(cdb, 0, 6u);
    cdb[0] = SCSI_REQUEST_SENSE;
    cdb[4] = length;
    return 6u;
}

/* The allocation length is bytes 3 and 4. */
unsigned scsi_inquiry(uint8_t *cdb, uint16_t length)
{
    memset(cdb, 0, 6u);
    cdb[0] = SCSI_INQUIRY;
    cdb[3] = (uint8_t)(length >> 8);
    cdb[4] = (uint8_t)length;
    return 6u;
}

/* The obsolete LOGICAL BLOCK ADDRESS and PMI left zero: the last block. */
unsigned scsi_read_capacity_10(uint8_t *cdb)
{
    memset(cdb, 0, 10u);
    cdb[0] = SCSI_READ_CAPACITY_10;
    return 10u;
}

unsigned scsi_read_10(uint8_t *cdb, uint32_t lba, uint16_t blocks)
{
    memset(cdb, 0, 10u);
    cdb[0] = SCSI_READ_10;
    cdb[2] = (uint8_t)(lba >> 24);
    cdb[3] = (uint8_t)(lba >> 16);
    cdb[4] = (uint8_t)(lba >> 8);
    cdb[5] = (uint8_t)lba;
    cdb[7] = (uint8_t)(blocks >> 8);
    cdb[8] = (uint8_t)blocks;
    return 10u;
}

/* READ (10)'s layout, with the data going out: the address in bytes 2 to 5
 * and the length in bytes 7 and 8. */
unsigned scsi_write_10(uint8_t *cdb, uint32_t lba, uint16_t blocks)
{
    (void)scsi_read_10(cdb, lba, blocks);
    cdb[0] = SCSI_WRITE_10;
    return 10u;
}

/*
 * **Every block, out of the cache.** Block 0 and a NUMBER OF BLOCKS of 0, which
 * means from there to the last; IMMED clear, so the status comes only once
 * the stick has done it - which is the whole point of asking.
 */
unsigned scsi_synchronize_cache_10(uint8_t *cdb)
{
    memset(cdb, 0, 10u);
    cdb[0] = SCSI_SYNCHRONIZE_CACHE_10;
    return 10u;
}

bool scsi_capacity_10(const uint8_t *data, unsigned got,
                      struct scsi_capacity *out)
{
    uint32_t last;

    memset(out, 0, sizeof(*out));

    if (got < SCSI_CAPACITY_10_LENGTH) {
        return false;
    }

    last = get32be(data);
    out->block_size = get32be(data + 4);

    if (out->block_size == 0) {
        return false;
    }

    if (last == 0xFFFFFFFFu) {
        out->too_many = true;
        return true;
    }

    out->blocks = (uint64_t)last + 1u;
    return true;
}

/*
 * **Either format.** Fixed is 70h or 71h in the response code - bit 7 is
 * VALID, about the INFORMATION field, and says nothing about the format - with
 * the sense key in byte 2, and the additional sense code and its qualifier in
 * bytes 12 and 13 only when the additional length in byte 7 reaches them.
 * Descriptor is 72h or 73h, with all three in bytes 1 to 3. 71h and 73h are
 * about a command before the last.
 */
bool scsi_sense(const uint8_t *data, unsigned got, struct scsi_sense *out)
{
    unsigned code;

    memset(out, 0, sizeof(*out));

    if (got < 1u) {
        return false;
    }

    code = data[0] & 0x7Fu;

    if (code == 0x70u || code == 0x71u) {
        if (got < 3u) {
            return false;
        }

        out->key = data[2] & 0x0Fu;
        out->deferred = code == 0x71u;

        if (got >= 14u && data[7] >= 6u) {
            out->asc = data[12];
            out->ascq = data[13];
            out->coded = true;
        }

        return true;
    }

    if (code == 0x72u || code == 0x73u) {
        if (got < 4u) {
            return false;
        }

        out->key = data[1] & 0x0Fu;
        out->asc = data[2];
        out->ascq = data[3];
        out->coded = true;
        out->deferred = code == 0x73u;
        return true;
    }

    return false;
}

const char *scsi_sense_key_name(unsigned key)
{
    static const char *const names[16] = {
        "NO SENSE", "RECOVERED ERROR", "NOT READY", "MEDIUM ERROR",
        "HARDWARE ERROR", "ILLEGAL REQUEST", "UNIT ATTENTION", "DATA PROTECT",
        "BLANK CHECK", "VENDOR SPECIFIC", "COPY ABORTED", "ABORTED COMMAND",
        "a reserved sense key", "VOLUME OVERFLOW", "MISCOMPARE", "COMPLETED",
    };

    return names[key & 0x0Fu];
}

/*
 * The two codes by the names SPC gives them; a copy of SPC was not read for
 * this. What holds them is that the ThinkPad's stick sent one and QEMU's sends
 * the other, and `test_storagedecode` checks both. With any other qualifier
 * the same two codes name other conditions, and every other ILLEGAL REQUEST
 * names something in the request - a block out of range - that the next
 * request need not repeat.
 */
bool scsi_not_supported(const struct scsi_sense *sense)
{
    return sense->coded && sense->key == SCSI_KEY_ILLEGAL_REQUEST
           && sense->ascq == 0x00u
           && (sense->asc == 0x20u || sense->asc == 0x24u);
}

uint32_t storage_crc32(uint32_t crc, const uint8_t *bytes, unsigned n)
{
    unsigned i, bit;

    crc = ~crc;

    for (i = 0; i < n; i++) {
        crc ^= bytes[i];

        for (bit = 0; bit < 8u; bit++) {
            crc = (crc & 1u) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
        }
    }

    return ~crc;
}

/*
 * The CRC is over the header's own size, with its field counted as four zero
 * bytes - carried across the field rather than written into the block, which
 * is the caller's.
 */
bool gpt_header_at(const uint8_t *block, unsigned size, uint64_t lba)
{
    static const uint8_t zero[4];
    uint32_t length, crc;

    if (size < GPT_HEADER_LEAST || memcmp(block, "EFI PART", 8u) != 0) {
        return false;
    }

    length = get32le(block + 12);

    if (length < GPT_HEADER_LEAST || length > size) {
        return false;
    }

    crc = storage_crc32(0, block, 16u);
    crc = storage_crc32(crc, zero, 4u);
    crc = storage_crc32(crc, block + 20, length - 20u);

    return crc == get32le(block + 16) && get64le(block + 24) == lba;
}
