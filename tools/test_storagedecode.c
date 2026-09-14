/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * What a USB stick is sent and what it answers, asked the awkward questions on
 * the host.
 *
 * QEMU's stick answers every command at once, in the format it was asked for,
 * and its status is always a valid one. A real stick is not ready at first,
 * reports a unit attention after its reset, may send sense data in the format
 * it prefers, and can lose count. So those are built here a byte at a time.
 * The wrappers are laid out from Bulk-Only 1.0's Tables 5.1 and 5.2; the
 * command blocks and answers from Seagate's SCSI Commands Reference Manual,
 * Rev. J; QEMU's own answers from `hw/scsi/scsi-disk.c`. The two GPT headers
 * were written by Python's `zlib` and `struct`, as `tools/mkusb_image.py`
 * writes them, so the CRC they carry is not this project's C.
 *
 * Same split as `tools/test_usbdecode.c`, for the same reason.
 */

#include <stdio.h>
#include <string.h>

#include "../user/servers/storage_decode.h"

static int checks;
static int fails;

static void check(int ok, const char *what)
{
    if (ok) {
        checks++;
    } else {
        fails++;
        printf("  %s\n", what);
    }
}

static int same(const uint8_t *a, const uint8_t *b, unsigned n)
{
    return memcmp(a, b, n) == 0;
}

static void put32le(uint8_t *at, uint32_t value)
{
    at[0] = (uint8_t)value;
    at[1] = (uint8_t)(value >> 8);
    at[2] = (uint8_t)(value >> 16);
    at[3] = (uint8_t)(value >> 24);
}

/* A status wrapper, byte by byte as Table 5.2 lays it out. */
static void csw(uint8_t *b, uint32_t signature, uint32_t tag, uint32_t residue,
                uint8_t status)
{
    put32le(b, signature);
    put32le(b + 4, tag);
    put32le(b + 8, residue);
    b[12] = status;
}

/* A header's size set to `length`, and its CRC computed again with the field
 * zero - so a check on the size is not also a check on the CRC. */
static void reseal(uint8_t *block, uint32_t length)
{
    put32le(block + 12, length);
    put32le(block + 16, 0);
    put32le(block + 16, storage_crc32(0, block, length));
}

/* INQUIRY for 36 bytes with tag 12345678h, LUN 0, from Table 5.1 by hand. */
static const uint8_t inquiry_cbw[BOT_CBW_LENGTH] = {
    0x55, 0x53, 0x42, 0x43,             /* dCBWSignature, 43425355h */
    0x78, 0x56, 0x34, 0x12,             /* dCBWTag */
    0x24, 0x00, 0x00, 0x00,             /* dCBWDataTransferLength, 36 */
    0x80,                               /* bmCBWFlags: Data-In */
    0x00,                               /* bCBWLUN */
    0x06,                               /* bCBWCBLength */
    0x12, 0x00, 0x00, 0x00, 0x24, 0x00, /* INQUIRY, allocation length 36 */
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};

/* A primary header at block 1 of a 32768-block disk, its backup at 32767:
 * first usable 34, last usable 32734, a disk GUID of 00h to 0Fh, and 128
 * empty entries of 128 bytes. */
static const uint8_t gpt_primary[GPT_HEADER_LEAST] = {
    0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54, 0x00, 0x00, 0x01, 0x00,
    0x5c, 0x00, 0x00, 0x00, 0xe7, 0xdc, 0x4b, 0xb8, 0x00, 0x00, 0x00, 0x00,
    0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0xff, 0x7f, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xde, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x86, 0xd2, 0x54, 0xab,
};

static const uint8_t gpt_backup[GPT_HEADER_LEAST] = {
    0x45, 0x46, 0x49, 0x20, 0x50, 0x41, 0x52, 0x54, 0x00, 0x00, 0x01, 0x00,
    0x5c, 0x00, 0x00, 0x00, 0x59, 0x8d, 0xbd, 0x41, 0x00, 0x00, 0x00, 0x00,
    0xff, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xde, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03,
    0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a, 0x0b, 0x0c, 0x0d, 0x0e, 0x0f,
    0xdf, 0x7f, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x00, 0x00,
    0x80, 0x00, 0x00, 0x00, 0x86, 0xd2, 0x54, 0xab,
};

int main(void)
{
    uint8_t cdb[16], cbw[BOT_CBW_LENGTH], status[BOT_CSW_LENGTH];
    uint8_t untouched[BOT_CBW_LENGTH], block[512];
    struct scsi_capacity cap;
    struct scsi_sense sense;
    uint32_t residue;
    unsigned n, i;
    int all;

    /* ---- the command block wrapper (5.1) ---- */

    n = scsi_inquiry(cdb, 36);
    check(n == 6 && bot_wrap(cbw, 0x12345678u, 36, true, 0, cdb, n)
          && same(cbw, inquiry_cbw, BOT_CBW_LENGTH),
          "INQUIRY's wrapper is not the one Table 5.1 lays out");

    n = scsi_test_unit_ready(cdb);
    check(bot_wrap(cbw, 1, 0, false, 0, cdb, n) && cbw[8] == 0 && cbw[12] == 0
          && cbw[14] == 6,
          "TEST UNIT READY's wrapper does not expect no data, out, six bytes");

    n = scsi_read_10(cdb, 1, 1);
    check(bot_wrap(cbw, 2, 512, true, 3, cdb, n) && cbw[8] == 0x00
          && cbw[9] == 0x02 && cbw[12] == 0x80 && cbw[13] == 3 && cbw[14] == 10
          && same(cbw + 15, cdb, 10),
          "READ (10)'s wrapper does not carry 512 bytes in, LUN 3, and ten "
          "bytes of command");

    memset(cbw, 0xAA, sizeof(cbw));
    memset(untouched, 0xAA, sizeof(untouched));
    check(!bot_wrap(cbw, 3, 0, false, 0, cdb, 0)
          && !bot_wrap(cbw, 3, 0, false, 0, cdb, 17)
          && !bot_wrap(cbw, 3, 0, false, 16, cdb, 6)
          && same(cbw, untouched, BOT_CBW_LENGTH),
          "a command block of 0 or 17 bytes, or LUN 16, was wrapped, or the "
          "wrapper was written anyway");

    /* ---- command blocks ---- */

    n = scsi_test_unit_ready(cdb);
    check(n == 6 && same(cdb, (const uint8_t[6]){ 0 }, 6),
          "TEST UNIT READY is not six zero bytes (Table 202)");

    n = scsi_request_sense(cdb, 18);
    check(n == 6 && same(cdb, (const uint8_t[]){ 0x03, 0, 0, 0, 18, 0 }, 6),
          "REQUEST SENSE for 18 bytes of fixed-format data is not Table 164's");

    n = scsi_inquiry(cdb, 0x0124);
    check(n == 6 && same(cdb, (const uint8_t[]){ 0x12, 0, 0, 0x01, 0x24, 0 }, 6),
          "INQUIRY's allocation length is not bytes 3 and 4, big-endian");

    n = scsi_read_capacity_10(cdb);
    check(n == 10 && cdb[0] == 0x25
          && same(cdb + 1, (const uint8_t[9]){ 0 }, 9),
          "READ CAPACITY (10) is not 25h and nine zero bytes (Table 119)");

    n = scsi_read_10(cdb, 0x12345678u, 0x0102);
    check(n == 10 && same(cdb, (const uint8_t[]){ 0x28, 0, 0x12, 0x34, 0x56,
                                                 0x78, 0, 0x01, 0x02, 0 }, 10),
          "READ (10)'s block address is not bytes 2 to 5 and its length bytes "
          "7 and 8, big-endian (Table 97)");

    /* ---- the command status wrapper (5.2, 6.3) ---- */

    csw(status, 0x53425355u, 7, 0, 0x00);
    check(bot_status_of(status, 13, 7, 36, &residue) == BOT_PASSED
          && residue == 0,
          "QEMU's status for an INQUIRY it answered is not a pass");

    csw(status, 0x53425355u, 7, 28, 0x00);
    check(bot_status_of(status, 13, 7, 36, &residue) == BOT_PASSED
          && residue == 28,
          "a pass with 8 bytes of 36 sent is not a pass with a residue of 28");

    csw(status, 0x53425355u, 7, 8, 0x01);
    check(bot_status_of(status, 13, 7, 8, &residue) == BOT_FAILED,
          "a failed command with nothing sent is not a failure");

    csw(status, 0x53425355u, 7, 0xFFFFFFFFu, 0x02);
    check(bot_status_of(status, 13, 7, 0, &residue) == BOT_PHASE_ERROR,
          "a phase error is meaningful whatever its residue says (6.3.2)");

    csw(status, 0x53425355u, 7, 0, 0x00);
    check(bot_status_of(status, 12, 7, 36, &residue) == BOT_NOT_VALID,
          "twelve bytes were taken for a status wrapper");

    csw(status, 0x43425355u, 7, 0, 0x00);
    check(bot_status_of(status, 13, 7, 36, &residue) == BOT_NOT_VALID,
          "a command wrapper's signature was taken for a status wrapper's");

    csw(status, 0x53425355u, 8, 0, 0x00);
    check(bot_status_of(status, 13, 7, 36, &residue) == BOT_NOT_VALID
          && residue == 0,
          "the status for another command's tag was taken for this one's");

    csw(status, 0x53425355u, 7, 37, 0x00);
    check(bot_status_of(status, 13, 7, 36, &residue) == BOT_NOT_MEANINGFUL,
          "a residue larger than the length asked for was meaningful");

    csw(status, 0x53425355u, 7, 0, 0x03);
    check(bot_status_of(status, 13, 7, 36, &residue) == BOT_NOT_MEANINGFUL,
          "a status of 03h was meaningful");

    /* ---- READ CAPACITY (10) (Table 120) ---- */

    check(scsi_capacity_10((const uint8_t[]){ 0, 0, 0x7f, 0xff, 0, 0, 0x02, 0 },
                           8, &cap)
          && cap.blocks == 32768 && cap.block_size == 512 && !cap.too_many,
          "QEMU's answer for a 16 MB disk is not 32768 blocks of 512 bytes");

    check(scsi_capacity_10((const uint8_t[]){ 0, 0, 0xff, 0xff, 0, 0, 0x10, 0 },
                           8, &cap)
          && cap.blocks == 65536 && cap.block_size == 4096,
          "a disk of 4096-byte blocks is not read as one");

    check(scsi_capacity_10((const uint8_t[]){ 0xff, 0xff, 0xff, 0xfe, 0, 0, 2,
                                              0 }, 8, &cap)
          && cap.blocks == 0xFFFFFFFFull && !cap.too_many,
          "a last block of FFFFFFFEh did not count 4294967295 blocks: the "
          "count was taken in 32 bits");

    check(scsi_capacity_10((const uint8_t[]){ 0xff, 0xff, 0xff, 0xff, 0, 0, 2,
                                              0 }, 8, &cap)
          && cap.too_many && cap.blocks == 0,
          "FFFFFFFFh was not taken for more blocks than READ CAPACITY (10) "
          "counts");

    check(!scsi_capacity_10((const uint8_t[]){ 0, 0, 0x7f, 0xff, 0, 0, 2, 0 },
                            7, &cap),
          "seven bytes of capacity were read as eight");

    check(!scsi_capacity_10((const uint8_t[]){ 0, 0, 0x7f, 0xff, 0, 0, 0, 0 },
                            8, &cap),
          "a block of no bytes was taken");

    /* ---- sense data (Tables 27 and 28) ---- */

    {
        uint8_t fixed[SCSI_SENSE_LENGTH];

        /* QEMU's REQUEST SENSE with nothing to report: fixed, NO SENSE. */
        memset(fixed, 0, sizeof(fixed));
        fixed[0] = 0x70;
        fixed[7] = 10;
        check(scsi_sense(fixed, 18, &sense) && sense.key == 0 && sense.coded
              && sense.asc == 0 && sense.ascq == 0 && !sense.deferred,
              "QEMU's sense with nothing to report is not NO SENSE, 00h/00h");

        /* A unit attention after a reset: 06h, 29h/00h. */
        fixed[2] = 0x06;
        fixed[12] = 0x29;
        check(scsi_sense(fixed, 18, &sense) && sense.key == 6 && sense.coded
              && sense.asc == 0x29 && sense.ascq == 0,
              "a unit attention for a reset is not key 6h, 29h/00h");

        /* VALID set, and the three bits above the key. */
        fixed[0] = 0xF0;
        fixed[2] = 0xE3;
        fixed[12] = 0x11;
        check(scsi_sense(fixed, 18, &sense) && sense.key == 3
              && sense.asc == 0x11,
              "the VALID bit or FILEMARK, EOM and ILI were read into the format "
              "or the key");

        fixed[0] = 0x71;
        check(scsi_sense(fixed, 18, &sense) && sense.deferred,
              "71h is not about an earlier command");

        fixed[0] = 0x70;
        check(scsi_sense(fixed, 13, &sense) && sense.key == 3 && !sense.coded,
              "thirteen bytes of fixed sense were read to byte 13");

        fixed[7] = 5;
        check(scsi_sense(fixed, 18, &sense) && !sense.coded,
              "an additional length of 5 was read as reaching byte 13");

        check(!scsi_sense(fixed, 2, &sense),
              "two bytes of fixed sense were taken as holding a key");

        fixed[0] = 0x00;
        check(!scsi_sense(fixed, 18, &sense),
              "a response code of 00h was taken for sense data");
    }

    check(scsi_sense((const uint8_t[]){ 0x72, 0x02, 0x3a, 0x00, 0, 0, 0, 0 }, 8,
                     &sense)
          && sense.key == 2 && sense.asc == 0x3a && sense.ascq == 0
          && sense.coded && !sense.deferred,
          "descriptor-format NOT READY, medium not present, is not key 2h, "
          "3Ah/00h");

    check(scsi_sense((const uint8_t[]){ 0x73, 0x04, 0x44, 0x00 }, 4, &sense)
          && sense.deferred && sense.key == 4,
          "73h is not a deferred descriptor-format sense");

    check(!scsi_sense((const uint8_t[]){ 0x72, 0x02, 0x3a }, 3, &sense),
          "three bytes of descriptor sense were read to byte 3");

    check(strcmp(scsi_sense_key_name(2), "NOT READY") == 0
          && strcmp(scsi_sense_key_name(6), "UNIT ATTENTION") == 0
          && strcmp(scsi_sense_key_name(15), "COMPLETED") == 0
          && strcmp(scsi_sense_key_name(12), "a reserved sense key") == 0,
          "a sense key's name is not Table 28's");

    /* ---- CRC-32 and a GPT header ---- */

    check(storage_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u,
          "CRC-32 of \"123456789\" is not CBF43926h, its check value");

    check(storage_crc32(storage_crc32(0, (const uint8_t *)"1234", 4),
                        (const uint8_t *)"56789", 5) == 0xCBF43926u,
          "CRC-32 carried across two calls is not the CRC of both");

    memset(block, 0, sizeof(block));
    memcpy(block, gpt_primary, sizeof(gpt_primary));
    check(gpt_header_at(block, 512, 1),
          "the primary header zlib wrote is not a header at block 1");

    check(!gpt_header_at(block, 512, 32767),
          "the primary header was taken for the one at block 32767: MyLBA was "
          "not checked");

    memset(block, 0, sizeof(block));
    memcpy(block, gpt_backup, sizeof(gpt_backup));
    check(gpt_header_at(block, 512, 32767) && !gpt_header_at(block, 512, 1),
          "the backup header is not a header at block 32767, and only there");

    block[40] ^= 1;
    check(!gpt_header_at(block, 512, 32767),
          "a header with its first usable block changed passed its CRC");

    memset(block, 0, sizeof(block));
    memcpy(block, gpt_primary, sizeof(gpt_primary));
    reseal(block, 512);
    check(gpt_header_at(block, 512, 1),
          "a header whose size is the whole block was refused: the size is "
          "the header's to say, from 92 up");

    reseal(block, 91);
    check(!gpt_header_at(block, 512, 1),
          "a header of 91 bytes was taken, with a CRC over those 91");

    put32le(block + 12, 513);
    check(!gpt_header_at(block, 512, 1),
          "a header larger than its block was taken");

    reseal(block, 92);
    block[0] = 'e';
    reseal(block, 92);
    check(!gpt_header_at(block, 512, 1),
          "\"eFI PART\" was taken for the signature");

    memset(block, 0, sizeof(block));
    check(!gpt_header_at(block, 512, 0) && !gpt_header_at(block, 91, 1),
          "an empty block, or 91 bytes, was taken for a header");

    all = 1;

    for (i = 0; i < 16; i++) {
        all = all && scsi_sense_key_name(i) != NULL;
    }

    check(all && scsi_sense_key_name(0x1F) == scsi_sense_key_name(0x0F),
          "a sense key's name is missing, or a key past Fh is not its low "
          "four bits");

    if (fails == 0) {
        printf("PASS: %d checks on what a stick is sent and answers (Bulk-Only "
               "wrappers, SCSI command blocks, capacity, sense in both formats, "
               "and a GPT header's CRC and place).\n", checks);
        return 0;
    }

    printf("FAIL: %d of %d checks on what a stick is sent and answers.\n",
           fails, checks + fails);
    return 1;
}
