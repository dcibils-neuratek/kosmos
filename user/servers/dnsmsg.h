/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_SERVERS_DNSMSG_H
#define KOSMOS_SERVERS_DNSMSG_H

/*
 * **A DNS message, in one place** (`docs/sharing.md` N11b): how its names
 * are written and read - labels, and the pointers that compress them - and
 * its header and records, for the network stack's resolver (`net.c`) and
 * for multicast DNS (`mdnsd`), which speaks the same messages on another
 * port. RFC 1035, section 4, says what every byte is; the section each rule
 * comes from is named beside it in `dnsmsg.c`.
 *
 * Its own file, with no system calls in it, as `fat_decode.c` is, so
 * `tools/test_dnsmsg.c` can ask it the awkward questions on this Mac: a
 * pointer that points at itself, a name longer than a name may be, a record
 * that says it is longer than the message. What arrives here comes from
 * anybody on the network, so nothing is read past the end of the message
 * and no loop runs for ever.
 */

#include <stdbool.h>
#include <stdint.h>

#define DNS_HEADER       12u
#define DNS_NAME_MAX    255u        /* RFC 1035 2.3.4: a name, as written */
#define DNS_LABEL_MAX    63u

#define DNS_TYPE_A        1u
#define DNS_TYPE_PTR     12u
#define DNS_TYPE_TXT     16u
#define DNS_TYPE_AAAA    28u
#define DNS_TYPE_SRV     33u
#define DNS_TYPE_ANY    255u
#define DNS_CLASS_IN      1u

struct dns_header {
    uint16_t id;
    uint16_t flags;
    uint16_t questions, answers, authorities, additionals;
};

/* A record as read: its owner's name, as text with dots, and where its
 * data lies in the message, for the reader of that type to take. */
struct dns_record {
    char     name[DNS_NAME_MAX + 1];
    uint16_t type;
    uint16_t class;                 /* multicast DNS's top bit: flush */
    uint32_t ttl;
    uint32_t data;                  /* where its data starts */
    uint16_t length;                /* and how long it is */
};

/* `name` (dots between labels) as labels into `out`: the bytes written, or
 * 0 when it does not fit or a label is empty or too long. */
unsigned dns_put_name(uint8_t *out, unsigned room, const char *name, unsigned len);

/* The header from `msg`; false when the message is shorter than one. */
bool dns_read_header(const uint8_t *msg, unsigned len, struct dns_header *out);

/* Where the name at `at` ends - after its last label, or after the pointer
 * that ends it; `len` when it runs off the message. */
unsigned dns_skip_name(const uint8_t *msg, unsigned len, unsigned at);

/* The name at `at`, pointers followed, as text with dots into `out`; where
 * the name ends in the message through `next`. False for a pointer forward
 * or into a loop, a label past the end, or a name longer than 255. */
bool dns_read_name(const uint8_t *msg, unsigned len, unsigned at,
                   char *out, unsigned room, unsigned *next);

/* A question at `*at`: its name, type and class; `*at` moved past it. */
bool dns_read_question(const uint8_t *msg, unsigned len, unsigned *at,
                       char *name, unsigned room, uint16_t *type, uint16_t *class);

/* A record at `*at`, its data inside the message; `*at` moved past it. */
bool dns_read_record(const uint8_t *msg, unsigned len, unsigned *at,
                     struct dns_record *out);

#endif
