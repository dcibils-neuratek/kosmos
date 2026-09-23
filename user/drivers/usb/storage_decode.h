/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_USB_STORAGE_DECODE_H
#define KOSMOS_DRIVERS_USB_STORAGE_DECODE_H

/*
 * What a USB stick is sent and what it answers, for the xHCI driver: Bulk-Only
 * Transport's two wrappers, SCSI's command blocks and the answers to them, and
 * whether a block holds a GUID partition table's header.
 *
 * Its own file, with no hardware and no system calls in it, for the reason
 * `usb_decode.c` is: QEMU's stick answers every command at once, in the format
 * it was asked for, and never needs recovering. A real one says it is not
 * ready at first, reports a unit attention after its reset, picks its own
 * sense format, and sends a status that is sometimes not one.
 * `tools/test_storagedecode.c` hands it those.
 */

#include <stdbool.h>
#include <stdint.h>

#define BOT_CBW_LENGTH          31u
#define BOT_CSW_LENGTH          13u

#define SCSI_TEST_UNIT_READY    0x00u
#define SCSI_REQUEST_SENSE      0x03u
#define SCSI_INQUIRY            0x12u
#define SCSI_READ_CAPACITY_10   0x25u
#define SCSI_READ_10            0x28u
#define SCSI_WRITE_10           0x2Au
#define SCSI_SYNCHRONIZE_CACHE_10 0x35u

#define SCSI_KEY_NOT_READY      0x2u
#define SCSI_KEY_ILLEGAL_REQUEST 0x5u
#define SCSI_KEY_UNIT_ATTENTION 0x6u

#define SCSI_SENSE_LENGTH       18u     /* fixed format, bytes 0 to 17 */
#define SCSI_CAPACITY_10_LENGTH 8u

#define GPT_HEADER_LEAST        92u

/*
 * **A command block wrapper** around `cdb` (Bulk-Only 1.0 5.1): its signature,
 * `tag`, the `length` of data expected, the direction, the LUN and the command
 * block. False, and `cbw` left alone, for a command block of no bytes or more
 * than sixteen, or a LUN past fifteen - the values the wrapper has no room for.
 */
bool bot_wrap(uint8_t *cbw, uint32_t tag, uint32_t length, bool in,
              uint8_t lun, const uint8_t *cdb, unsigned cdb_length);

/* What a command status wrapper said (5.2, 6.3). */
enum bot_status {
    BOT_PASSED,                 /* 00h */
    BOT_FAILED,                 /* 01h: REQUEST SENSE says why */
    BOT_PHASE_ERROR,            /* 02h: only a Reset Recovery answers it */
    BOT_NOT_VALID,              /* 6.3.1: its length, signature or tag */
    BOT_NOT_MEANINGFUL,         /* 6.3.2: a status past 02h, or a residue
                                 * larger than what was asked for */
};

/*
 * The `got` bytes that came back for the command sent with `tag`, which
 * expected `length` bytes of data. `residue` is what the stick said it did not
 * send or take, and 0 when the wrapper is not valid.
 */
enum bot_status bot_status_of(const uint8_t *csw, unsigned got, uint32_t tag,
                              uint32_t length, uint32_t *residue);

/*
 * Command blocks, written into `cdb` - which has room for sixteen bytes - and
 * their lengths returned. REQUEST SENSE asks for fixed-format sense data;
 * READ (10) and WRITE (10) move `blocks` at `lba`, in and out; SYNCHRONIZE
 * CACHE (10) asks for every block the stick holds to be written out of any
 * cache it keeps.
 */
unsigned scsi_test_unit_ready(uint8_t *cdb);
unsigned scsi_request_sense(uint8_t *cdb, uint8_t length);
unsigned scsi_inquiry(uint8_t *cdb, uint16_t length);
unsigned scsi_read_capacity_10(uint8_t *cdb);
unsigned scsi_read_10(uint8_t *cdb, uint32_t lba, uint16_t blocks);
unsigned scsi_write_10(uint8_t *cdb, uint32_t lba, uint16_t blocks);
unsigned scsi_synchronize_cache_10(uint8_t *cdb);

struct scsi_capacity {
    uint64_t blocks;            /* the last block's address, plus one */
    uint32_t block_size;        /* in bytes */
    bool     too_many;          /* FFFFFFFFh: more than READ CAPACITY (10)
                                 * can count, and `blocks` 0 */
};

/* READ CAPACITY (10)'s eight bytes. False when fewer came, or a block of no
 * bytes. */
bool scsi_capacity_10(const uint8_t *data, unsigned got,
                      struct scsi_capacity *out);

struct scsi_sense {
    uint8_t key;                /* 0h to Fh */
    uint8_t asc;                /* additional sense code */
    uint8_t ascq;               /* ...and its qualifier */
    bool    coded;              /* whether the two above were sent at all */
    bool    deferred;           /* about an earlier command than the last */
};

/* Sense data in either format - fixed, as asked for, or descriptor, which a
 * device may send anyway. False when it is neither, or too short to hold a
 * sense key. */
bool scsi_sense(const uint8_t *data, unsigned got, struct scsi_sense *out);

/* A sense key's name, as SPC gives it: "NOT READY", "UNIT ATTENTION". */
const char *scsi_sense_key_name(unsigned key);

/*
 * Whether sense data says the device does not do the command it was sent, as
 * it was sent: ILLEGAL REQUEST with 20h/00h, INVALID COMMAND OPERATION CODE,
 * or 24h/00h, INVALID FIELD IN CDB. A command block that does not change is
 * answered the same way every time, so this is an answer to remember rather
 * than a failure to report again - the ThinkPad's Kingston said 20h/00h to
 * every SYNCHRONIZE CACHE (10) it was sent (`usb.md` §7).
 */
bool scsi_not_supported(const struct scsi_sense *sense);

/*
 * CRC-32 as zlib computes it - reflected, polynomial EDB88320h - carried on
 * from `crc`, which is 0 to start.
 */
uint32_t storage_crc32(uint32_t crc, const uint8_t *bytes, unsigned n);

/*
 * Whether `block`, `size` bytes read from block `lba`, holds a GUID partition
 * table's header by the checks the UEFI specification gives: its signature, a
 * header size from 92 bytes to the block's, its CRC-32 over that size with the
 * field itself taken as zero, and MyLBA the block it was read from. The
 * partition entries' CRC is not checked here, because they are not in this
 * block. Those are the checks as `tools/mkusb_image.py` already writes to
 * them; a copy of the specification was not read for this.
 */
bool gpt_header_at(const uint8_t *block, unsigned size, uint64_t lba);

#endif
