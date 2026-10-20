/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_DRIVERS_BLUETOOTH_HCI_DECODE_H
#define KOSMOS_DRIVERS_BLUETOOTH_HCI_DECODE_H

/*
 * **HCI, the language a host speaks to a Bluetooth controller**
 * (`bluetooth.md` B1): commands built, events put back together from the
 * pieces a transport hands over, and what the first four commands answer -
 * who made the controller, which Bluetooth it speaks, its address, and what
 * it can do.
 *
 * The section numbers are the Bluetooth Core specification's, Volume 4
 * Part E: 5.4 for the packets, 7 for the commands and events. Names of
 * versions and makers are the Assigned Numbers document's.
 *
 * It knows nothing of USB or of a UART, because the controller does not:
 * `xhci.c` uses it for B1 and the host stack takes it at B2. No hardware
 * and no system calls in it, so `tools/test_btdecode.c` holds it on the
 * Mac.
 */

#include <stdbool.h>
#include <stdint.h>

/* Opcodes: OGF in 15:10, OCF in 9:0 (5.4.1). */
#define HCI_RESET                   0x0C03u   /* 7.3.2 */
#define HCI_READ_LOCAL_VERSION      0x1001u   /* 7.4.1 */
#define HCI_READ_LOCAL_FEATURES     0x1003u   /* 7.4.3 */
#define HCI_READ_BUFFER_SIZE        0x1005u   /* 7.4.5 */
#define HCI_READ_BD_ADDR            0x1009u   /* 7.4.6 */

/* Intel's own (OGF 3Fh), from Linux's `btintel.c`: what the chip is and
 * whether it is running its bootloader or its firmware. */
#define HCI_INTEL_READ_VERSION      0xFC05u

/* Events (7.7). */
#define HCI_EVENT_COMMAND_COMPLETE  0x0Eu     /* 7.7.14 */
#define HCI_EVENT_COMMAND_STATUS    0x0Fu     /* 7.7.15 */

/* An event is a code, a length, and up to 255 bytes of parameters (5.4.4). */
#define HCI_EVENT_MAX               257u

/* Feature bits (Volume 2 Part C 3.3), numbered as the specification does. */
#define HCI_FEATURE_BREDR_NOT_SUPPORTED  37u
#define HCI_FEATURE_LE                   38u
#define HCI_FEATURE_SSP                  51u

/*
 * A command packet, 5.4.1: the opcode, the parameters' length, and the
 * parameters, into `out`. The bytes written, or 0 when `room` is too small
 * or there are more than 255 parameter bytes.
 */
unsigned hci_command(uint8_t *out, unsigned room, uint16_t opcode,
                     const uint8_t *params, unsigned length);

/*
 * **An event put back together.** A transport may cut one event across
 * several of its packets - USB's interrupt endpoint is 16 bytes and an
 * event up to 257 - and may carry the end of one and the start of the next
 * in the same packet, so the header's length is what decides, as Linux's
 * `btusb.c` reassembles. Start with `have` at 0.
 */
struct hci_assembler {
    uint8_t  bytes[HCI_EVENT_MAX];
    unsigned have;
};

/*
 * Takes bytes from `in[0 .. length)` until one event is whole, and says how
 * many it took in `*used`. True when `a->bytes` holds a whole event, which
 * is then `a->have` bytes long and is let go of by the next call; false
 * when it took every byte and wants more. Call again with what is left
 * while `*used` is less than `length`.
 */
bool hci_assemble(struct hci_assembler *a, const uint8_t *in, unsigned length,
                  unsigned *used);

/* An event's code and parameters, pointing into the bytes given. */
struct hci_event {
    uint8_t        code;
    uint8_t        length;
    const uint8_t *params;
};

/* False if `bytes` is shorter than its header says. */
bool hci_decode_event(const uint8_t *bytes, unsigned length,
                      struct hci_event *out);

/*
 * **A command's answer**: Command Complete (7.7.14) or Command Status
 * (7.7.15), which are the same answer in two shapes. `ret` points at the
 * return parameters after the status, which for Command Status there are
 * none of.
 */
struct hci_answer {
    bool           complete;    /* Command Complete; false: Command Status */
    uint8_t        commands;    /* Num_HCI_Command_Packets: how many may be sent */
    uint16_t       opcode;      /* what it answers; 0 for a "you may send" only */
    uint8_t        status;      /* 0 is success (Volume 1 Part F) */
    const uint8_t *ret;
    unsigned       ret_length;
};

/* False for any other event, or one too short to be either. */
bool hci_decode_answer(const struct hci_event *e, struct hci_answer *out);

/* Read Local Version Information's return parameters (7.4.1). */
struct hci_version {
    uint8_t  hci_version;       /* the Bluetooth this controller speaks */
    uint16_t hci_subversion;
    uint8_t  lmp_version;       /* and its link manager */
    uint16_t manufacturer;      /* a company identifier */
    uint16_t lmp_subversion;
};

/* Each false when the answer is not a successful one to its command, or is
 * shorter than that command's return parameters. */
bool hci_decode_version(const struct hci_answer *a, struct hci_version *out);
bool hci_decode_address(const struct hci_answer *a, uint8_t address[6]);
bool hci_decode_features(const struct hci_answer *a, uint8_t features[8]);

/* Read Buffer Size's return parameters (7.4.5): how long an ACL packet the
 * controller takes, and how many it holds before the host must wait for
 * Number Of Completed Packets. */
struct hci_buffers {
    uint16_t acl_length;
    uint8_t  sco_length;
    uint16_t acl_packets;
    uint16_t sco_packets;
};

bool hci_decode_buffers(const struct hci_answer *a, struct hci_buffers *out);

/*
 * Intel Read Version's answer in its older, fixed form (Linux's `struct
 * intel_version`): nine bytes after the status. Newer chips answer in
 * type-length-value form instead, which this refuses - the stack logs the
 * raw bytes either way.
 */
#define HCI_INTEL_BOOTLOADER   0x06u     /* fw_variant: waiting for firmware */
#define HCI_INTEL_OPERATIONAL  0x23u     /* fw_variant: firmware running */

struct hci_intel_version {
    uint8_t hw_platform;
    uint8_t hw_variant;
    uint8_t hw_revision;
    uint8_t fw_variant;
    uint8_t fw_revision;
    uint8_t fw_build_num;
    uint8_t fw_build_ww;
    uint8_t fw_build_yy;
    uint8_t fw_patch_num;
};

/* True only for exactly the fixed form: nine bytes after the status. */
bool hci_decode_intel_version(const struct hci_answer *a,
                              struct hci_intel_version *out);

/* One bit of the features, numbered as `HCI_FEATURE_` above. */
bool hci_feature(const uint8_t features[8], unsigned bit);

/* "5.0", or 0 for a number the table does not have. */
const char *hci_version_name(uint8_t version);

/* "Realtek", or 0 for a company the table does not have. */
const char *hci_manufacturer_name(uint16_t company);

/*
 * The address as a person reads it, "00:1A:7D:DA:71:13", most significant
 * byte first: on the wire it is least significant first (5.2). `out` holds
 * 18 bytes.
 */
void hci_format_address(const uint8_t address[6], char out[18]);

#endif
