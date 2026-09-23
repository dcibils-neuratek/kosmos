/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The USB host controllers - steps one to three of the USB stack.
 *
 * `docs/usb.md` builds USB in steps that each end in something visible:
 * controllers up, enumeration, a mouse, bulk transfers, mass storage,
 * Ethernet. This is the first three. For every xHCI controller the board
 * reports it maps the registers, takes the controller from the firmware if
 * the firmware held it, halts and resets it, and reads which ports have
 * something plugged in - step one. Then it gives the controller its rings and
 * its interrupter, starts it, proves the command ring and the event ring with
 * a No-Op, resets each USB 2 port that has a device, gives every device a
 * slot and an address, and reads its descriptors: what it is and who made it
 * - step two. **And a device that is a mouse is read**: its configuration
 * chosen, its interrupt endpoint given a ring, the boot protocol asked for,
 * and each report it sends handed to the pointer - step three.
 *
 * **Then it stays, and watches.** A device plugged in is reset, addressed and
 * named as the ones found at boot were, and one pulled out has its slot given
 * back, each with a line saying which port - which is how a socket on the
 * outside of a machine is matched to a port number inside it. A controller
 * that did not start is stopped, because its rings' pages go back to the
 * kernel with this process and a controller still running would write into
 * them.
 *
 * --------------------------------------------------------------------
 * Where the numbers come from.
 *
 * Every offset and bit below is from Intel's *eXtensible Host Controller
 * Interface for Universal Serial Bus* specification, revision 1.2, and the
 * table or section is named beside each. QEMU 11.1.1's `hw/usb/hcd-xhci.c`
 * is the model the tests run against and agrees with all of them; it was read
 * for how it behaves, and nothing here is taken from it.
 *
 * **The USB descriptors and requests are the USB 2.0 specification's**
 * (chapter 9), and the mouse's class request and report are HID 1.11's. Both
 * came into the references with the mouse and are named beside what they
 * give. Before that the descriptors' offsets here were remembered, and so
 * were the intervals a device is owed - which turned out to be missing.
 *
 * --------------------------------------------------------------------
 * The firmware, which QEMU does not have.
 *
 * On a real machine the firmware drives the controller before the operating
 * system does - that is how a USB keyboard works in a boot menu - and it has
 * to be asked for it back. The protocol is the USB Legacy Support capability
 * (7.1, 4.22.1): the OS sets its semaphore and waits, for no more than a
 * second, for the firmware to clear its own. The two semaphores sit in
 * adjacent bytes so each side can write its own without rewriting the
 * other's, which is why that write is a byte and not a word.
 *
 * QEMU's controller has no such capability, so under emulation this path
 * never runs and the log says so. The ThinkPad is where it runs first, and
 * the line it prints says which of the outcomes happened.
 *
 * --------------------------------------------------------------------
 * What QEMU cannot show of step two.
 *
 * QEMU's controller asks for no scratchpad buffers, uses 32-byte contexts and
 * addresses 64 bits. The ThinkPad's may do none of those, so all three paths
 * are written and all three are said aloud in the controller's line, and the
 * first place they run is there.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "mmio.h"
#include "say.h"
#include "blockproto.h"
#include "ethproto.h"
#include "ethring.h"
#include "storage_decode.h"
#include "pad_decode.h"
#include "usb_decode.h"

/* Capability registers, from the start of the BAR: Table 5-9. */
#define CAP_LENGTH_VERSION  0x00u   /* CAPLENGTH 7:0, HCIVERSION 31:16 (BCD) */
#define CAP_HCSPARAMS1      0x04u
#define CAP_HCSPARAMS2      0x08u
#define CAP_HCCPARAMS1      0x10u
#define CAP_DBOFF           0x14u   /* 5.3.7: 31:2, in bytes */
#define CAP_RTSOFF          0x18u   /* 5.3.8: 31:5, in bytes */

#define HCS1_SLOTS(v)       ((v) & 0xFFu)               /* Table 5-10 */
#define HCS1_PORTS(v)       (((v) >> 24) & 0xFFu)
#define HCS2_SCRATCHPADS(v) (((((v) >> 21) & 0x1Fu) << 5) \
                             | (((v) >> 27) & 0x1Fu))   /* Table 5-11 */
#define HCC1_AC64           (1u << 0)                   /* Table 5-13 */
#define HCC1_CSZ            (1u << 2)
#define HCC1_XECP(v)        (((v) >> 16) & 0xFFFFu)     /* 5.3.6, in Dwords */

/* Operational registers, from base + CAPLENGTH: Table 5-18. */
#define OP_USBCMD           0x00u
#define OP_USBSTS           0x04u
#define OP_PAGESIZE         0x08u                       /* 5.4.3 */
#define OP_CRCR             0x18u                       /* 5.4.5, 64 bits */
#define OP_DCBAAP           0x30u                       /* 5.4.6, 64 bits */
#define OP_CONFIG           0x38u                       /* 5.4.7 */
#define OP_PORTSC(n)        (0x400u + 0x10u * ((n) - 1u))   /* 5.4.8, n >= 1 */

#define USBCMD_RS           (1u << 0)                   /* 5.4.1 */
#define USBCMD_HCRST        (1u << 1)
#define USBCMD_INTE         (1u << 2)
#define USBSTS_HCH          (1u << 0)                   /* 5.4.2 */
#define USBSTS_EINT         (1u << 3)
#define USBSTS_CNR          (1u << 11)
#define PAGESIZE_4K         (1u << 0)
#define CRCR_RCS            (1u << 0)

/* PORTSC, Table 5-27. The change bits are write-1-to-clear. */
#define PORTSC_CCS          (1u << 0)
#define PORTSC_PED          (1u << 1)
#define PORTSC_PR           (1u << 4)
#define PORTSC_PP           (1u << 9)
#define PORTSC_SPEED(v)     (((v) >> 10) & 0xFu)
#define PORTSC_PRC          (1u << 21)
#define PORTSC_CSC          (1u << 17)
#define PORTSC_CHANGES      0x00FE0000u /* CSC PEC WRC OCC PRC PLC CEC, 17:23 */

/* Runtime registers, interrupter 0: Table 5-37. */
#define RT_IMAN             0x20u
#define RT_IMOD             0x24u
#define RT_ERSTSZ           0x28u
#define RT_ERSTBA           0x30u                       /* 64 bits */
#define RT_ERDP             0x38u                       /* 64 bits */

#define IMAN_IP             (1u << 0)                   /* 5.5.2.1 */
#define IMAN_IE             (1u << 1)
#define IMOD_ONE_MS         4000u                       /* 5.5.2.2, 250 ns each */
#define ERDP_EHB            (1u << 3)                   /* 5.5.2.3.3 */

/* Extended capabilities: Tables 7-1 and 7-2. */
#define XCAP_ID(v)          ((v) & 0xFFu)
#define XCAP_NEXT(v)        (((v) >> 8) & 0xFFu)        /* Dwords, from this one */
#define XCAP_LEGACY         1u
#define XCAP_PROTOCOL       2u

/* USB Legacy Support, Table 7-4: bits 16 and 24, as the bytes they sit in. */
#define LEGSUP_BIOS_BYTE    2u
#define LEGSUP_OS_BYTE      3u
#define LEGSUP_OWNED        0x01u

/* USBLEGCTLSTS, Table 7-5: the five SMI enables, bits 0, 4, 13, 14, 15. */
#define LEGCTLSTS           4u
#define LEGCTLSTS_ENABLES   0x0000E011u

/* Supported Protocol, Tables 7-6, 7-8 and 7-9. */
#define PROTO_MAJOR(v)      (((v) >> 24) & 0xFFu)       /* BCD: 02h, 03h */
#define PROTO_PORTS         8u
#define PROTO_FIRST(v)      ((v) & 0xFFu)
#define PROTO_COUNT(v)      (((v) >> 8) & 0xFFu)
#define PROTO_SLOT          0x0Cu
#define PROTO_SLOT_TYPE(v)  ((v) & 0x1Fu)

/*
 * TRBs: sixteen bytes, four little-endian words (6.4). The type is word 3's
 * 15:10 and the cycle bit its bit 0 (Table 6-91 for the numbers).
 */
#define TRB_C               (1u << 0)
#define TRB_TC              (1u << 1)                   /* Link: toggle cycle */
#define TRB_ISP             (1u << 2)                   /* Normal: short packet */
#define TRB_IOC             (1u << 5)
#define TRB_IDT             (1u << 6)                   /* Setup: data inline */
#define TRB_TYPE(t)         ((uint32_t)(t) << 10)
#define TRB_TYPE_OF(w)      (((w) >> 10) & 0x3Fu)
#define TRB_DIR_IN          (1u << 16)                  /* Data, Status: IN */
#define TRB_TRT_IN          (3u << 16)                  /* Setup: IN data stage */
#define TRB_SLOT(s)         ((uint32_t)(s) << 24)
#define TRB_SLOT_OF(w)      (((w) >> 24) & 0xFFu)
#define TRB_ENDPOINT_OF(w)  (((w) >> 16) & 0x1Fu)       /* word 3 of a transfer */
#define TRB_ENDPOINT(e)     ((uint32_t)(e) << 16)       /* ...and of a command */
#define TRB_CODE_OF(w)      (((w) >> 24) & 0xFFu)       /* word 2 of an event */
#define TRB_LEFT_OF(w)      ((w) & 0xFFFFFFu)           /* ...and bytes not sent */

#define TRB_NORMAL          1u
#define TRB_SETUP           2u
#define TRB_DATA            3u
#define TRB_STATUS          4u
#define TRB_LINK            6u
#define TRB_ENABLE_SLOT     9u
#define TRB_DISABLE_SLOT    10u
#define TRB_ADDRESS_DEVICE  11u
#define TRB_CONFIGURE       12u
#define TRB_EVALUATE        13u
#define TRB_RESET_ENDPOINT  14u
#define TRB_STOP_ENDPOINT   15u
#define TRB_SET_DEQUEUE     16u
#define TRB_NOOP            23u
#define TRB_TRANSFER        32u
#define TRB_COMPLETION      33u
#define TRB_PORT_CHANGE     34u

/* Completion codes, Table 6-90. */
#define CC_SUCCESS          1u
#define CC_SHORT_PACKET     13u
#define CC_CONTEXT_STATE    19u

#define PORTS_MAX           255u        /* MaxPorts is eight bits */
#define CAPS_MAX            64u         /* a list longer than this is a loop */

/*
 * **Memory**: one region a controller, one physical run, so every address
 * the controller is told is the region's bus address plus an offset. Pages:
 *
 *   0   the Device Context Base Address Array (5.4.6, 6.1)
 *   1   the command ring, 256 TRBs, the last a Link back to the first
 *   2   the event ring's one segment, 256 TRBs (6.5)
 *   3   the one-entry Event Ring Segment Table, and the scratchpad array
 *   4+  six pages a device: input context, output context, the default
 *       control endpoint's ring, a buffer for what it sends back, and - for
 *       a mouse - its interrupt endpoint's ring and the page its reports
 *       come back in, or - for a stick - its bulk OUT and bulk IN rings,
 *       with what it is sent and sends back in the buffer
 *   then a page for each scratchpad buffer the controller asks for (4.20)
 *
 * Eight devices a controller, as a start: the slots enabled are that many,
 * so a slot ID is also the device's place in the region.
 */
#define PAGE                4096u
#define RING_TRBS           256u
#define DEVICES_MAX         8u
#define NAME_MAX_CHARS      48u         /* of a product string, kept */
#define PORT_FAILED         0xFFu       /* a port whose device was not named */
#define PAGES_A_DEVICE      8u
#define PAGE_DCBAA          0u
#define PAGE_COMMANDS       1u
#define PAGE_EVENTS         2u
#define PAGE_ERST           3u
#define PAGE_DEVICES        4u
#define DEVICE_PAGE_RING    4u          /* of a device's own eight: a mouse's */
#define DEVICE_PAGE_REPORT  5u          /* ...ring, and its reports */
#define DEVICE_PAGE_OUT     4u          /* ...or a stick's bulk OUT ring */
#define DEVICE_PAGE_IN      5u          /* ...and its bulk IN ring */
#define DEVICE_PAGE_PAD_OUT 6u          /* an Xbox One pad's OUT ring */
#define DEVICE_PAGE_PAD_SAY 7u          /* ...and what is sent on it */
#define DEVICE_PAGE_NOTIFY  6u          /* an Ethernet adapter's interrupt ring */
#define DEVICE_PAGE_NOTE    7u          /* ...and the notifications it reads */
#define PAD_SAY_SLOT        64u         /* bytes a message, a page of them */
#define SCRATCH_OFFSET      64u         /* the array, after the table's entry */
#define SCRATCHPADS_MAX     ((PAGE - SCRATCH_OFFSET) / 8u)

/*
 * **Waits in milliseconds**, turned into scheduler ticks by `ticks_for` once
 * the tick rate is known.
 *
 * They were written as ticks, "a hundredth of a second each", from a comment
 * in `kosmos.h` that had stopped being true when the kernel moved to 250 Hz.
 * So every wait here was two and a half times shorter than it said - the
 * second the specification gives the firmware among them - and the handoff
 * line would have printed its milliseconds two and a half times too long.
 */
#define HALT_MS             20u         /* 5.4.1 allows 16 ms */
#define FIRMWARE_MS         1000u       /* 4.22.1: no more than a second */
#define RESET_MS            1000u
#define SETTLE_MS           500u
#define ANSWER_MS           1000u       /* a command, a transfer, a port reset */
#define EVENTS_MAX          64u         /* unrelated events borne while waiting */

/*
 * **USB 2.0's own intervals**, which a device is owed and which the driver did
 * not give until the specification was in the references. The ThinkPad showed
 * the cost: its mouse, plugged back in, failed its first request with a USB
 * Transaction Error 12 ms after its port's reset, and was named only at its
 * next plug.
 *
 *   ATTACH_MS          7.1.7.3, TATTDB: a connection stable this long before
 *                      the device is reset, started again by a disconnect
 *   RESET_RECOVERY_MS  7.1.7.5 and 9.2.6.2, TRSTRCY: after a reset and before
 *                      the first request, during which "the device may ignore
 *                      any data transfers"
 *   ADDRESS_MS         9.2.6.3: after SET_ADDRESS's status stage and before a
 *                      request to the new address. Address Device is what
 *                      sends it, and xHCI 1.2 4.6.5 leaves this to software
 *
 * `ATTACH_TRIES` is how many intervals a connection may bounce through before
 * it is left for its next change to try again.
 */
#define ATTACH_MS           100u
#define ATTACH_TRIES        5u
#define RESET_RECOVERY_MS   10u
#define ADDRESS_MS          2u

/*
 * The pause before a second attempt to address a device, on top of those:
 * 4.6.5's notes allow a failed Address Device to be tried again after a reset,
 * and a device that failed with USB 2.0's intervals given is given fifty
 * milliseconds more.
 */
#define RECOVERY_MS         50u

/*
 * How long the watch waits for any controller's interrupt before looking at
 * every port anyway. A plug, an unplug and a mouse's report all interrupt at
 * once, so this bounds only a controller whose interrupt never arrives, or
 * one whose interrupt could not be claimed and is polled.
 */
#define WATCH_MS            50u

struct ring {
    uint32_t *trbs;                     /* RING_TRBS of them, the last a Link */
    uint64_t  bus;
    unsigned  enqueue;
    uint32_t  cycle;                    /* what goes in C: TRB_C or 0 */
};

struct device {
    unsigned  slot;
    unsigned  port;
    unsigned  speed;
    uint32_t *input;
    uint32_t *output;
    uint8_t  *buffer;
    uint64_t  input_bus;
    uint64_t  output_bus;
    uint64_t  buffer_bus;
    struct ring ep0;

    /* bNumConfigurations, from its device descriptor (9.6.1), and the first
     * language its strings are offered in - 0 until one has been asked
     * for, since no LANGID is 0. */
    unsigned  configurations;
    uint16_t  language;
};

/*
 * A mouse: its interrupt endpoint, the ring its requests go on, and the page
 * its reports come back in. One request is on the ring at a time, and the
 * next is queued when a report is read - so `reading` is whether there is one.
 */
struct mouse {
    bool          reading;
    unsigned      port;
    unsigned      dci;                  /* its endpoint's context index, 4.5.1 */
    unsigned      length;               /* a request's buffer: its packet */
    uint32_t      buttons;              /* what its last report held down */
    unsigned long reports;              /* read, for the line when it leaves */
    unsigned long looked;               /* of those, found without its interrupt */
    struct ring   ring;
    uint8_t      *report;
    uint64_t      report_bus;

    /* Where its buttons and movement are, by its Report descriptor; when
     * that is not `ok`, it is read as a boot mouse. */
    struct usb_mouse_report layout;

    /*
     * **Or a game controller**, read on the same ring the same way: an Xbox
     * 360 pad (`pad_decode.h`), whose reports become keys rather than
     * movement. `pressed` is what programs were last told is down, so a
     * report says only what changed; `said` counts the presses logged.
     */
    bool          pad;
    uint32_t      pressed;
    unsigned      said;

    /*
     * **Or a keyboard**, read on the same ring the same way: a boot
     * keyboard's report is eight fixed bytes (HID 1.11 B.1), so there is no
     * Report descriptor to read and nothing to work out. `was` is the report
     * before this one, which is what a change is measured against - a
     * keyboard sends its whole state every time.
     *
     * `held` is what this keyboard has told the system is down, so a
     * keyboard unplugged with a key held can have it let go. Without that a
     * Control taken out of its port is a Control held down for ever.
     */
    bool          keyboard;
    uint8_t       was[8];
    uint16_t      held[8];
    unsigned      holding;
    unsigned long keys;                 /* changes pushed, for its last line */

    /*
     * **An Xbox One or Series pad** (`xone`) is spoken to as well: its
     * interrupt OUT, a ring and a page of 64-byte messages taken in turn,
     * the sequence number of the next one, and the state its messages build
     * up - the Xbox button arrives in a message of its own. `announced` is
     * whether the start-up has been sent again after the pad said it had
     * arrived, which some pads wait for.
     */
    bool          xone;
    bool          announced;
    unsigned      out_dci;
    struct ring   out_ring;
    uint8_t      *say;
    uint64_t      say_bus;
    unsigned      say_next;
    uint8_t       seq;
    uint16_t      vendor, product;
    struct pad_state state;
};

/*
 * A stick: its two bulk endpoints' context indexes and rings, its interface
 * for Bulk-Only's class reset, and the tag its next command goes out with -
 * which the stick sends back with the status, so an answer can be told from
 * the answer to something else. `spoil` is a test's (`transact_once`).
 */
struct stick {
    unsigned      out_dci;
    unsigned      in_dci;
    uint8_t       interface;
    bool          spoil;
    uint32_t      tag;
    struct ring   out;
    struct ring   in;

    /*
     * **Its device, kept.** `attach` holds a device in a variable of its own,
     * gone when the plug is done; a stick is spoken to long after that, when
     * a client asks, and Reset Recovery then goes on its endpoint 0 ring. So
     * `use_stick` works on this copy from its first line.
     */
    struct device dev;

    /*
     * **The buffer every command's data comes through**: a run of its own
     * that the controller reaches (`stick_buffer`), where the device's page is
     * 4 KB and the status comes through that. A client's reads are copied out
     * of it, so a client's pages are never the controller's to write.
     */
    long          transfer_cap;
    uintptr_t     transfer;             /* mapped here, or 0 for none */
    uint64_t      transfer_bus;

    /*
     * Once its size is known: a unit a client can name (`find_unit`), by a
     * number no other stick is ever given (`units_named`).
     */
    bool          ready;
    uint32_t      unit;
    uint64_t      blocks;
    uint32_t      block_size;
    char          vendor[8];
    char          product[16];

    /* Whether a flush it kept has been said yet (USB step 5e). */
    bool          flushed;

    /*
     * Whether it has said it does not do SYNCHRONIZE CACHE (10) at all
     * (`scsi_not_supported`), after which none is sent to it and a flush is
     * answered BLOCK_ERR_NO_FLUSH. The ThinkPad's Kingston said so to every
     * one - two a commit, each with a REQUEST SENSE and a line on the screen.
     */
    bool          no_flush;
};

/*
 * **An Ethernet adapter**, once it is configured - `usb.md` step 7b.
 *
 * Three endpoints: the two bulk ones a frame moves on, and an interrupt IN
 * the adapter says its link on. The notification is read the way a mouse's
 * report is - one request out at a time, the next queued when one comes back
 * - so a link that goes up an hour from now is noticed without anything
 * waiting for it, and nothing here blocks a plug on the other side of the
 * machine.
 *
 * `link_said` and `speed_said` are whether the adapter has ever said, which
 * is not the same as what it said: a link that is down and a link nothing
 * has reported are different things to a person reading the log.
 */
struct ether {
    bool          reading;              /* a notification request is out */
    unsigned      port;
    unsigned      in_dci;
    unsigned      out_dci;
    unsigned      out_packet;           /* its wMaxPacketSize, for ECM 3.3.1 */
    unsigned      notify_dci;
    unsigned      notify_length;        /* a request's buffer: its packet */
    struct ring   in;
    struct ring   out;
    struct ring   notify;
    uint8_t      *note;                 /* where a notification arrives */
    uint64_t      note_bus;

    uint8_t       mac[6];
    bool          have_mac;
    uint16_t      max_segment;          /* the largest frame it carries */

    bool          link;                 /* what NETWORK_CONNECTION said */
    bool          link_said;
    uint32_t      upstream;             /* bits a second, CONNECTION_SPEED */
    uint32_t      downstream;
    bool          speed_said;

    /*
     * **Where frames go**, a run of its own the controller can reach, the
     * same three calls a stick's transfer buffer is made with: the frame
     * being sent at the front and the one being received behind it.
     *
     * **One read outstanding**, as a mouse's report is. Frames that arrive
     * back to back while this end is busy are NAKed and sent again by the
     * adapter, which is what a bulk endpoint is for; several reads in flight
     * is what makes a link fast rather than what makes it work, and it
     * belongs with the ring that carries frames to the stack (7d).
     */
    long          frames_cap;
    uintptr_t     frames;               /* mapped here, or 0 for none */
    uint64_t      frames_bus;
    bool          receiving;            /* a read is out on the bulk IN */
    unsigned long received;
    unsigned long sent;
    unsigned long dropped;              /* arrived with the stack's ring full */
    unsigned      said_frames;          /* of those, written down */

    /* Its device, kept, for the same reason a stick keeps one. */
    struct device dev;
};

/*
 * **How many unit numbers have been given out.** A stick takes the next as it
 * becomes ready and keeps it until it leaves, and no other stick is ever given
 * it (USB step 5e). The disk server keeps the unit its partition is on, so a
 * number that could pass to another stick would be that stick written over.
 */
static uint32_t units_named;

struct controller {
    uintptr_t     base;                 /* the capability registers */
    uintptr_t     op;                   /* the operational registers */
    uintptr_t     rt;                   /* the runtime registers */
    uintptr_t     doorbells;
    unsigned long size;                 /* of the window, in bytes */
    unsigned      where;                /* bus << 8 | slot << 3 | function */
    unsigned      ports;
    unsigned char usb[PORTS_MAX + 1];   /* the USB major version, 0 unknown */
    unsigned char slot_type[PORTS_MAX + 1];

    unsigned      context;              /* bytes: 32, or 64 with CSZ */
    bool          ac64;                 /* it can reach memory above 4 GB */
    unsigned      slots;                /* MaxSlotsEn */
    unsigned      scratchpads;
    uintptr_t     mem;                  /* the region, mapped here */
    uint64_t      bus;                  /* and there, for the controller */
    unsigned long pages;

    uint64_t     *dcbaa;
    struct ring   commands;
    uint32_t     *events;               /* the segment */
    uint64_t      events_bus;
    unsigned      dequeue;
    uint32_t      event_cycle;

    long          irq;                  /* a capability, or negative */
    unsigned      intid;
    unsigned      interrupts;           /* arrived, and counted */
    unsigned      named;                /* devices whose descriptors were read */
    uint32_t      last_code;            /* the last answer's code; 0, none came */

    bool          running;              /* started, and watched */
    bool          woken;                /* this look is its own interrupt's */

    /* Each port's device: 0 none, its slot, or PORT_FAILED - there, not named. */
    unsigned char port_slot[PORTS_MAX + 1];

    /* What each slot's device said it was, for the line when it leaves. */
    bool          described[DEVICES_MAX + 1];
    uint16_t      vendor[DEVICES_MAX + 1];
    uint16_t      product[DEVICES_MAX + 1];
    char          name[DEVICES_MAX + 1][NAME_MAX_CHARS + 1];

    /* Each slot's mouse, when its device is one. */
    struct mouse  mouse[DEVICES_MAX + 1];

    /* ...and its stick, when it is one of those. */
    struct stick  stick[DEVICES_MAX + 1];

    /* ...and its Ethernet adapter. */
    struct ether  ether[DEVICES_MAX + 1];
};

static long console = -1;
/*
 * **The network stack, once it has attached** - `usb.md` 7d.
 *
 * One stack and one adapter: `SPAWN_NET` means there is exactly one process
 * that may hold the wire, and the first adapter plugged in is the one it
 * gets. A second adapter is named and left alone, which is honest about what
 * this does rather than pretending to a generality it has not got.
 */
static long frames_endpoint = -1;
static struct eth_ring *frames_ring;    /* the stack's region, mapped here */
static long frames_ring_cap = -1;
static struct controller *frames_on;    /* whose adapter the stack has */
static unsigned frames_slot;

struct say_line;
static void serve_frames(struct say_line *line);

/*
 * As many controllers as the closing line names and the driver keeps. The
 * board keeps four, so the limit is never the one that stops the search. At
 * file scope because a mouse's buttons are the pointer's together with every
 * other mouse's, on whichever controller.
 */
#define NAMED_MAX           8u

static struct controller controllers[NAMED_MAX];
static unsigned controllers_found;

/* Asked of the machine when the driver starts; 250 only if it will not say. */
static unsigned long tick_hz = 250u;

/* And the counter's rate, which a wait's deadline is measured in: 62.5 MHz,
 * QEMU's on `virt`, only if the machine will not say. */
static unsigned long counter_hz = 62500000u;

/* Milliseconds as scheduler ticks, rounded up so no wait is shorter than
 * asked for. */
static unsigned ticks_for(unsigned long ms)
{
    return (unsigned)((ms * tick_hz + 999u) / 1000u);
}

/* Milliseconds in the counter's units, for a deadline that is measured
 * rather than slept (`wait_serving`). */
static unsigned long counter_for(unsigned long ms)
{
    return (unsigned long)((uint64_t)ms * counter_hz / 1000u);
}

/* A PCI address the way people write one: 00:0d.0. */
static void address(struct say_line *line, unsigned where)
{
    say_hex(line, (where >> 8) & 0xFFu, 2);
    say_text(line, ":");
    say_hex(line, (where >> 3) & 0x1Fu, 2);
    say_text(line, ".");
    say_dec(line, where & 7u);
}

/* "xhci: 00:0d.0" - the start of every line about one controller. */
static void about(struct say_line *line, const struct controller *c)
{
    say_begin(line);
    say_text(line, "xhci: ");
    address(line, c->where);
}

/* Whether (reg & mask) == want within `ticks`, polling once a tick. */
static bool settles(uintptr_t reg, uint32_t mask, uint32_t want,
                    unsigned ticks)
{
    unsigned waited = 0;

    for (;;) {
        if ((mmio_read32(reg) & mask) == want) {
            return true;
        }

        if (waited++ >= ticks) {
            return false;
        }

        kosmos_sleep(1);
    }
}

/*
 * A 64-bit register as two Dword writes, **low half first and high half
 * second** - 5.1's words for every register with a 64-bit address in it are
 * "low Dword-first, high-Dword second". ERSTBA's high half is what arms the
 * event ring in QEMU's model. ERDP comes through here too, from
 * `event_dequeue_to`, and for a while it did not.
 */
static void write64(uintptr_t reg, uint64_t value)
{
    mmio_write32(reg, (uint32_t)value);
    mmio_write32(reg + 4u, (uint32_t)(value >> 32));
}

/*
 * The OS semaphore set, and a second for the firmware to let go (4.22.1).
 *
 * Used either way when the second is up, and said so: the specification
 * gives the firmware no way to refuse, only to be slow, and a machine whose
 * firmware never answers is better told than hung.
 */
static void take_from_firmware(const struct controller *c, uintptr_t legsup,
                               struct say_line *line)
{
    uint32_t enables;
    unsigned waited = 0;
    bool held = (mmio_read8(legsup + LEGSUP_BIOS_BYTE) & LEGSUP_OWNED) != 0;

    mmio_write8(legsup + LEGSUP_OS_BYTE,
                (uint8_t)(mmio_read8(legsup + LEGSUP_OS_BYTE) | LEGSUP_OWNED));

    while ((mmio_read8(legsup + LEGSUP_BIOS_BYTE) & LEGSUP_OWNED) != 0
           && waited < ticks_for(FIRMWARE_MS)) {
        kosmos_sleep(1);
        waited++;
    }

    about(line, c);

    if (!held) {
        say_text(line, " was not the firmware's; claimed");
    } else if ((mmio_read8(legsup + LEGSUP_BIOS_BYTE) & LEGSUP_OWNED) != 0) {
        say_text(line, " is still the firmware's after a second of asking; "
                       "used anyway");
    } else {
        say_text(line, " taken from the firmware after ");
        say_dec(line, (unsigned long)waited * 1000u / tick_hz);
        say_text(line, " ms");
    }

    enables = mmio_read32(legsup + LEGCTLSTS) & LEGCTLSTS_ENABLES;

    if (enables != 0) {
        say_text(line, ", with its SMI enables 0x");
        say_hex(line, enables, 4);
        say_text(line, " still set");
    }

    say_send(console, line);
}

/*
 * The extended capabilities: the firmware handoff, and which ports speak
 * which USB with which slot type. Every read is checked against the window
 * first, because the pointers are the device's to set and a wrong one would
 * fault this process.
 */
static void walk_capabilities(struct controller *c, struct say_line *line)
{
    unsigned long at = (unsigned long)HCC1_XECP(
        mmio_read32(c->base + CAP_HCCPARAMS1)) * 4u;
    unsigned guard;
    bool legacy = false;

    for (guard = 0; at != 0 && guard < CAPS_MAX; guard++) {
        uint32_t head;

        if (at + 16u > c->size) {
            break;
        }

        head = mmio_read32(c->base + at);

        if (XCAP_ID(head) == XCAP_LEGACY) {
            take_from_firmware(c, c->base + at, line);
            legacy = true;
        } else if (XCAP_ID(head) == XCAP_PROTOCOL) {
            uint32_t ports = mmio_read32(c->base + at + PROTO_PORTS);
            uint32_t slot = mmio_read32(c->base + at + PROTO_SLOT);
            unsigned p;

            /*
             * The slot type is the controller's own number for this protocol
             * (Table 7-9, and its footnote: software shall not assume one),
             * and Enable Slot has to be given it for a device on these ports.
             */
            for (p = PROTO_FIRST(ports);
                 p < PROTO_FIRST(ports) + PROTO_COUNT(ports) && p <= PORTS_MAX;
                 p++) {
                c->usb[p] = (unsigned char)PROTO_MAJOR(head);
                c->slot_type[p] = (unsigned char)PROTO_SLOT_TYPE(slot);
            }
        }

        if (XCAP_NEXT(head) == 0) {
            break;
        }

        at += (unsigned long)XCAP_NEXT(head) * 4u;
    }

    if (!legacy) {
        about(line, c);
        say_text(line, " has no firmware handoff to make");
        say_send(console, line);
    }
}

/*
 * Halted, then reset, then ready (4.2, 5.4.1, 5.4.2).
 *
 * Nothing operational is written until Controller Not Ready clears, a reset
 * is only asked of a halted controller, and the reset is over when the
 * controller clears HCRST itself and is ready again.
 */
static bool reset(const struct controller *c, struct say_line *line)
{
    const char *why = NULL;

    if (!settles(c->op + OP_USBSTS, USBSTS_CNR, 0, ticks_for(RESET_MS))) {
        why = " never became ready";
    } else {
        if ((mmio_read32(c->op + OP_USBSTS) & USBSTS_HCH) == 0) {
            mmio_write32(c->op + OP_USBCMD,
                         mmio_read32(c->op + OP_USBCMD)
                         & ~(USBCMD_RS | USBCMD_INTE));

            if (!settles(c->op + OP_USBSTS, USBSTS_HCH, USBSTS_HCH,
                         ticks_for(HALT_MS))) {
                why = " would not halt";
            }
        }

        if (why == NULL) {
            mmio_write32(c->op + OP_USBCMD,
                         mmio_read32(c->op + OP_USBCMD) | USBCMD_HCRST);

            if (!settles(c->op + OP_USBCMD, USBCMD_HCRST, 0,
                         ticks_for(RESET_MS))
                || !settles(c->op + OP_USBSTS, USBSTS_CNR, 0,
                            ticks_for(RESET_MS))) {
                why = " did not finish resetting";
            }
        }
    }

    if (why != NULL) {
        about(line, c);
        say_text(line, why);
        say_send(console, line);
        return false;
    }

    return true;
}

/*
 * Table 7-13's default speeds. A controller that defines its own Protocol
 * Speed IDs (PSIC nonzero) may number them differently; the number is
 * printed alongside for that reason.
 */
static const char *speed_name(unsigned id)
{
    switch (id) {
    case 1:  return "Full-speed";
    case 2:  return "Low-speed";
    case 3:  return "High-speed";
    case 4:  return "SuperSpeed";
    case 5:
    case 6:
    case 7:  return "SuperSpeedPlus";
    default: return "unnamed-speed";
    }
}

/*
 * The default control endpoint's packet size, by speed (4.3.3, 5): 8 for low
 * and full speed - full speed's real size is read from the device and set
 * afterwards - 64 for high speed, 512 for SuperSpeed.
 */
static unsigned default_packet(unsigned speed)
{
    switch (speed) {
    case 1:
    case 2:  return 8u;
    case 3:  return 64u;
    default: return 512u;
    }
}

/* ------------------------------------------------------------------ rings */

/*
 * A ring of RING_TRBS in one page, the last a Link back to the first with
 * Toggle Cycle set (4.9.2, 6.4.4.1). The cycle bit starts at 1, which is what
 * CRCR's RCS and an endpoint context's DCS tell the controller.
 */
static void ring_start(struct ring *r, uint32_t *trbs, uint64_t bus)
{
    uint32_t *link = trbs + (RING_TRBS - 1u) * 4u;

    memset(trbs, 0, PAGE);

    r->trbs = trbs;
    r->bus = bus;
    r->enqueue = 0;
    r->cycle = TRB_C;

    link[0] = (uint32_t)bus;
    link[1] = (uint32_t)(bus >> 32);
    link[3] = TRB_TYPE(TRB_LINK) | TRB_TC | TRB_C;
}

/*
 * One TRB onto a ring, and where it went in the controller's addresses.
 *
 * **Word 3 last**, because it carries the cycle bit, and a TRB whose cycle bit
 * matches is one the controller may take: the other three words have to be
 * in place first.
 *
 * At the Link the cycle bit turns over (4.9.2): the Link is given the current
 * value, so the controller follows it, and everything written after carries
 * the other - which is what makes last time round's TRBs stale.
 */
static uint64_t ring_push(struct ring *r, uint32_t w0, uint32_t w1,
                          uint32_t w2, uint32_t w3)
{
    uint32_t *trb = r->trbs + r->enqueue * 4u;
    uint64_t at = r->bus + (uint64_t)r->enqueue * 16u;

    trb[0] = w0;
    trb[1] = w1;
    trb[2] = w2;
    trb[3] = (w3 & ~TRB_C) | r->cycle;

    r->enqueue++;

    if (r->enqueue == RING_TRBS - 1u) {
        uint32_t *link = r->trbs + r->enqueue * 4u;

        link[3] = (link[3] & ~TRB_C) | r->cycle;
        r->cycle ^= TRB_C;
        r->enqueue = 0;
    }

    return at;
}

/*
 * ERDP, with Event Handler Busy written as 1 once events are taken, so the
 * controller clears it and interrupts again if more are waiting behind the
 * pointer (5.5.2.3.3, 4.17.2) - and like every 64-bit register, low half
 * first and high half second (`write64`, 5.1).
 *
 * **It was the other way round from 0.10.54 until the ThinkPad**, under a
 * comment saying the low half's write is where the controller looks. That is
 * QEMU's model, which acts on the low write (`hcd-xhci.c`), and not the
 * specification. What the ThinkPad showed on 13 September fits a controller
 * that takes the register when its high half arrives, and so cleared Busy one
 * write late: once events had been taken, no interrupt came for the next -
 * a second for each step of naming its mouse, and 882 reports read in 25
 * minutes from a mouse that offers one a millisecond while it moves. QEMU
 * cannot show the order's effect, so `run_x86.py`'s `usb` reads the order out
 * of QEMU's own trace of the writes.
 */
static void event_dequeue_to(const struct controller *c, uint64_t at,
                             bool busy)
{
    write64(c->rt + RT_ERDP, at | (busy ? ERDP_EHB : 0u));
}

/*
 * The next event, if there is one: a TRB whose cycle bit is the one this side
 * expects (4.9.4). Taken, the dequeue moves on - turning the cycle bit over
 * where the segment wraps, as the controller does - and the controller is
 * told how far this side has read.
 */
static bool take_event(struct controller *c, uint32_t *out)
{
    uint32_t *trb = c->events + c->dequeue * 4u;

    if ((trb[3] & TRB_C) != c->event_cycle) {
        return false;
    }

    memcpy(out, trb, 4u * sizeof(uint32_t));

    if (++c->dequeue == RING_TRBS) {
        c->dequeue = 0;
        c->event_cycle ^= TRB_C;
    }

    event_dequeue_to(c, c->events_bus + (uint64_t)c->dequeue * 16u, true);
    return true;
}

/*
 * A controller's interrupt acknowledged: EINT cleared before IP, the order
 * 5.4.2 gives, and the line unmasked - before its ring is emptied, so an event
 * written after the emptying raises it again rather than waiting for a look.
 */
static void acknowledge(const struct controller *c)
{
    if (c->irq >= 0) {
        mmio_write32(c->op + OP_USBSTS, USBSTS_EINT);
        mmio_write32(c->rt + RT_IMAN, IMAN_IE | IMAN_IP);
        (void)kosmos_irq_ack(c->irq);
    }
}

/* Whether an event is a mouse's report: a transfer on the endpoint of a mouse
 * that is being read. */
static bool is_report(const struct controller *c, const uint32_t *event)
{
    unsigned slot = TRB_SLOT_OF(event[3]);

    return TRB_TYPE_OF(event[3]) == TRB_TRANSFER && slot != 0
           && slot <= DEVICES_MAX && c->mouse[slot].reading
           && TRB_ENDPOINT_OF(event[3]) == c->mouse[slot].dci;
}

/*
 * The same for an Ethernet adapter's notification, which arrives whenever the
 * link changes and must not be mistaken for the answer to whatever a plug on
 * another port is waiting for.
 */
static bool is_note(const struct controller *c, const uint32_t *event)
{
    unsigned slot = TRB_SLOT_OF(event[3]);

    return TRB_TYPE_OF(event[3]) == TRB_TRANSFER && slot != 0
           && slot <= DEVICES_MAX && c->ether[slot].reading
           && TRB_ENDPOINT_OF(event[3]) == c->ether[slot].notify_dci;
}

/*
 * And a frame off an adapter's bulk IN, for the same reason: it arrives
 * whenever something on the wire sends one, which is not when anything here
 * asked.
 */
static bool is_frame(const struct controller *c, const uint32_t *event)
{
    unsigned slot = TRB_SLOT_OF(event[3]);

    return TRB_TYPE_OF(event[3]) == TRB_TRANSFER && slot != 0
           && slot <= DEVICES_MAX && c->ether[slot].receiving
           && TRB_ENDPOINT_OF(event[3]) == c->ether[slot].in_dci;
}

static void take_report(struct controller *c, const uint32_t *event);
static void take_note(struct controller *c, const uint32_t *event);
static void take_frame(struct controller *c, const uint32_t *event);

/*
 * **A wait that goes on reading mice.** Until `c` has an event that is not a
 * mouse's report - put in `out`, and true - or until `ms` have passed, and
 * false; with `out` NULL, only the time. Meanwhile every running controller's
 * mice have their reports read as they come, `c`'s among them.
 *
 * Every wait a plug or an unplug makes comes through here: USB 2.0's
 * debounce, a port's reset and its recovery, and every command and control
 * transfer. They waited on `c`'s interrupt alone, and kept a report that came
 * meanwhile to read afterwards - so a plug held every mouse, on every
 * controller, for as long as naming the device took. QEMU's trace put that at
 * 104 and 107 ms, the debounce; on the ThinkPad a stick that dropped off its
 * bus took 2.3 seconds to be named again, and a mouse would have waited all
 * of it.
 *
 * **The deadline is the counter's**, not a count of wakes: a mouse interrupts
 * a thousand times a second, and a wait counted in wakes would give a command
 * a quarter of the second it is owed. When it passes, every ring is looked at
 * once more, so a missing interrupt costs the time rather than the device.
 * A controller that is polled is looked at every tick.
 *
 * **Nothing here attaches or detaches.** A port that changes meanwhile keeps
 * its change bits, and the watch reads them when the plug in hand is done.
 */
static bool wait_serving(struct controller *c, unsigned long ms,
                         uint32_t *out)
{
    unsigned long start = kosmos_ticks();
    unsigned long span = counter_for(ms);
    long lines[IRQ_WAIT_ANY_MAX];
    struct controller *owner[IRQ_WAIT_ANY_MAX];
    unsigned count = 0, i;
    bool polled = false;
    long woke = SYS_NO_INTERRUPT;

    for (i = 0; i < controllers_found; i++) {
        struct controller *o = &controllers[i];

        if (o->events == NULL || !(o->running || o == c)) {
            continue;
        }

        if (o->irq < 0 || count == IRQ_WAIT_ANY_MAX) {
            polled = true;
        } else {
            owner[count] = o;
            lines[count++] = o->irq;
        }
    }

    for (;;) {
        unsigned long passed, ticks;

        for (i = 0; i < controllers_found; i++) {
            struct controller *o = &controllers[i];
            uint32_t event[4];

            if (o->events == NULL || !(o->running || o == c)) {
                continue;
            }

            o->woken = woke >= 0 && (unsigned long)woke < count
                       && owner[woke] == o;

            while (take_event(o, event)) {
                if (o == c && out != NULL && !is_report(o, event)
                    && !is_note(o, event) && !is_frame(o, event)) {
                    memcpy(out, event, sizeof(event));
                    return true;
                }

                take_report(o, event);
                take_note(o, event);
                take_frame(o, event);
            }
        }

        passed = kosmos_ticks() - start;

        if (passed >= span) {
            return false;
        }

        ticks = (unsigned long)(((uint64_t)(span - passed) * tick_hz
                                 + counter_hz - 1u) / counter_hz);

        if (polled || ticks == 0) {
            ticks = 1;
        }

        woke = count > 0 ? kosmos_irq_wait_any(lines, count, ticks, NULL, 0)
                         : SYS_NO_INTERRUPT;

        /* Nothing to wait on, or a wait refused: slept instead, never spun. */
        if (count == 0 || (woke < 0 && woke != SYS_NO_INTERRUPT)) {
            kosmos_sleep(1);
            woke = SYS_NO_INTERRUPT;
        }

        if (woke >= 0 && (unsigned long)woke < count) {
            owner[woke]->interrupts++;
        }

        for (i = 0; i < count; i++) {
            acknowledge(owner[i]);
        }
    }
}

/*
 * A command, and its Command Completion Event (4.6, 6.4.2.2). Events for
 * anything else that arrive first - a port changing - are passed over: the
 * port registers are what this driver reads for ports. A mouse's report, on
 * this controller or another, is read as it comes (`wait_serving`).
 */
static bool command(struct controller *c, uint32_t w0, uint32_t w1,
                    uint32_t w3, uint32_t *done)
{
    uint64_t at = ring_push(&c->commands, w0, w1, 0, w3);
    unsigned seen;

    mmio_write32(c->doorbells, 0);      /* doorbell 0, target 0: commands */
    c->last_code = 0;

    for (seen = 0; seen < EVENTS_MAX; seen++) {
        if (!wait_serving(c, ANSWER_MS, done)) {
            return false;
        }

        if (TRB_TYPE_OF(done[3]) == TRB_COMPLETION
            && (((uint64_t)done[1] << 32) | done[0]) == at) {
            c->last_code = TRB_CODE_OF(done[2]);
            return c->last_code == CC_SUCCESS;
        }
    }

    return false;
}

/* --------------------------------------------------------------- starting */

/*
 * The region, the rings and the interrupter, then Run (4.2).
 *
 * Every write here is one 4.2 lists before Run/Stop, in its order: slots
 * enabled, the context array, the command ring, the interrupter's event ring
 * with ERSTBA's high half last, moderation, and the two interrupt enables.
 */
static bool start(struct controller *c, const struct dev_info *dev,
                  struct say_line *line)
{
    uint32_t hcs1 = mmio_read32(c->base + CAP_HCSPARAMS1);
    uint32_t hcs2 = mmio_read32(c->base + CAP_HCSPARAMS2);
    uint32_t hcc1 = mmio_read32(c->base + CAP_HCCPARAMS1);
    uint32_t *erst;
    uint64_t *scratch;
    const char *why = NULL;
    long region, mapped, bus;
    unsigned i;

    c->rt = c->base + (mmio_read32(c->base + CAP_RTSOFF) & ~0x1Fu);
    c->doorbells = c->base + (mmio_read32(c->base + CAP_DBOFF) & ~0x3u);
    c->context = (hcc1 & HCC1_CSZ) != 0 ? 64u : 32u;
    c->scratchpads = HCS2_SCRATCHPADS(hcs2);
    c->slots = HCS1_SLOTS(hcs1) < DEVICES_MAX ? HCS1_SLOTS(hcs1) : DEVICES_MAX;
    c->pages = PAGE_DEVICES + DEVICES_MAX * PAGES_A_DEVICE + c->scratchpads;
    c->irq = -1;

    if (c->rt + RT_ERDP + 8u > c->base + c->size
        || c->doorbells + 4u * (DEVICES_MAX + 1u) > c->base + c->size) {
        why = "'s runtime registers or doorbells lie outside its window";
    } else if ((mmio_read32(c->op + OP_PAGESIZE) & PAGESIZE_4K) == 0) {
        why = " does not take 4K pages; not driven";
    } else if (c->scratchpads > SCRATCHPADS_MAX) {
        why = " asks for more scratchpad buffers than one page lists; not driven";
    } else if (c->slots == 0) {
        why = " has no device slots";
    }

    if (why == NULL) {
        region = kosmos_mem_create_flags(c->pages, MEM_CONTIGUOUS);
        mapped = region < 0 ? region : kosmos_mem_map(region);
        bus = region < 0 ? region : kosmos_mem_phys(region);

        if (region < 0 || mapped < 0 || bus <= 0) {
            why = " could not have memory it can reach";
        } else {
            c->mem = (uintptr_t)mapped;
            c->bus = (uint64_t)bus;

            c->ac64 = (hcc1 & HCC1_AC64) != 0;

            /* 5.3.6: without AC64 the controller ignores the high half. */
            if ((hcc1 & HCC1_AC64) == 0
                && c->bus + (uint64_t)c->pages * PAGE > 0x100000000ull) {
                why = " addresses 32 bits and its memory is above 4 GB; "
                      "not driven";
            }
        }
    }

    if (why != NULL) {
        about(line, c);
        say_text(line, why);
        say_send(console, line);
        return false;
    }

    /* The context array, and its entry 0 for the scratchpads (4.20, 6.6). */
    c->dcbaa = (uint64_t *)(c->mem + PAGE_DCBAA * PAGE);
    erst = (uint32_t *)(c->mem + PAGE_ERST * PAGE);
    scratch = (uint64_t *)(c->mem + PAGE_ERST * PAGE + SCRATCH_OFFSET);

    if (c->scratchpads > 0) {
        c->dcbaa[0] = c->bus + PAGE_ERST * PAGE + SCRATCH_OFFSET;

        for (i = 0; i < c->scratchpads; i++) {
            scratch[i] = c->bus + ((uint64_t)PAGE_DEVICES
                                   + DEVICES_MAX * PAGES_A_DEVICE + i) * PAGE;
        }
    }

    mmio_write32(c->op + OP_CONFIG,
                 (mmio_read32(c->op + OP_CONFIG) & ~0xFFu) | c->slots);
    write64(c->op + OP_DCBAAP, c->bus + PAGE_DCBAA * PAGE);

    ring_start(&c->commands, (uint32_t *)(c->mem + PAGE_COMMANDS * PAGE),
               c->bus + PAGE_COMMANDS * PAGE);
    write64(c->op + OP_CRCR, c->commands.bus | CRCR_RCS);

    /* The event ring: one segment, the table's one entry (6.5), then the
     * registers in 4.9.4's order. */
    c->events = (uint32_t *)(c->mem + PAGE_EVENTS * PAGE);
    c->events_bus = c->bus + PAGE_EVENTS * PAGE;
    c->dequeue = 0;
    c->event_cycle = TRB_C;

    erst[0] = (uint32_t)c->events_bus;
    erst[1] = (uint32_t)(c->events_bus >> 32);
    erst[2] = RING_TRBS;

    mmio_write32(c->rt + RT_ERSTSZ, 1u);
    event_dequeue_to(c, c->events_bus, false);
    write64(c->rt + RT_ERSTBA, c->bus + PAGE_ERST * PAGE);

    /*
     * The interrupt, if it can be had: a claim refused is a controller driven
     * by looking at its ring, which works, and is said.
     */
    c->intid = dev->intid;
    c->irq = kosmos_irq_claim(dev->intid);

    mmio_write32(c->rt + RT_IMOD, IMOD_ONE_MS);
    mmio_write32(c->rt + RT_IMAN, IMAN_IE | IMAN_IP);
    mmio_write32(c->op + OP_USBCMD,
                 mmio_read32(c->op + OP_USBCMD) | USBCMD_INTE | USBCMD_RS);

    if (!settles(c->op + OP_USBSTS, USBSTS_HCH, 0, ticks_for(HALT_MS))) {
        about(line, c);
        say_text(line, " would not run");
        say_send(console, line);
        return false;
    }

    about(line, c);
    say_text(line, " runs: contexts of ");
    say_dec(line, c->context);
    say_text(line, " bytes, ");
    say_dec(line, c->scratchpads);
    say_text(line, c->scratchpads == 1 ? " scratchpad page, "
                                       : " scratchpad pages, ");
    say_dec(line, c->slots);
    say_text(line, " slots enabled, ");

    if (c->irq >= 0) {
        say_text(line, "interrupt ");
        say_dec(line, c->intid);
    } else {
        say_text(line, "interrupt ");
        say_dec(line, c->intid);
        say_text(line, " not claimed, so polled");
    }

    say_send(console, line);
    return true;
}

/*
 * The first proof that the rings work: a No-Op command (4.6.2), answered on
 * the event ring - and whether its interrupt reached this process, which on
 * x86 is the first MSI to do so.
 *
 * **Whether it arrived is asked, not inferred from how the answer was
 * found.** QEMU completes a command inside the doorbell write, so the event
 * is on the ring before anybody looks, and a driver that credits only an
 * interrupt it waited for reports none while one sits pending on its line.
 * The first run did exactly that, with a probe in the kernel saying the
 * interrupt had been delivered to this claim. So once the answer is taken
 * the line is asked, with the same deadline: an interrupt that arrived is
 * there at once, and one that never did costs the second.
 */
static bool no_op(struct controller *c, struct say_line *line)
{
    uint32_t done[4];
    unsigned before = c->interrupts;
    bool answered = command(c, 0, 0, TRB_TYPE(TRB_NOOP), done);
    bool arrived = c->interrupts > before;

    if (answered && !arrived && c->irq >= 0) {
        arrived = kosmos_irq_wait_for(c->irq, ticks_for(ANSWER_MS)) == 0;
        acknowledge(c);

        if (arrived) {
            c->interrupts++;
        }
    }

    about(line, c);

    if (!answered) {
        say_text(line, " did not answer a No-Op command on its event ring");
        say_send(console, line);
        return false;
    }

    say_text(line, " answered a No-Op command on its event ring");

    if (arrived) {
        say_text(line, ", by interrupt");
    } else if (c->irq >= 0) {
        say_text(line, ", found by looking: no interrupt within a second");
    } else {
        say_text(line, ", found by looking");
    }

    say_send(console, line);
    return true;
}

/*
 * A USB 2 port reset (4.3.1): Port Reset written with Port Power and nothing
 * else - the change bits are write-1-to-clear and must not be echoed back -
 * then Port Reset Change awaited and cleared. Enabled after it, or not; and
 * when it is, USB 2.0's reset recovery before anything is asked of the device
 * (`RESET_RECOVERY_MS`).
 */
static bool reset_port(struct controller *c, unsigned port)
{
    uintptr_t reg = c->op + OP_PORTSC(port);
    unsigned long start = kosmos_ticks();

    mmio_write32(reg, PORTSC_PP | PORTSC_PR);

    while ((mmio_read32(reg) & PORTSC_PRC) == 0) {
        if (kosmos_ticks() - start >= counter_for(ANSWER_MS)) {
            return false;
        }

        (void)wait_serving(c, 1u, NULL);
    }

    mmio_write32(reg, PORTSC_PP | PORTSC_PRC);

    if ((mmio_read32(reg) & PORTSC_PED) == 0) {
        return false;
    }

    (void)wait_serving(c, RESET_RECOVERY_MS, NULL);
    return true;
}

/* ------------------------------------------------------------ a device */

/*
 * A completion code's name, for the ones Table 6-90 gives here legibly: 1 to 9.
 * The number is printed beside every one, so a code past those is still
 * exact, only unnamed. 0 is this driver's own: nothing answered in time.
 */
static const char *completion_name(uint32_t code)
{
    switch (code) {
    case 0u: return "no answer within a second";
    case 1u: return "Success";
    case 2u: return "Data Buffer Error";
    case 3u: return "Babble Detected Error";
    case 4u: return "USB Transaction Error";
    case 5u: return "TRB Error";
    case 6u: return "Stall Error";
    case 7u: return "Resource Error";
    case 8u: return "Bandwidth Error";
    case 9u: return "No Slots Available Error";
    case 19u: return "Context State Error";
    case 26u: return "Stopped";
    default: return "completion code";
    }
}

/*
 * "xhci: 00:14.0 port 7: Address Device failed: USB Transaction Error (4)",
 * with what attempt it was when there was more than one. A step whose command
 * succeeded and answered something unusable has no code to give, and its name
 * says the whole of it.
 */
static void say_failure(struct controller *c, unsigned port, const char *step,
                        const char *note, struct say_line *line)
{
    about(line, c);
    say_text(line, " port ");
    say_dec(line, port);
    say_text(line, ": ");
    say_text(line, step);

    if (c->last_code != CC_SUCCESS) {
        say_text(line, " failed: ");
        say_text(line, completion_name(c->last_code));

        if (c->last_code != 0) {
            say_text(line, " (");
            say_dec(line, c->last_code);
            say_text(line, ")");
        }
    }

    say_text(line, note);
    say_send(console, line);
}

/* Entry `index` of a context: 0 the input control context, 1 the slot, 2
 * endpoint 0 (6.2.5) - each 32 or 64 bytes. */
static uint32_t *context(const struct controller *c, uint32_t *base,
                         unsigned index)
{
    return base + index * (c->context / 4u);
}

/*
 * A slot given back (4.6.4). Its entry in the context array goes to 0, what an
 * unallocated slot's entry holds (6.1) - once the controller has said the slot
 * is disabled, because software shall not modify the entry of a slot that is
 * still enabled.
 */
static void disable_slot(struct controller *c, unsigned slot)
{
    uint32_t done[4];

    if (command(c, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(slot), done)) {
        c->dcbaa[slot] = 0;
    }
}

/*
 * One attempt at a slot and an address (4.3.2 to 4.3.4): Enable Slot, then an
 * input context naming the port and the default control endpoint, an output
 * context in the array, and Address Device. NULL when it worked, and otherwise
 * the step that failed, with its code in `last_code`.
 */
static const char *address_once(struct controller *c, struct device *d)
{
    uintptr_t page;
    uint32_t done[4];
    uint32_t *icc, *slot, *ep0;

    d->slot = 0;

    if (!command(c, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT)
                          | ((uint32_t)c->slot_type[d->port] << 16), done)) {
        return "Enable Slot";
    }

    d->slot = TRB_SLOT_OF(done[3]);

    /*
     * A slot past the ones enabled has no pages here, and should never come:
     * MaxSlotsEn makes slots 1 to that number the active ones (5.4.7). QEMU
     * gives one anyway - it looks for a free slot among all it has - so a
     * driver that kept its slots would meet this. It is given back, without
     * an array entry, which 6.1 sizes to the slots enabled; and the line says
     * what came, not a failure code for a command that succeeded.
     */
    if (d->slot == 0 || d->slot > c->slots) {
        if (d->slot != 0) {
            (void)command(c, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT)
                                   | TRB_SLOT(d->slot), done);
        }

        c->last_code = CC_SUCCESS;
        d->slot = 0;
        return "Enable Slot gave a slot past the ones enabled";
    }

    page = PAGE_DEVICES + (d->slot - 1u) * PAGES_A_DEVICE;

    d->input = (uint32_t *)(c->mem + page * PAGE);
    d->output = (uint32_t *)(c->mem + (page + 1u) * PAGE);
    d->buffer = (uint8_t *)(c->mem + (page + 3u) * PAGE);
    d->input_bus = c->bus + page * PAGE;
    d->output_bus = c->bus + (page + 1u) * PAGE;
    d->buffer_bus = c->bus + (page + 3u) * PAGE;

    memset(d->input, 0, PAGE);
    memset(d->output, 0, PAGE);
    ring_start(&d->ep0, (uint32_t *)(c->mem + (page + 2u) * PAGE),
               c->bus + (page + 2u) * PAGE);

    icc = context(c, d->input, 0);
    slot = context(c, d->input, 1);
    ep0 = context(c, d->input, 2);

    icc[1] = 0x3u;                          /* A0, A1: slot and endpoint 0 */

    /*
     * Context Entries 1, and the speed in 23:20 - which 1.2 calls deprecated
     * and reserved (Table 6-4), and which controllers written to earlier
     * revisions still read. The root port in 23:16 of the next word, and a
     * route string of 0 for a device on the root hub.
     */
    slot[0] = (1u << 27) | (d->speed << 20);
    slot[1] = d->port << 16;

    /* Control endpoint, three errors allowed, the default packet (6.2.3). */
    ep0[1] = (default_packet(d->speed) << 16) | (4u << 3) | (3u << 1);
    ep0[2] = (uint32_t)d->ep0.bus | 1u;     /* and DCS */
    ep0[3] = (uint32_t)(d->ep0.bus >> 32);
    ep0[4] = 8u;                            /* average TRB length, control */

    c->dcbaa[d->slot] = d->output_bus;

    if (!command(c, (uint32_t)d->input_bus, (uint32_t)(d->input_bus >> 32),
                 TRB_TYPE(TRB_ADDRESS_DEVICE) | TRB_SLOT(d->slot), done)) {
        return "Address Device";
    }

    /* The command sent SET_ADDRESS, and the device has 9.2.6.3's interval
     * before it answers to its new address. */
    (void)wait_serving(c, ADDRESS_MS, NULL);
    return NULL;
}

/*
 * ...and a second, once, when the first fails - the way 4.6.5's notes put it:
 * a failed Address Device leaves the slot in Default, and software may disable
 * it or reset the device and try again; a Transaction Error may be a stall,
 * for which Disable Slot then Enable Slot. So: the slot given back, the port
 * reset again if it is USB 2, RECOVERY_MS, and the whole attempt once more.
 *
 * **The first attempt waits only what USB 2.0 asks for** - the reset's
 * recovery and the address's - so that a photograph of the ThinkPad still
 * says which of the two a device needed.
 */
static bool address_device(struct controller *c, struct device *d,
                           struct say_line *line)
{
    const char *failed = address_once(c, d);

    if (failed == NULL) {
        return true;
    }

    say_failure(c, d->port, failed, "", line);

    if (d->slot != 0) {
        disable_slot(c, d->slot);
    }

    if (c->usb[d->port] == 2 && !reset_port(c, d->port)) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);
        say_text(line, ": would not reset for a second attempt");
        say_send(console, line);
        return false;
    }

    (void)wait_serving(c, RECOVERY_MS, NULL);
    d->speed = PORTSC_SPEED(mmio_read32(c->op + OP_PORTSC(d->port)));

    failed = address_once(c, d);

    /*
     * **And given back after the second failure too.** The port records no
     * slot for a device it could not address, so nothing later would disable
     * this one: the ThinkPad's two such ports would have held two of the
     * eight slots from boot, and a device that fails every time one more on
     * each plug.
     */
    if (failed != NULL) {
        say_failure(c, d->port, failed, ", again after a reset and a pause",
                    line);

        if (d->slot != 0) {
            disable_slot(c, d->slot);
        }

        return false;
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": addressed at a second attempt, after a reset and a pause");
    say_send(console, line);
    return true;
}

/*
 * The rest of a control transfer once its stages are on the ring: endpoint
 * 0's doorbell, and its events until the Status stage's. True when that
 * succeeded. A stage before it that failed ends the wait; a short Data stage
 * is no error - a descriptor says how long it is.
 *
 * Endpoint 0's events only: a mouse's report, from this device or any other,
 * is read as it comes (`wait_serving`).
 */
static bool control_wait(struct controller *c, struct device *d,
                         uint64_t status)
{
    uint32_t done[4];
    unsigned seen;

    mmio_write32(c->doorbells + 4u * d->slot, 1u);  /* target 1: endpoint 0 */
    c->last_code = 0;

    for (seen = 0; seen < EVENTS_MAX; seen++) {
        uint32_t code;

        if (!wait_serving(c, ANSWER_MS, done)) {
            return false;
        }

        if (TRB_TYPE_OF(done[3]) != TRB_TRANSFER
            || TRB_SLOT_OF(done[3]) != d->slot
            || TRB_ENDPOINT_OF(done[3]) != 1u) {
            continue;
        }

        code = TRB_CODE_OF(done[2]);
        c->last_code = code;

        if ((((uint64_t)done[1] << 32) | done[0]) == status) {
            return code == CC_SUCCESS;
        }

        if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
            return false;
        }
    }

    return false;
}

/*
 * A control transfer that reads (4.11.2.2, 6.4.1.2): Setup with its eight
 * bytes inline, an IN Data stage into the device's buffer, and a Status stage
 * - OUT, the other way from the data (Table 4-7) - that interrupts on
 * completion.
 */
static bool control_in(struct controller *c, struct device *d,
                       uint32_t request, uint16_t value, uint16_t index,
                       uint16_t length)
{
    uint64_t status;

    memset(d->buffer, 0, length);

    (void)ring_push(&d->ep0, request | ((uint32_t)value << 16),
                    index | ((uint32_t)length << 16), 8u,
                    TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT_IN);
    (void)ring_push(&d->ep0, (uint32_t)d->buffer_bus,
                    (uint32_t)(d->buffer_bus >> 32), length,
                    TRB_TYPE(TRB_DATA) | TRB_DIR_IN);
    status = ring_push(&d->ep0, 0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);

    return control_wait(c, d, status);
}

/*
 * A control transfer with nothing to read or write - SET_CONFIGURATION,
 * HID's SET_PROTOCOL, CLEAR_FEATURE and Bulk-Only's reset: Setup with a
 * length of 0 and a Transfer Type of no data stage, which is 0 (Table 6-26),
 * then a Status stage that is IN, because with no data stage it is the
 * device that answers (Table 4-7).
 */
static bool control_nodata(struct controller *c, struct device *d,
                           uint32_t request, uint16_t value, uint16_t index)
{
    uint64_t status;

    (void)ring_push(&d->ep0, request | ((uint32_t)value << 16), index, 8u,
                    TRB_TYPE(TRB_SETUP) | TRB_IDT);
    status = ring_push(&d->ep0, 0, 0, 0,
                       TRB_TYPE(TRB_STATUS) | TRB_DIR_IN | TRB_IOC);

    return control_wait(c, d, status);
}

/*
 * Requests, as bmRequestType and bRequest in the Setup stage's first word
 * (Table 9-2). GET_DESCRIPTOR and SET_CONFIGURATION are standard requests to
 * the device (9.4.3, 9.4.7, Table 9-4); SET_PROTOCOL is HID's, a class
 * request to an interface (HID 1.11 7.2.6, its bRequest from 7.2's table).
 * A descriptor's type is wValue's high byte (Table 9-5). A HID class
 * descriptor is GET_DESCRIPTOR asked of the interface instead - 10000001,
 * the interface in wIndex - and the Report descriptor is its type 0x22
 * (HID 1.11 7.1, 7.1.1). CLEAR_FEATURE to an endpoint is 00000010, with
 * ENDPOINT_HALT in wValue and the endpoint's address in wIndex (9.4.1, Table
 * 9-6); Bulk-Only Transport's class reset is 00100001 and FFh, to the stick's
 * interface (Bulk-Only 1.0 3.1).
 */
#define GET_DESCRIPTOR      (0x80u | (6u << 8))
#define GET_HID_DESCRIPTOR  (0x81u | (6u << 8))
#define SET_CONFIGURATION   (0x00u | (9u << 8))
#define SET_PROTOCOL        (0x21u | (0x0Bu << 8))
#define CLEAR_FEATURE       (0x02u | (1u << 8))
#define MASS_STORAGE_RESET  (0x21u | (0xFFu << 8))
/*
 * SET_INTERFACE is a standard request to an *interface* - 00000001, the
 * setting in wValue and the interface in wIndex (9.4.9) - and it is what
 * takes an ECM Data interface off its empty setting 0.
 * SET_ETHERNET_PACKET_FILTER is CDC's class request to the Communications
 * interface, 00100001 and 43h, the filter bits in wValue (ECM 6.2.4).
 */
#define SET_INTERFACE       (0x01u | (11u << 8))
#define GET_INTERFACE       (0x81u | (10u << 8))
#define SET_ETH_FILTER      (0x21u | (0x43u << 8))
#define DESC_DEVICE         0x0100u
#define DESC_CONFIGURATION  0x0200u
#define DESC_STRING         0x0300u
#define DESC_REPORT         0x2200u
#define PROTOCOL_BOOT       0u          /* 7.2.6: 0 boot, 1 report */
#define PROTOCOL_REPORT     1u
#define ENDPOINT_HALT       0u

/*
 * A string descriptor as ASCII: UTF-16LE after a two-byte header, anything
 * that is not printable ASCII shown as '?', and no longer than a line has
 * room for.
 */
static void as_ascii(char *out, const uint8_t *desc)
{
    unsigned chars = desc[0] >= 2u ? (unsigned)(desc[0] - 2u) / 2u : 0u;
    unsigned i;

    if (chars > NAME_MAX_CHARS) {
        chars = NAME_MAX_CHARS;
    }

    for (i = 0; i < chars; i++) {
        unsigned code = desc[2u + 2u * i] | (unsigned)desc[3u + 2u * i] << 8;

        out[i] = (code >= 0x20u && code < 0x7Fu) ? (char)code : '?';
    }

    out[i] = '\0';
}

/*
 * What it is and who made it: the device descriptor's first eight bytes, the
 * packet size corrected for a full-speed device (4.3.4, step 7a), the whole
 * eighteen, and the product string in the first language the device offers.
 */
static bool describe(struct controller *c, struct device *d,
                     struct say_line *line)
{
    char name[NAME_MAX_CHARS + 1];
    unsigned vendor, product, bcd, class, iproduct;

    name[0] = '\0';

    if (!control_in(c, d, GET_DESCRIPTOR, DESC_DEVICE, 0, 8u)) {
        say_failure(c, d->port, "GET_DESCRIPTOR for 8 bytes", "", line);
        return false;
    }

    if (d->speed == 1 && d->buffer[7] != default_packet(d->speed)
        && (d->buffer[7] == 16u || d->buffer[7] == 32u
            || d->buffer[7] == 64u)) {
        uint32_t done[4];
        uint32_t *icc = context(c, d->input, 0);
        uint32_t *ep0 = context(c, d->input, 2);

        icc[0] = 0;
        icc[1] = 0x2u;                      /* A1: endpoint 0 alone */
        ep0[1] = (ep0[1] & 0xFFFFu) | ((uint32_t)d->buffer[7] << 16);

        if (!command(c, (uint32_t)d->input_bus,
                     (uint32_t)(d->input_bus >> 32),
                     TRB_TYPE(TRB_EVALUATE) | TRB_SLOT(d->slot), done)) {
            say_failure(c, d->port, "Evaluate Context", "", line);
            return false;
        }
    }

    if (!control_in(c, d, GET_DESCRIPTOR, DESC_DEVICE, 0, 18u)) {
        say_failure(c, d->port, "GET_DESCRIPTOR for 18 bytes", "", line);
        return false;
    }

    if (d->buffer[0] < 18u || d->buffer[1] != 1u) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);
        say_text(line, ": answered with something that is not a device "
                       "descriptor");
        say_send(console, line);
        return false;
    }

    bcd = d->buffer[2] | (unsigned)d->buffer[3] << 8;
    class = d->buffer[4];
    d->configurations = d->buffer[17];
    vendor = d->buffer[8] | (unsigned)d->buffer[9] << 8;
    product = d->buffer[10] | (unsigned)d->buffer[11] << 8;
    iproduct = d->buffer[15];

    if (iproduct != 0
        && control_in(c, d, GET_DESCRIPTOR, DESC_STRING, 0, 255u)
        && d->buffer[0] >= 4u && d->buffer[1] == 3u) {
        uint16_t language = (uint16_t)(d->buffer[2]
                                       | (unsigned)d->buffer[3] << 8);

        d->language = language;

        if (control_in(c, d, GET_DESCRIPTOR,
                       (uint16_t)(DESC_STRING | iproduct), language, 255u)
            && d->buffer[1] == 3u) {
            as_ascii(name, d->buffer);
        }
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": ");
    say_hex(line, vendor, 4);
    say_text(line, ":");
    say_hex(line, product, 4);
    say_text(line, ", USB ");
    say_dec(line, ((bcd >> 12) & 0xFu) * 10u + ((bcd >> 8) & 0xFu));
    say_text(line, ".");
    say_dec(line, (bcd >> 4) & 0xFu);
    say_text(line, ", class ");
    say_dec(line, class);

    if (name[0] != '\0') {
        say_text(line, ", \"");
        say_text(line, name);
        say_text(line, "\"");
    }

    say_send(console, line);
    c->named++;

    c->described[d->slot] = true;
    c->vendor[d->slot] = (uint16_t)vendor;
    c->product[d->slot] = (uint16_t)product;
    memcpy(c->name[d->slot], name, sizeof(c->name[d->slot]));

    return true;
}

/* ---------------------------------------------------------------- a mouse */

/*
 * An endpoint descriptor's bInterval as an endpoint context's Interval, a
 * power of two in steps of 125 microseconds (6.2.3.6, Table 6-12). A full- or
 * low-speed device says milliseconds, 1 to 255, and the power is rounded down
 * so the endpoint is asked at least as often as it said: 3 to 10. A high-speed
 * device says the power already, plus one.
 */
static unsigned interval_for(unsigned speed, unsigned interval)
{
    unsigned power = 3u;                    /* 8 x 125 us: a millisecond */

    if (speed == 1u || speed == 2u) {
        unsigned steps = (interval == 0u ? 1u : interval) * 8u;

        while (power < 10u && (2u << power) <= steps) {
            power++;
        }

        return power;
    }

    if (interval < 1u) {
        interval = 1u;
    }

    return (interval > 16u ? 16u : interval) - 1u;
}

/*
 * Every mouse's buttons, on every controller, together. The pointer holds this
 * driver's buttons as one source's (`hal_pointer_move`), so a release on one
 * mouse must not let go of a button another is holding down.
 */
static uint32_t all_buttons(void)
{
    uint32_t held = 0;
    unsigned i, slot;

    for (i = 0; i < controllers_found; i++) {
        for (slot = 1; slot <= DEVICES_MAX; slot++) {
            held |= controllers[i].mouse[slot].buttons;
        }
    }

    return held;
}

/*
 * A movement and every mouse's buttons, to the pointer. Said once if the
 * pointer will not take them - on a board whose pointer is a tablet - and
 * after that only tried.
 */
static void to_pointer(int dx, int dy)
{
    static bool said;
    long refused = kosmos_pointer_move(dx, dy, all_buttons());
    struct say_line line;

    if (refused == 0 || said) {
        return;
    }

    said = true;
    say_begin(&line);
    say_text(&line, refused == SYS_ERR_NO_DEVICE
                    ? "xhci: this machine's pointer is absolute, and a "
                      "mouse's movement is not added to it"
                    : "xhci: the kernel would not take a mouse's movement");
    say_send(console, &line);
}

/*
 * A request for one report (6.4.1.1): a Normal TRB over the mouse's report
 * page, as long as its packet, that interrupts when it completes and when a
 * report comes back shorter - then the endpoint's doorbell, whose target is
 * its context index, as endpoint 0's is 1.
 */
static void ask_for_report(struct controller *c, unsigned slot)
{
    struct mouse *m = &c->mouse[slot];

    (void)ring_push(&m->ring, (uint32_t)m->report_bus,
                    (uint32_t)(m->report_bus >> 32), m->length,
                    TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    mmio_write32(c->doorbells + 4u * slot, m->dci);
}

/* A report's X or Y: a signed byte, -127 to 127 (HID 1.11 B.2). */
static int count_of(uint8_t byte)
{
    return byte < 0x80u ? (int)byte : (int)byte - 256;
}

static void say_count(struct say_line *line, int n)
{
    if (n < 0) {
        say_text(line, "-");
    }

    say_dec(line, (unsigned long)(n < 0 ? -n : n));
}

/*
 * What one report says: by the mouse's Report descriptor when it gave one
 * that `usb_decode.c` could lay out, and otherwise as a boot report (HID 1.11
 * B.2) - byte 0 the buttons, bytes 1 and 2 the movement. False for a report
 * about something else, under another Report ID (6.2.2.7), and for a boot
 * report too short to be one.
 *
 * Buttons 1 to 3 are the pointer's, in the order the boot report has them:
 * primary, secondary, tertiary. A field past what arrived reads as 0.
 */
static bool read_report(const struct mouse *m, unsigned got,
                        uint32_t *buttons, int *dx, int *dy)
{
    const struct usb_mouse_report *l = &m->layout;
    const uint8_t *r = m->report;

    if (!l->ok) {
        if (got < 3u) {
            return false;
        }

        *buttons = r[0] & 0x07u;
        *dx = count_of(r[1]);
        *dy = count_of(r[2]);
        return true;
    }

    if (l->id != 0) {
        if (got == 0 || r[0] != l->id) {
            return false;
        }

        r++;
        got--;
    }

    *buttons = (uint32_t)usb_report_field(r, got, l->buttons_at,
                                          l->buttons < 3u ? l->buttons : 3u,
                                          false) & 0x07u;
    *dx = usb_report_field(r, got, l->x_at, l->x_bits, l->x_signed);
    *dy = usb_report_field(r, got, l->y_at, l->y_bits, l->y_signed);
    return true;
}

/*
 * **A game controller's report, as keys.** What programs see down - the
 * pad's buttons, its triggers past half-way, its left stick as the D-pad
 * (`pad_pressed`) - is compared with what they were last told, and each
 * change goes to the kernel as a key pressed or let go (`SYS_KEY_PUSH`),
 * which puts it in the focused window's events beside the keyboard's.
 *
 * The first thirty-two changes are said: enough for a photograph of the
 * ThinkPad to show which button arrived as which, and not a line for every
 * press of a game.
 */
/*
 * A message to an Xbox One pad: into the next 64-byte slot of its page, a
 * Normal TRB for it on the OUT ring, and the OUT endpoint's doorbell. Not
 * waited for - the pad's answer is in what it sends next - and a slot is
 * reused only after sixty-three more, far past the one or two in flight.
 */
static void pad_say(struct controller *c, unsigned slot, const uint8_t *bytes,
                    unsigned length)
{
    struct mouse *m = &c->mouse[slot];
    unsigned at = (m->say_next++ % (PAGE / PAD_SAY_SLOT)) * PAD_SAY_SLOT;

    memcpy(m->say + at, bytes, length);
    (void)ring_push(&m->out_ring, (uint32_t)(m->say_bus + at),
                    (uint32_t)((m->say_bus + at) >> 32), length,
                    TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    mmio_write32(c->doorbells + 4u * slot, m->out_dci);
}

/* What the host says first (`pad_xone_init`), each with the next sequence. */
static void pad_start(struct controller *c, unsigned slot)
{
    struct mouse *m = &c->mouse[slot];
    uint8_t bytes[16];
    unsigned step, length;

    for (step = 0; step < PAD_XONE_STEPS; step++) {
        length = pad_xone_init(m->vendor, m->product, step, m->seq, bytes);

        if (length != 0) {
            pad_say(c, slot, bytes, length);
            m->seq++;
        }
    }
}

static void pad_keys(struct controller *c, struct mouse *m, uint32_t now)
{
    uint32_t changed = now ^ m->pressed;
    struct say_line line;
    unsigned bit;

    for (bit = 0; bit < PAD_BITS; bit++) {
        bool down = (now & (1u << bit)) != 0;

        if ((changed & (1u << bit)) == 0) {
            continue;
        }

        if (kosmos_key_push(pad_codes[bit], down) != 0) {
            continue;                   /* refused: told again next change */
        }

        m->pressed ^= 1u << bit;

        if (m->said < 32u) {
            m->said++;
            about(&line, c);
            say_text(&line, " port ");
            say_dec(&line, m->port);
            say_text(&line, ": the pad's ");
            say_text(&line, pad_names[bit]);
            say_text(&line, down ? " down" : " up");
            say_send(console, &line);
        }
    }
}

static void pad_report(struct controller *c, unsigned slot, unsigned got)
{
    struct mouse *m = &c->mouse[slot];
    struct pad_state s;

    if (m->xone) {
        uint8_t bytes[16], seq = 0;
        bool ack = false;
        enum pad_xone_kind kind = pad_decode_xone(m->report, got, &m->state,
                                                  &ack, &seq);

        if (ack) {
            pad_say(c, slot, bytes, pad_xone_ack(seq, bytes));
        }

        if (kind == PAD_XONE_ANNOUNCE && !m->announced) {
            m->announced = true;
            pad_start(c, slot);
            return;
        }

        if (kind != PAD_XONE_INPUT && kind != PAD_XONE_GUIDE) {
            return;
        }

        s = m->state;
    } else if (!pad_decode_x360(m->report, got, &s)) {
        return;                         /* its lights' status, not input */
    }

    if (++m->reports == 1u) {
        struct say_line line;

        about(&line, c);
        say_text(&line, " port ");
        say_dec(&line, m->port);
        say_text(&line, ": the pad's first report: buttons ");
        say_hex(&line, s.buttons, 4);
        say_text(&line, ", left stick ");
        say_count(&line, s.lx);
        say_text(&line, ",");
        say_count(&line, s.ly);
        say_send(console, &line);
    }

    pad_keys(c, m, pad_pressed(&s, m->pressed));
}

/*
 * A report, read by `read_report`. The movement and the
 * buttons go to the pointer when there is something new in them, and the
 * next request goes on the ring whatever there was. The first report is said,
 * which is how a photograph of the ThinkPad tells a mouse that sends nothing
 * from a pointer that does nothing with what it sends.
 *
 * **A report that failed stops the mouse**, and says so. The endpoint is
 * halted by then (4.10.2.1, 4.10.2.3), and bringing it back is a Reset
 * Endpoint, a CLEAR_FEATURE to the device and a new dequeue pointer - none of
 * which QEMU's mouse can be made to need, so none of which a test here could
 * reach. A mouse unplugged and plugged in again is read from the start.
 */
/*
 * **What this keyboard has told the system is down, let go of.**
 *
 * A keyboard unplugged with Control held is a machine holding Control for
 * ever: nothing else will ever send the release, because the thing that
 * would have is gone. So the driver keeps what it pushed and undoes it -
 * the same reason the pad lets go of its buttons and the mouse of its own.
 */
static void keyboard_release(struct mouse *m)
{
    unsigned i;

    for (i = 0; i < m->holding; i++) {
        (void)kosmos_key_push(m->held[i], 0);
    }

    m->holding = 0;
    memset(m->was, 0, sizeof(m->was));
}

static void keyboard_hold(struct mouse *m, uint16_t code, bool down)
{
    unsigned i;

    for (i = 0; i < m->holding; i++) {
        if (m->held[i] == code) {
            if (!down) {
                m->held[i] = m->held[--m->holding];
            }

            return;
        }
    }

    if (down && m->holding < sizeof(m->held) / sizeof(m->held[0])) {
        m->held[m->holding++] = code;
    }
}

/*
 * **One report off a boot keyboard, as changes.**
 *
 * `usb_decode_keys` does the comparing - it is arithmetic over two
 * eight-byte reports and lives where a host test can hold it - and this
 * pushes what it says. Eight modifiers and six slots cannot produce more
 * than fourteen changes at once.
 *
 * The first report is read against all-up, so a keyboard plugged in with a
 * key already held says so rather than saying nothing until it is let go.
 */
static void keyboard_report(struct controller *c, unsigned slot, unsigned got)
{
    struct mouse *m = &c->mouse[slot];
    struct usb_key_change changes[14];
    struct say_line line;
    unsigned n, i;

    n = usb_decode_keys(m->report, got, m->was, sizeof(m->was),
                        changes, sizeof(changes) / sizeof(changes[0]));

    for (i = 0; i < n; i++) {
        keyboard_hold(m, changes[i].code, changes[i].down != 0);
        (void)kosmos_key_push(changes[i].code, changes[i].down);
    }

    if (n > 0 && m->keys == 0) {
        about(&line, c);
        say_text(&line, " port ");
        say_dec(&line, m->port);
        say_text(&line, ": the keyboard's first key: ");
        say_dec(&line, changes[0].code);
        say_text(&line, changes[0].down ? " down" : " up");
        say_send(console, &line);
    }

    m->keys += n;

    if (got >= sizeof(m->was)) {
        memcpy(m->was, m->report, sizeof(m->was));
    }
}

static void take_report(struct controller *c, const uint32_t *event)
{
    unsigned slot = TRB_SLOT_OF(event[3]);
    struct say_line line;
    struct mouse *m;
    uint32_t code, left, buttons;
    int dx, dy;

    if (TRB_TYPE_OF(event[3]) != TRB_TRANSFER || slot == 0
        || slot > DEVICES_MAX) {
        return;
    }

    m = &c->mouse[slot];

    if (!m->reading || TRB_ENDPOINT_OF(event[3]) != m->dci) {
        return;
    }

    code = TRB_CODE_OF(event[2]);
    left = TRB_LEFT_OF(event[2]);

    if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
        bool held = m->buttons != 0;

        m->reading = false;
        m->buttons = 0;

        if (held) {
            to_pointer(0, 0);
        }

        if (m->pad) {
            pad_keys(c, m, 0);          /* let go of whatever it held */
        }

        if (m->keyboard) {
            keyboard_release(m);
        }

        about(&line, c);
        say_text(&line, " port ");
        say_dec(&line, m->port);
        say_text(&line, m->pad ? ": the pad's report failed: "
                               : ": the mouse's report failed: ");
        say_text(&line, completion_name(code));
        say_text(&line, " (");
        say_dec(&line, code);
        say_text(&line, "); not read again until it is plugged in again");
        say_send(console, &line);
        return;
    }

    if (m->keyboard) {
        if (left < m->length) {
            keyboard_report(c, slot, m->length - left);
        }

        ask_for_report(c, slot);
        return;
    }

    if (m->pad) {
        if (left < m->length) {
            pad_report(c, slot, m->length - left);
        }

        ask_for_report(c, slot);
        return;
    }

    if (left < m->length
        && read_report(m, m->length - left, &buttons, &dx, &dy)) {
        if (++m->reports == 1u) {
            about(&line, c);
            say_text(&line, " port ");
            say_dec(&line, m->port);
            say_text(&line, ": the mouse's first report: buttons ");
            say_dec(&line, buttons);
            say_text(&line, ", moved ");
            say_count(&line, dx);
            say_text(&line, ",");
            say_count(&line, dy);
            say_send(console, &line);
        }

        if (!c->woken) {
            m->looked++;
        }

        if (dx != 0 || dy != 0 || buttons != m->buttons) {
            m->buttons = buttons;
            to_pointer(dx, dy);
        }
    }

    ask_for_report(c, slot);
}

/*
 * A Report descriptor's bytes, thirty-two to a line and four lines at most.
 * What a mouse declared is what turns the next one read wrongly into a
 * reading rather than a guess: the ThinkPad's gaming mouse was worked out
 * from a photograph of its first report, and this would have said it.
 */
static void say_descriptor(const struct controller *c, unsigned port,
                           const uint8_t *bytes, unsigned length,
                           struct say_line *line)
{
    unsigned from, i;

    for (from = 0; from < length && from < 128u; from += 32u) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, port);

        if (from == 0) {
            say_text(line, ": its Report descriptor, ");
            say_dec(line, length);
            say_text(line, " bytes:");
        } else {
            say_text(line, ":   from byte ");
            say_dec(line, from);
            say_text(line, ":");
        }

        for (i = from; i < length && i < from + 32u; i++) {
            say_text(line, " ");
            say_hex(line, bytes[i], 2u);
        }

        if (from + 32u >= 128u && length > 128u) {
            say_text(line, " ...");
        }

        say_send(console, line);
    }
}

/* ---------------------------------------------------------------- a stick */

/*
 * Bulk-Only Transport's two wrappers and SCSI's command blocks are laid out,
 * and what comes back is read, in `storage_decode.c` - where
 * `tools/test_storagedecode.c` hands them what QEMU's stick never sends.
 *
 * What stays here is where INQUIRY's standard data keeps its fields, as the
 * Seagate SCSI Commands Reference Manual (rev. J, SPC-5) gives them: at least
 * 36 bytes (Table 59), the peripheral qualifier and device type in byte 0,
 * then eight bytes of vendor, sixteen of product and four of revision, in
 * ASCII padded with spaces. And how long a stick is given to become ready,
 * and the largest block read - one page, because a block comes through the
 * device's buffer page.
 */
#define INQUIRY_LENGTH      36u
#define INQUIRY_VENDOR      8u
#define INQUIRY_PRODUCT     16u
#define INQUIRY_REVISION    32u
#define READY_TRIES         8u
#define READY_WAIT_MS       250u        /* between tries, when NOT READY */
#define BLOCK_MOST          4096u

/*
 * A bulk endpoint's context (4.8.2.3): Bulk OUT or Bulk IN from Table 6-9,
 * three errors allowed, its packet and - at SuperSpeed - its burst, its ring,
 * and no streams. 4.14.1.1 gives three kilobytes as a reasonable first
 * Average TRB Length for one.
 */
#define EP_TYPE_BULK_OUT    2u
#define EP_TYPE_BULK_IN     6u
#define BULK_TRB_AVERAGE    3072u

/*
 * One bulk transfer (4.11.2.1, 6.4.1.1): a Normal TRB over `length` bytes at
 * `bus` on a stick's ring - interrupting when it completes, and when an IN
 * comes back short - then the endpoint's doorbell, its context index the
 * target, and the Transfer Event for that TRB. `moved` is how many bytes
 * went: the length, less what the event says was left. True for Success and
 * for a short IN; anything else leaves the code in `last_code` - and after a
 * stall or a transaction error the endpoint halted, which `reset_pipe` clears.
 */
static bool bulk(struct controller *c, unsigned slot, unsigned dci,
                 struct ring *r, uint64_t bus, unsigned length,
                 unsigned *moved)
{
    uint64_t at = ring_push(r, (uint32_t)bus, (uint32_t)(bus >> 32), length,
                            TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    uint32_t done[4];
    unsigned seen;

    mmio_write32(c->doorbells + 4u * slot, dci);
    c->last_code = 0;
    *moved = 0;

    for (seen = 0; seen < EVENTS_MAX; seen++) {
        uint32_t left;

        if (!wait_serving(c, ANSWER_MS, done)) {
            return false;
        }

        if (TRB_TYPE_OF(done[3]) != TRB_TRANSFER
            || TRB_SLOT_OF(done[3]) != slot
            || TRB_ENDPOINT_OF(done[3]) != dci
            || (((uint64_t)done[1] << 32) | done[0]) != at) {
            continue;
        }

        c->last_code = TRB_CODE_OF(done[2]);

        if (c->last_code != CC_SUCCESS && c->last_code != CC_SHORT_PACKET) {
            return false;
        }

        left = TRB_LEFT_OF(done[2]);
        *moved = left < length ? length - left : 0;
        return true;
    }

    return false;
}

/*
 * **A stick's transfer buffer**: `TRANSFER_PAGES` in one physical run, mapped
 * here and told to the controller by its bus address - the same three calls
 * that give a controller its memory, and the same refusal for a controller
 * that cannot reach above 4 GB when the run is there (xHCI 1.2 5.3.6).
 */
#define TRANSFER_PAGES      32u         /* 128 KB: BLOCK_TRANSFER_MOST fits */

static bool stick_buffer(const struct controller *c, struct stick *s)
{
    long region = kosmos_mem_create_flags(TRANSFER_PAGES, MEM_CONTIGUOUS);
    long mapped = region < 0 ? region : kosmos_mem_map(region);
    long bus = mapped < 0 ? mapped : kosmos_mem_phys(region);

    if (region >= 0 && (mapped < 0 || bus <= 0
                        || (!c->ac64 && (uint64_t)bus
                            + (uint64_t)TRANSFER_PAGES * PAGE
                            > 0x100000000ull))) {
        if (mapped >= 0) {
            (void)kosmos_share_unmap((unsigned long)mapped, TRANSFER_PAGES);
        }

        (void)kosmos_cap_drop(region);
        return false;
    }

    if (region < 0) {
        return false;
    }

    s->transfer_cap = region;
    s->transfer = (uintptr_t)mapped;
    s->transfer_bus = (uint64_t)bus;
    return true;
}

/* Unmapped before the capability goes, as `sys.release` does it and for its
 * reason: dropped first, the pages could be freed under this mapping. */
static void stick_release_buffer(struct stick *s)
{
    if (s->transfer != 0) {
        (void)kosmos_share_unmap((unsigned long)s->transfer, TRANSFER_PAGES);
        (void)kosmos_cap_drop(s->transfer_cap);
        s->transfer = 0;
    }
}

/*
 * The step a command stopped at, for `say_failure`: "the READ (10)'s data".
 * One at a time - the driver has one thread - so one buffer does.
 */
static const char *named(const char *command, const char *part)
{
    static char step[80];
    const char *pieces[3] = { "the ", command, part };
    size_t at = 0;
    unsigned i;

    for (i = 0; i < 3u; i++) {
        size_t n = strlen(pieces[i]);

        if (n > sizeof(step) - 1u - at) {
            n = sizeof(step) - 1u - at;
        }

        memcpy(step + at, pieces[i], n);
        at += n;
    }

    step[at] = '\0';
    return step;
}

/*
 * **One SCSI command, through Bulk-Only Transport** (5.1 to 5.3.3), once: its
 * wrapper out; when it expects data, up to `length` bytes in, copied to
 * `into`, with how many came in `got`; then its status in, held to what 6.3
 * asks of a host by `bot_status_of`.
 *
 * NULL when the status says what happened - `*status` passed, or failed, which
 * REQUEST SENSE can explain - and otherwise the step that did not, for
 * `say_failure`. The data comes through the stick's transfer buffer and the
 * status through the device's page, so neither is written over the other;
 * the data is copied to `into` when there is one, and otherwise left in the
 * transfer buffer for the caller. With `out` it goes the other way: from the
 * transfer buffer, where the caller has put it, to the stick's bulk OUT
 * endpoint - all of it, or the data phase failed.
 *
 * **`spoil` is a test's, and nothing else's.** QEMU's stick stalls nothing a
 * driver sends it well, and nothing in QEMU can make it; a wrapper with the
 * wrong signature it stalls at once (`hw/usb/dev-storage.c`). So a machine
 * started with `opt/kosmos/stickfault=signature` sends each stick's first
 * wrapper with its signature's first byte turned over, and `run_x86.py`'s
 * `usb` watches the recovery. The driver says so when it takes the option, so
 * a log with that stall in it also says why.
 */
static const char *transact_once(struct controller *c, struct device *d,
                                 struct stick *s, const char *command,
                                 const uint8_t *cdb, unsigned cdb_length,
                                 bool out, uint8_t *into, unsigned length,
                                 unsigned *got, enum bot_status *status)
{
    uint8_t *b = d->buffer;
    uint32_t tag = ++s->tag, residue;
    unsigned moved;

    *got = 0;
    *status = BOT_NOT_VALID;
    (void)bot_wrap(b, tag, length, length > 0u && !out, 0, cdb, cdb_length);

    if (s->spoil) {
        b[0] ^= 0xFFu;
        s->spoil = false;
    }

    if (!bulk(c, d->slot, s->out_dci, &s->out, d->buffer_bus, BOT_CBW_LENGTH,
              &moved) || moved != BOT_CBW_LENGTH) {
        return named(command, "'s command");
    }

    if (length > 0u && out) {
        if (!bulk(c, d->slot, s->out_dci, &s->out, s->transfer_bus, length,
                  got) || *got != length) {
            return named(command, "'s data");
        }
    } else if (length > 0u) {
        memset((void *)s->transfer, 0, length);

        if (!bulk(c, d->slot, s->in_dci, &s->in, s->transfer_bus, length,
                  got)) {
            return named(command, "'s data");
        }

        if (into != NULL) {
            memcpy(into, (void *)s->transfer, length);
        }
    }

    if (!bulk(c, d->slot, s->in_dci, &s->in, d->buffer_bus, BOT_CSW_LENGTH,
              &moved)) {
        return named(command, "'s status");
    }

    c->last_code = CC_SUCCESS;
    *status = bot_status_of(b, moved, tag, length, &residue);

    switch (*status) {
    case BOT_PASSED:
    case BOT_FAILED:
        return NULL;
    case BOT_PHASE_ERROR:
        return named(command, ", which ended in a phase error");
    case BOT_NOT_MEANINGFUL:
        return named(command, "'s status, which is not a meaningful one");
    default:
        return named(command, "'s status, which is not a valid one");
    }
}

/*
 * xHCI 1.2 6.2.3: an Endpoint Context's EP State, bits 2:0 of its first
 * dword, which the controller keeps in the device's output context. The
 * values are the specification's - read from FreeBSD's `xhci.h`, which
 * states them, on 22 September, when Intel's copy of the specification
 * would not download.
 */
#define EP_STATE_MASK       0x7u
#define EP_STATE_RUNNING    1u
#define EP_STATE_HALTED     2u
#define EP_STATE_STOPPED    3u
#define EP_STATE_ERROR      4u

/* What the controller says an endpoint is in, as of its last command. */
static unsigned ep_state(const struct controller *c, struct device *d,
                         unsigned dci)
{
    const volatile uint32_t *ep = context(c, d->output, dci);

    return ep[0] & EP_STATE_MASK;
}

/*
 * **One endpoint made usable again**, from whatever a failure left it in, by
 * xHCI 1.2 4.6.8's "reset a pipe": the endpoint Stopped - with Stop Endpoint
 * if it is Running (4.6.9), Reset Endpoint if it is Halted - then the
 * device's halt cleared, ENDPOINT_HALT to the endpoint's address (USB 2.0
 * 9.4.1), and the controller's dequeue pointer moved to where the next TRB
 * will go, with the cycle bit it will carry (4.6.10) - so nothing left on
 * the ring from before is tried again. NULL when it worked, and otherwise the
 * step.
 *
 * **By the state the controller says the endpoint is in**, and not by
 * trying Reset Endpoint and reading its refusal. That was this, and it was
 * wrong in one order of events: Reset Endpoint refused because the endpoint
 * was Running, then the stick stalled it before Stop Endpoint arrived, so
 * Stop was refused too - which this read as "already stopped" - and Set TR
 * Dequeue Pointer, which takes only Stopped or Error, found a Halted
 * endpoint. On the ThinkPad on 22 September: "the bulk OUT's Set TR Dequeue
 * Pointer failed: Context State Error (19)", on the pipe every write to the
 * stick goes through, minutes before Appearance could not save to it.
 *
 * So the state is read, acted on, and read again - a stall can land while
 * an endpoint is being stopped - and Set TR Dequeue Pointer is sent only to
 * an endpoint that is Stopped or Error, or the failure says which it was.
 */
static const char *reset_pipe(struct controller *c, struct device *d,
                              struct ring *r, unsigned dci, uint8_t address,
                              const char *which)
{
    uint64_t next = r->bus + (uint64_t)r->enqueue * 16u;
    uint32_t target = TRB_ENDPOINT(dci) | TRB_SLOT(d->slot);
    uint32_t done[4];
    unsigned state = ep_state(c, d, dci);

    if (state == EP_STATE_RUNNING) {
        if (!command(c, 0, 0, TRB_TYPE(TRB_STOP_ENDPOINT) | target, done)
            && c->last_code != CC_CONTEXT_STATE) {
            return named(which, "'s Stop Endpoint");
        }

        state = ep_state(c, d, dci);
    }

    if (state == EP_STATE_HALTED) {
        if (!command(c, 0, 0, TRB_TYPE(TRB_RESET_ENDPOINT) | target, done)) {
            return named(which, "'s Reset Endpoint");
        }

        state = ep_state(c, d, dci);
    }

    if (!control_nodata(c, d, CLEAR_FEATURE, ENDPOINT_HALT, address)) {
        return named(which, "'s CLEAR_FEATURE");
    }

    if (state != EP_STATE_STOPPED && state != EP_STATE_ERROR) {
        return named(which, state == EP_STATE_RUNNING
                            ? ", still running after Stop Endpoint"
                            : state == EP_STATE_HALTED
                            ? ", still halted after Reset Endpoint"
                            : ", in a state Set TR Dequeue Pointer refuses");
    }

    if (!command(c, (uint32_t)next | (r->cycle == TRB_C ? 1u : 0u),
                 (uint32_t)(next >> 32), TRB_TYPE(TRB_SET_DEQUEUE) | target,
                 done)) {
        return named(which, "'s Set TR Dequeue Pointer");
    }

    return NULL;
}

/*
 * **Reset Recovery** (Bulk-Only 1.0 5.3.4): the class reset to the stick's
 * interface (3.1), then the halt cleared on bulk IN and then on bulk OUT - on
 * the controller as well as the stick, which is `reset_pipe`. What a host does
 * after a stall, a status that is not valid, or a phase error (6.4 to 6.6);
 * done here after a transfer that never answered as well, because the other
 * choice is a stick left in the middle of a command.
 */
static const char *recover(struct controller *c, struct device *d,
                           struct stick *s)
{
    const char *failed;

    if (!control_nodata(c, d, MASS_STORAGE_RESET, 0, s->interface)) {
        return "the Bulk-Only Mass Storage Reset";
    }

    failed = reset_pipe(c, d, &s->in, s->in_dci,
                        (uint8_t)(0x80u | (s->in_dci / 2u)), "bulk IN");

    if (failed == NULL) {
        failed = reset_pipe(c, d, &s->out, s->out_dci,
                            (uint8_t)(s->out_dci / 2u), "bulk OUT");
    }

    return failed;
}

/*
 * **A command, and a second chance.** `transact_once`; and when no status
 * came back that says what happened, the failure is said, the stick is put
 * through Reset Recovery, and the command is sent once more - so one stall is
 * a line in the log rather than a stick left unused. A second failure is the
 * caller's to say. A command the stick answered with a failed status is not
 * the transport's fault and is not sent again: `say_why_failed` asks why.
 */
static const char *transact(struct controller *c, struct device *d,
                            struct stick *s, const char *command,
                            const uint8_t *cdb, unsigned cdb_length,
                            bool out, uint8_t *into, unsigned length,
                            unsigned *got, enum bot_status *status,
                            struct say_line *line)
{
    const char *failed = transact_once(c, d, s, command, cdb, cdb_length,
                                       out, into, length, got, status);

    if (failed == NULL) {
        return NULL;
    }

    say_failure(c, d->port, failed, ", so the stick is reset", line);
    failed = recover(c, d, s);

    if (failed != NULL) {
        return failed;
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": the stick is reset, and the ");
    say_text(line, command);
    say_text(line, " sent again");
    say_send(console, line);

    return transact_once(c, d, s, command, cdb, cdb_length, out, into, length,
                         got, status);
}

/* An ASCII field of INQUIRY's data in quotes: its padding off the end, and
 * anything unprintable a '?'. */
static void say_field(struct say_line *line, const uint8_t *bytes, unsigned n)
{
    char text[INQUIRY_PRODUCT + 1u];
    unsigned i, end = 0;

    for (i = 0; i < n && i < INQUIRY_PRODUCT; i++) {
        text[i] = (bytes[i] >= 0x20u && bytes[i] < 0x7Fu) ? (char)bytes[i]
                                                          : '?';

        if (bytes[i] != ' ' && bytes[i] != 0) {
            end = i + 1u;
        }
    }

    text[end] = '\0';
    say_text(line, "\"");
    say_text(line, text);
    say_text(line, "\"");
}

/*
 * "UNIT ATTENTION (29h/00h)": a sense key's name, and the additional sense
 * code and its qualifier when the stick sent them.
 */
static void say_sense(struct say_line *line, const struct scsi_sense *sense)
{
    say_text(line, scsi_sense_key_name(sense->key));

    if (sense->coded) {
        say_text(line, " (");
        say_hex(line, sense->asc, 2u);
        say_text(line, "h/");
        say_hex(line, sense->ascq, 2u);
        say_text(line, "h)");
    }
}

/*
 * Why a command failed. REQUEST SENSE is what a failed command leaves its
 * reason in, so it is asked before any line about the failure is begun. True
 * when what came back is sense data, which is then in `sense`.
 */
static bool why_failed(struct controller *c, struct device *d, struct stick *s,
                       struct scsi_sense *sense, struct say_line *line)
{
    uint8_t cdb[16], data[SCSI_SENSE_LENGTH];
    enum bot_status status;
    unsigned got;

    memset(sense, 0, sizeof(*sense));

    return transact(c, d, s, "REQUEST SENSE", cdb,
                    scsi_request_sense(cdb, SCSI_SENSE_LENGTH), false,
                    data, SCSI_SENSE_LENGTH, &got, &status, line) == NULL
           && status == BOT_PASSED && scsi_sense(data, got, sense);
}

/*
 * The line for a command the stick failed: the command and, when `known`, the
 * stick's reason for it.
 */
static void say_failed(struct controller *c, struct device *d,
                       const char *command, bool known,
                       const struct scsi_sense *sense, struct say_line *line)
{
    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": the ");
    say_text(line, command);
    say_text(line, ", which the stick failed");

    if (known) {
        say_text(line, ": ");
        say_sense(line, sense);
    }

    say_send(console, line);
}

/* A command the stick failed, and why. */
static void say_why_failed(struct controller *c, struct device *d,
                           struct stick *s, const char *command,
                           struct say_line *line)
{
    struct scsi_sense sense;
    bool known = why_failed(c, d, s, &sense, line);

    say_failed(c, d, command, known, &sense, line);
}

/*
 * **Whether the stick is ready**, by TEST UNIT READY, asked up to READY_TRIES
 * times. A failure is asked why with REQUEST SENSE, which also clears what it
 * reports, and a stick that says NOT READY is given READY_WAIT_MS before the
 * next try. A unit attention is what a device says first after a reset, and
 * this one's port has just been reset, so a first try that fails is not a
 * surprise. `first` and `last` are what the stick said and `asked` whether it
 * said anything; `tries` is how many it took.
 */
static const char *ready(struct controller *c, struct device *d,
                         struct stick *s, struct scsi_sense *first,
                         struct scsi_sense *last, unsigned *tries,
                         bool *asked, struct say_line *line)
{
    uint8_t cdb[16], data[SCSI_SENSE_LENGTH];
    enum bot_status status;
    const char *failed;
    unsigned got;

    memset(first, 0, sizeof(*first));
    memset(last, 0, sizeof(*last));
    *asked = false;

    for (*tries = 1u; ; ++*tries) {
        failed = transact(c, d, s, "TEST UNIT READY", cdb,
                          scsi_test_unit_ready(cdb), false, NULL, 0u, &got,
                          &status, line);

        if (failed != NULL || status == BOT_PASSED) {
            return failed;
        }

        failed = transact(c, d, s, "REQUEST SENSE", cdb,
                          scsi_request_sense(cdb, SCSI_SENSE_LENGTH), false,
                          data, SCSI_SENSE_LENGTH, &got, &status, line);

        if (failed != NULL) {
            return failed;
        }

        if (status != BOT_PASSED || !scsi_sense(data, got, last)) {
            return named("REQUEST SENSE", ", which answered no sense data");
        }

        if (!*asked) {
            *first = *last;
            *asked = true;
        }

        if (*tries == READY_TRIES) {
            return named("TEST UNIT READY",
                         ", which the stick failed every time");
        }

        if (last->key == SCSI_KEY_NOT_READY) {
            (void)wait_serving(c, READY_WAIT_MS, NULL);
        }
    }
}

/* One block, read into `stick_block`. */
static uint8_t stick_block[BLOCK_MOST];

static const char *read_block(struct controller *c, struct device *d,
                              struct stick *s, uint32_t lba, unsigned size,
                              enum bot_status *status, struct say_line *line)
{
    uint8_t cdb[16];
    unsigned got;
    const char *failed = transact(c, d, s, "READ (10)", cdb,
                                  scsi_read_10(cdb, lba, 1u), false,
                                  stick_block, size, &got, status, line);

    if (failed == NULL && *status == BOT_PASSED && got != size) {
        return named("READ (10)", ", which sent less than a block");
    }

    return failed;
}

/* "16 MB", or "119 GB" from ten gigabytes up. */
static void say_size(struct say_line *line, uint64_t bytes)
{
    if (bytes < (10ull << 30)) {
        say_dec(line, (unsigned long)(bytes >> 20));
        say_text(line, " MB");
    } else {
        say_dec(line, (unsigned long)(bytes >> 30));
        say_text(line, " GB");
    }
}

/*
 * **USB step 5a: the stick's size, and its first blocks read.** Ready, by
 * `ready`; how many blocks and how big, by READ CAPACITY (10); then block 1
 * and the last block, by READ (10), asked whether they hold a GUID partition
 * table's header and its backup - which a stick made by `mkusb_image.py` does,
 * and which is the first thing a filesystem will be looked for by (`usb.md`
 * §7).
 *
 * READ CAPACITY (10) counts to 2^32 blocks, about 2 TB; a stick past that is
 * said, and READ CAPACITY (16) waits for one. A block larger than a page is
 * said and not read.
 */
static void first_blocks(struct controller *c, struct device *d,
                         struct stick *s, struct say_line *line)
{
    uint8_t cdb[16], data[SCSI_CAPACITY_10_LENGTH];
    struct scsi_sense first, last;
    struct scsi_capacity capacity;
    enum bot_status status;
    unsigned tries, got;
    bool asked, primary = false, backup = false;
    const char *failed;
    uint32_t end;

    failed = ready(c, d, s, &first, &last, &tries, &asked, line);

    if (asked) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);
        say_text(line, ": the stick said ");
        say_sense(line, &first);

        if (failed == NULL) {
            say_text(line, ", and was ready on try ");
            say_dec(line, tries);
        } else {
            say_text(line, ", and last ");
            say_sense(line, &last);
        }

        say_send(console, line);
    }

    if (failed != NULL) {
        say_failure(c, d->port, failed, "", line);
        return;
    }

    failed = transact(c, d, s, "READ CAPACITY (10)", cdb,
                      scsi_read_capacity_10(cdb), false, data,
                      SCSI_CAPACITY_10_LENGTH, &got, &status, line);

    if (failed == NULL && status != BOT_PASSED) {
        say_why_failed(c, d, s, "READ CAPACITY (10)", line);
        return;
    }

    if (failed == NULL && !scsi_capacity_10(data, got, &capacity)) {
        failed = named("READ CAPACITY (10)", ", which answered no capacity");
    }

    if (failed != NULL) {
        say_failure(c, d->port, failed, "", line);
        return;
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);

    if (capacity.too_many) {
        say_text(line, ": the stick has more blocks than READ CAPACITY (10) "
                       "counts, and READ CAPACITY (16) is not asked yet");
        say_send(console, line);
        return;
    }

    say_text(line, ": the stick holds ");
    say_dec(line, (unsigned long)capacity.blocks);
    say_text(line, " blocks of ");
    say_dec(line, capacity.block_size);
    say_text(line, " bytes, ");
    say_size(line, capacity.blocks * capacity.block_size);

    /*
     * A unit from here: its size is known, and a block fits one read. Its
     * number is the next never given out, kept until the stick leaves.
     */
    if (capacity.block_size <= BLOCK_TRANSFER_MOST) {
        s->blocks = capacity.blocks;
        s->block_size = capacity.block_size;

        if (!s->ready) {
            s->unit = units_named++;
            s->ready = true;
        }
    }

    if (capacity.block_size > BLOCK_MOST || capacity.blocks < 3u) {
        say_text(line, ", and its blocks are not read");
        say_send(console, line);
        return;
    }

    say_send(console, line);

    end = (uint32_t)(capacity.blocks - 1u);
    failed = read_block(c, d, s, 1u, capacity.block_size, &status, line);

    if (failed == NULL && status == BOT_PASSED) {
        primary = gpt_header_at(stick_block, capacity.block_size, 1u);
        failed = read_block(c, d, s, end, capacity.block_size, &status, line);
        backup = failed == NULL && status == BOT_PASSED
                 && gpt_header_at(stick_block, capacity.block_size, end);
    }

    if (failed == NULL && status != BOT_PASSED) {
        say_why_failed(c, d, s, "READ (10)", line);
        return;
    }

    if (failed != NULL) {
        say_failure(c, d->port, failed, "", line);
        return;
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, primary ? ": block 1 holds a" : ": block 1 holds no");
    say_text(line, " GUID partition table's header, and block ");
    say_dec(line, end);
    say_text(line, backup ? " its backup" : " no backup");
    say_send(console, line);
}

/*
 * **A stick, and USB step 4: bytes each way on its bulk endpoints.** Its two
 * endpoints given to the controller in one Configure Endpoint - bulk OUT at
 * context index twice its number, bulk IN at one more (4.5.1) - then
 * SET_CONFIGURATION, the order the mouse's comment gives; then INQUIRY, and
 * what the stick says it is; then step 5a's, in `first_blocks`.
 *
 * Its rings are the device's pages 4 and 5, which a mouse would have used, and
 * what it is sent and sends back goes in the device's buffer page, which
 * enumeration is finished with by now. LUN 0 only: Get Max LUN is left
 * unasked, because a stick with one unit may stall it (Bulk-Only 1.0 3.2),
 * and a stall on endpoint 0 is not recovered from here (`roadmap.md`).
 */
static void use_stick(struct controller *c, struct device *d,
                      const struct usb_config *found, struct say_line *line)
{
    struct stick *s = &c->stick[d->slot];
    uintptr_t page = PAGE_DEVICES + (d->slot - 1u) * PAGES_A_DEVICE;
    uint8_t cdb[16], data[INQUIRY_LENGTH];
    enum bot_status status;
    char fault[16];
    uint32_t done[4];
    uint32_t *icc, *slot, *ep;
    unsigned got = 0, last;
    const char *failed;

    memset(s, 0, sizeof(*s));
    s->dev = *d;
    d = &s->dev;                        /* the kept device, from here on */
    s->interface = found->storage_interface;
    s->out_dci = 2u * found->bulk_out;
    s->in_dci = 2u * found->bulk_in + 1u;
    last = s->in_dci > s->out_dci ? s->in_dci : s->out_dci;

    ring_start(&s->out,
               (uint32_t *)(c->mem + (page + DEVICE_PAGE_OUT) * PAGE),
               c->bus + (page + DEVICE_PAGE_OUT) * PAGE);
    ring_start(&s->in,
               (uint32_t *)(c->mem + (page + DEVICE_PAGE_IN) * PAGE),
               c->bus + (page + DEVICE_PAGE_IN) * PAGE);

    icc = context(c, d->input, 0);
    slot = context(c, d->input, 1);

    /* Both endpoints and the slot added, nothing dropped, endpoint 0 left
     * out; the slot's Context Entries raised to the higher of the two. */
    icc[0] = 0;
    icc[1] = 1u | (1u << s->out_dci) | (1u << s->in_dci);
    slot[0] = (slot[0] & ~(0x1Fu << 27)) | (last << 27);

    ep = context(c, d->input, s->out_dci + 1u);
    memset(ep, 0, c->context);
    ep[1] = ((uint32_t)found->bulk_out_packet << 16)
          | ((uint32_t)found->bulk_out_burst << 8)
          | (EP_TYPE_BULK_OUT << 3) | (3u << 1);
    ep[2] = (uint32_t)s->out.bus | 1u;
    ep[3] = (uint32_t)(s->out.bus >> 32);
    ep[4] = BULK_TRB_AVERAGE;

    ep = context(c, d->input, s->in_dci + 1u);
    memset(ep, 0, c->context);
    ep[1] = ((uint32_t)found->bulk_in_packet << 16)
          | ((uint32_t)found->bulk_in_burst << 8)
          | (EP_TYPE_BULK_IN << 3) | (3u << 1);
    ep[2] = (uint32_t)s->in.bus | 1u;
    ep[3] = (uint32_t)(s->in.bus >> 32);
    ep[4] = BULK_TRB_AVERAGE;

    if (!command(c, (uint32_t)d->input_bus, (uint32_t)(d->input_bus >> 32),
                 TRB_TYPE(TRB_CONFIGURE) | TRB_SLOT(d->slot), done)) {
        say_failure(c, d->port, "Configure Endpoint",
                    "; the stick is not spoken to", line);
        return;
    }

    if (!control_nodata(c, d, SET_CONFIGURATION, found->configuration, 0)) {
        say_failure(c, d->port, "SET_CONFIGURATION",
                    "; the stick is not spoken to", line);
        return;
    }

    if (!stick_buffer(c, s)) {
        c->last_code = CC_SUCCESS;
        say_failure(c, d->port, "a transfer buffer the controller can reach",
                    " could not be had; the stick is not spoken to", line);
        return;
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": a stick: SCSI over Bulk-Only, bulk IN endpoint ");
    say_dec(line, found->bulk_in);
    say_text(line, " and OUT endpoint ");
    say_dec(line, found->bulk_out);
    say_text(line, ", up to ");
    say_dec(line, found->bulk_in_packet);
    say_text(line, " bytes a packet");

    if (found->bulk_in_burst > 0) {
        say_text(line, " in bursts of ");
        say_dec(line, found->bulk_in_burst + 1u);
    }

    say_send(console, line);

    if (kosmos_boot_option("opt/kosmos/stickfault", fault, sizeof(fault)) == 9
        && memcmp(fault, "signature", 9u) == 0) {
        s->spoil = true;
        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);
        say_text(line, ": its first command goes out with a wrong signature, "
                       "as opt/kosmos/stickfault asks");
        say_send(console, line);
    }

    failed = transact(c, d, s, "INQUIRY", cdb,
                      scsi_inquiry(cdb, INQUIRY_LENGTH), false, data,
                      INQUIRY_LENGTH, &got, &status, line);

    if (failed == NULL && status != BOT_PASSED) {
        say_why_failed(c, d, s, "INQUIRY", line);
        return;
    }

    if (failed == NULL) {
        memcpy(s->vendor, data + INQUIRY_VENDOR, sizeof(s->vendor));
        memcpy(s->product, data + INQUIRY_PRODUCT, sizeof(s->product));
    }

    if (failed != NULL) {
        say_failure(c, d->port, failed, "", line);
        return;
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": the stick says it is ");
    say_field(line, data + INQUIRY_VENDOR, 8u);
    say_text(line, " ");
    say_field(line, data + INQUIRY_PRODUCT, 16u);
    say_text(line, ", revision ");
    say_field(line, data + INQUIRY_REVISION, 4u);
    say_text(line, ", device type ");
    say_dec(line, data[0] & 0x1Fu);

    if (got < INQUIRY_LENGTH) {
        say_text(line, ", in ");
        say_dec(line, got);
        say_text(line, " bytes of 36");
    }

    say_send(console, line);
    first_blocks(c, d, s, line);
}

/* ------------------------------------------------------- USB Ethernet */

/*
 * **The packet filter an adapter is asked for** (ECM 6.2.4, Table 8): frames
 * addressed to this machine, broadcast, and multicast - D2, D3 and D4.
 *
 * Not promiscuous, which is D0: a stack that is handed every frame on the
 * wire has to throw most of them away, and on a switch it would be handed
 * very few extra ones anyway. Multicast is in because IPv6's neighbour
 * discovery and mDNS are multicast and both are things this will want; ECM
 * 6.2.4 says a device with no multicast filter table takes D4 as "all of
 * them", which is the answer we would give it.
 */
#define ETH_FILTER_DIRECTED   0x04u
#define ETH_FILTER_BROADCAST  0x08u
#define ETH_FILTER_MULTICAST  0x10u
#define ETH_FILTER_WANTED     (ETH_FILTER_DIRECTED | ETH_FILTER_BROADCAST \
                               | ETH_FILTER_MULTICAST)

/*
 * **Where an adapter's frames live**: a run of its own the controller can
 * reach, made with the same three calls a stick's transfer buffer is
 * (`stick_buffer`), and refused for the same reason when the controller
 * addresses 32 bits and the run is above 4 GB.
 *
 * One page, which holds a frame each way with room to spare: an ECM adapter
 * says how large a frame it carries and the largest anything will say is
 * 1514, so 2048 each way is the round number above it.
 */
#define ETHER_PAGES         1u
#define ETHER_SLOT          2048u
#define ETHER_OUT_AT        0u
#define ETHER_IN_AT         ETHER_SLOT

static bool ether_buffer(const struct controller *c, struct ether *e)
{
    long region = kosmos_mem_create_flags(ETHER_PAGES, MEM_CONTIGUOUS);
    long mapped = region < 0 ? region : kosmos_mem_map(region);
    long bus = mapped < 0 ? mapped : kosmos_mem_phys(region);

    if (region < 0 || mapped < 0 || bus <= 0) {
        return false;
    }

    if (!c->ac64 && (uint64_t)bus + ETHER_PAGES * PAGE > 0x100000000ull) {
        return false;
    }

    e->frames_cap = region;
    e->frames = (uintptr_t)mapped;
    e->frames_bus = (uint64_t)bus;
    return true;
}

/*
 * One read out on the adapter's bulk IN, for the next frame that arrives.
 *
 * A frame is one transfer, ended by a packet shorter than the endpoint's -
 * which is what `TRB_ISP` is for, and why a short completion is success
 * here rather than a fault (ECM 1.2 3.3.1).
 */
static void ask_for_frame(struct controller *c, unsigned slot)
{
    struct ether *e = &c->ether[slot];

    if (e->frames == 0) {
        return;
    }

    e->receiving = true;
    (void)ring_push(&e->in, (uint32_t)(e->frames_bus + ETHER_IN_AT),
                    (uint32_t)((e->frames_bus + ETHER_IN_AT) >> 32),
                    ETHER_SLOT, TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    mmio_write32(c->doorbells + 4u * slot, e->in_dci);
}

/*
 * **A frame out**, and the zero-length packet that has to follow one whose
 * length is an exact multiple of the endpoint's packet (ECM 1.2 3.3.1).
 *
 * Without it the adapter is still waiting for the rest of a frame that has
 * already ended, and the next frame sent joins the end of this one. It costs
 * a second transfer on exactly the lengths that need it - 64, 128, 512,
 * 1024 - and nothing on any other.
 *
 * Synchronous, because there is one frame in the buffer: the caller has the
 * frame in hand and a bulk OUT that is refused is a frame that was not sent,
 * which is something the caller has to know. `wait_serving` goes on reading
 * every other device on the machine while this waits.
 */
static bool ether_send(struct controller *c, unsigned slot,
                       const uint8_t *frame, unsigned length,
                       unsigned packet)
{
    struct ether *e = &c->ether[slot];
    unsigned moved = 0;

    if (e->frames == 0 || length == 0 || length > ETHER_SLOT) {
        return false;
    }

    memcpy((uint8_t *)(e->frames + ETHER_OUT_AT), frame, length);

    if (!bulk(c, slot, e->out_dci, &e->out, e->frames_bus + ETHER_OUT_AT,
              length, &moved) || moved != length) {
        return false;
    }

    if (packet != 0 && length % packet == 0) {
        if (!bulk(c, slot, e->out_dci, &e->out, e->frames_bus + ETHER_OUT_AT,
                  0, &moved)) {
            return false;
        }
    }

    e->sent++;
    return true;
}

/*
 * **A frame in.**
 *
 * Today it is written down and dropped: the first four with what they are,
 * and the rest counted. Step 7d hands them to `net.c` through a ring, and
 * this is where that will happen - the point of doing it in two steps is
 * that a frame that never arrives is a fault in the adapter, the endpoint or
 * the setting, and a frame that arrives and is not understood is a fault
 * somewhere else entirely.
 */
static void take_frame(struct controller *c, const uint32_t *event)
{
    unsigned slot = TRB_SLOT_OF(event[3]);
    struct say_line line;
    struct ether *e;
    const uint8_t *frame;
    uint32_t code, left;
    unsigned got, type, i;

    if (TRB_TYPE_OF(event[3]) != TRB_TRANSFER || slot == 0
        || slot > DEVICES_MAX) {
        return;
    }

    e = &c->ether[slot];

    if (!e->receiving || TRB_ENDPOINT_OF(event[3]) != e->in_dci) {
        return;
    }

    code = TRB_CODE_OF(event[2]);
    left = TRB_LEFT_OF(event[2]);

    if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
        e->receiving = false;

        about(&line, c);
        say_text(&line, " port ");
        say_dec(&line, e->port);
        say_text(&line, ": a frame failed to arrive: ");
        say_text(&line, completion_name(code));
        say_text(&line, " (");
        say_dec(&line, code);
        say_text(&line, "); nothing is read from it until it is plugged in "
                        "again");
        say_send(console, &line);
        return;
    }

    got = left < ETHER_SLOT ? ETHER_SLOT - left : 0u;
    frame = (const uint8_t *)(e->frames + ETHER_IN_AT);

    /* A zero-length packet is the end of the one before it, not a frame. */
    if (got >= 14u) {
        e->received++;

        /*
         * **To the stack, if it is holding this adapter** - into its ring
         * and a wake, which is all the driver does with a frame from here
         * on (`usb.md` 7d). A ring with no room is a frame dropped, which is
         * what a full receive queue means on any card: the stack is behind,
         * and the far end will send it again.
         */
        if (frames_ring != NULL && frames_on == c && frames_slot == slot
            && got <= ETH_RING_SLOT) {
            uint32_t write = frames_ring->in_write;
            uint32_t read = eth_ring_acquire(&frames_ring->in_read);

            if (eth_ring_space(write, read) > 0) {
                memcpy(eth_ring_in(frames_ring, write), frame, got);
                frames_ring->in_length[write % ETH_RING_SLOTS] = got;
                eth_ring_publish(&frames_ring->in_write, write + 1u);

                /*
                 * And the stack, if it is asleep. It is often not - a reply
                 * to something it just sent lands while it is still busy,
                 * which is what `drain` before the receive is for - so this
                 * is the other case: a frame nobody here asked for.
                 */

                /*
                 * And the stack, if it is asleep. It often is not - a reply
                 * to something it just sent lands while it is still busy,
                 * which is what draining before the receive is for - so this
                 * is the other case: a frame nobody here asked for.
                 */
                (void)kosmos_net_wake();
            } else {
                e->dropped++;
            }
        }

        /*
         * **The first four, and only while nobody is holding the adapter.**
         * Before the stack attaches these are the whole of what a person can
         * see - `opt/kosmos/ethprobe` is read by them - and after it they are
         * a driver narrating traffic that belongs to somebody else.
         */
        if (e->said_frames < 4u && frames_ring == NULL) {
            e->said_frames++;
            type = ((unsigned)frame[12] << 8) | frame[13];

            about(&line, c);
            say_text(&line, " port ");
            say_dec(&line, e->port);
            say_text(&line, ": a frame of ");
            say_dec(&line, got);
            say_text(&line, " bytes from ");

            for (i = 0; i < 6u; i++) {
                if (i != 0) {
                    say_text(&line, ":");
                }

                say_hex(&line, frame[6u + i], 2);
            }

            say_text(&line, ", type ");
            say_hex(&line, type, 4);

            if (type == 0x0806u) {
                say_text(&line, " (ARP)");
            } else if (type == 0x0800u) {
                say_text(&line, " (IPv4)");
            } else if (type == 0x86DDu) {
                say_text(&line, " (IPv6)");
            }

            say_send(console, &line);
        }
    }

    ask_for_frame(c, slot);
}

/*
 * **Two ARP requests, when `opt/kosmos/ethprobe` asks for them.**
 *
 * `opt/kosmos/ethprobe=10.0.2.15,10.0.2.2`: this machine's address, then the
 * one to ask about. A frame goes out and something on the other end answers
 * without being asked twice, which is the smallest useful traffic there is
 * and is what step 7c is proved with (RFC 826).
 *
 * **Two, and the second one is the zero-length packet's test.** The first is
 * 60 bytes, Ethernet's shortest frame, which no endpoint's packet size
 * divides. The second is padded to a multiple of the bulk OUT endpoint's
 * packet - 64 bytes on QEMU's adapter, 512 on a high-speed one - which is
 * exactly the length that needs a zero-length packet after it (ECM 1.2
 * 3.3.1), because otherwise the adapter is still waiting for the rest of a
 * frame that has already ended. A driver without that rule gets one answer
 * and not two, which is a test rather than a specification quoted in a
 * comment. Padding an ARP request is legal and every receiver ignores it.
 *
 * **It is a diagnostic and it stays one.** The stack sends the real ARP from
 * 7d on; what this is for afterwards is the question a person standing in
 * front of the ThinkPad wants answered - is the *adapter* moving frames -
 * separately from whether the stack above it is. It is off unless the option
 * is there, so nothing this machine has not been told to send goes out.
 */
static bool dotted_quad(const char **at, const char *end, uint8_t out[4])
{
    unsigned part;

    for (part = 0; part < 4u; part++) {
        unsigned value = 0, digits = 0;

        while (*at < end && **at >= '0' && **at <= '9' && digits < 3u) {
            value = value * 10u + (unsigned)(*(*at)++ - '0');
            digits++;
        }

        if (digits == 0 || value > 255u) {
            return false;
        }

        out[part] = (uint8_t)value;

        if (part < 3u) {
            if (*at >= end || **at != '.') {
                return false;
            }

            (*at)++;
        }
    }

    return true;
}

#define ETHER_LEAST         60u         /* Ethernet's shortest frame, less FCS */
#define ARP_REQUEST_BYTES   42u

static void ether_probe(struct controller *c, unsigned slot, unsigned packet,
                        struct say_line *line)
{
    struct ether *e = &c->ether[slot];
    char option[40];
    uint8_t frame[ETHER_SLOT], mine[4], theirs[4];
    const char *at, *end;
    unsigned padded, i;
    long length = kosmos_boot_option("opt/kosmos/ethprobe", option,
                                     sizeof(option));

    if (length <= 0 || (unsigned long)length >= sizeof(option)
        || !e->have_mac) {
        return;
    }

    at = option;
    end = option + length;

    if (!dotted_quad(&at, end, mine) || at >= end || *at != ','
        || (at++, !dotted_quad(&at, end, theirs))) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, e->port);
        say_text(line, ": opt/kosmos/ethprobe wants two addresses, "
                       "<mine>,<theirs>, and no frame is sent");
        say_send(console, line);
        return;
    }

    memset(frame, 0, ETHER_SLOT);
    memset(frame, 0xFF, 6u);                    /* to everyone */
    memcpy(frame + 6u, e->mac, 6u);
    frame[12] = 0x08;
    frame[13] = 0x06;                           /* ARP */
    frame[15] = 1u;                             /* Ethernet */
    frame[16] = 0x08;                           /* IPv4 */
    frame[18] = 6u;                             /* a hardware address */
    frame[19] = 4u;                             /* a protocol address */
    frame[21] = 1u;                             /* a request */
    memcpy(frame + 22u, e->mac, 6u);
    memcpy(frame + 28u, mine, 4u);
    memcpy(frame + 38u, theirs, 4u);

    about(line, c);
    say_text(line, " port ");
    say_dec(line, e->port);
    /*
     * The second one's length: the smallest multiple of the endpoint's
     * packet that holds a request, so it is the case ECM 3.3.1 is about.
     * An endpoint whose packet is larger than a frame gets one frame only.
     */
    padded = packet != 0 ? ((ETHER_LEAST + packet - 1u) / packet) * packet : 0u;

    if (padded > ETHER_SLOT) {
        padded = 0;
    }

    say_text(line, ": asking who has ");

    for (i = 0; i < 4u; i++) {
        if (i != 0) {
            say_text(line, ".");
        }

        say_dec(line, theirs[i]);
    }

    say_text(line, ", twice - ");
    say_dec(line, ETHER_LEAST);
    say_text(line, " bytes and ");
    say_dec(line, padded);
    say_text(line, " - as opt/kosmos/ethprobe asks");

    /*
     * Said before they are sent, not after. `ether_send` waits for the
     * transfer, and a wait goes on serving every device on the machine - so
     * the answer to the first request is read, and written down, while this
     * line is still being composed. The log showed the reply above the
     * question.
     */
    say_send(console, line);

    for (i = 0; i < 2u; i++) {
        unsigned bytes = i == 0 ? ETHER_LEAST : padded;

        if (bytes == 0) {
            continue;
        }

        if (!ether_send(c, slot, frame, bytes, packet)) {
            about(line, c);
            say_text(line, " port ");
            say_dec(line, e->port);
            say_text(line, ": the ");
            say_dec(line, bytes);
            say_text(line, "-byte frame would not go out: ");
            say_text(line, completion_name(c->last_code));
            say_send(console, line);
        }
    }
}

/*
 * One request out on the adapter's interrupt endpoint, for the next thing it
 * has to say about its link. A mouse's reports work the same way and for the
 * same reason (`ask_for_report`): one request is outstanding, and the next
 * goes out when that one comes back, so nothing polls and nothing waits.
 */
static void ask_for_note(struct controller *c, unsigned slot)
{
    struct ether *e = &c->ether[slot];

    e->reading = true;
    (void)ring_push(&e->notify, (uint32_t)e->note_bus,
                    (uint32_t)(e->note_bus >> 32), e->notify_length,
                    TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    mmio_write32(c->doorbells + 4u * slot, e->notify_dci);
}

/* A rate in bits a second, as the unit a person reads it in. */
static void say_rate(struct say_line *line, uint32_t bits)
{
    if (bits >= 1000000000u) {
        say_dec(line, bits / 1000000000u);
        say_text(line, " Gb/s");
    } else if (bits >= 1000000u) {
        say_dec(line, bits / 1000000u);
        say_text(line, " Mb/s");
    } else if (bits >= 1000u) {
        say_dec(line, bits / 1000u);
        say_text(line, " kb/s");
    } else {
        say_dec(line, bits);
        say_text(line, " b/s");
    }
}

/*
 * **What the adapter said about its link.**
 *
 * A transfer may carry more than one notification, so this walks what
 * arrived rather than reading the first and stopping: QEMU's adapter sends
 * NETWORK_CONNECTION and CONNECTION_SPEED_CHANGE, and a device is free to
 * put both in one transfer. `usb_decode_notify` says how long each is.
 *
 * **Only a change is said.** The link is reported when it is first heard and
 * whenever it turns over, not on every notification: an adapter that repeats
 * itself would otherwise fill the log.
 */
static void take_note(struct controller *c, const uint32_t *event)
{
    unsigned slot = TRB_SLOT_OF(event[3]);
    struct say_line line;
    struct ether *e;
    struct usb_notify note;
    uint32_t code, left;
    unsigned got, at;

    if (TRB_TYPE_OF(event[3]) != TRB_TRANSFER || slot == 0
        || slot > DEVICES_MAX) {
        return;
    }

    e = &c->ether[slot];

    if (!e->reading || TRB_ENDPOINT_OF(event[3]) != e->notify_dci) {
        return;
    }

    code = TRB_CODE_OF(event[2]);
    left = TRB_LEFT_OF(event[2]);

    if (code != CC_SUCCESS && code != CC_SHORT_PACKET) {
        e->reading = false;

        about(&line, c);
        say_text(&line, " port ");
        say_dec(&line, e->port);
        say_text(&line, ": the adapter's link notification failed: ");
        say_text(&line, completion_name(code));
        say_text(&line, " (");
        say_dec(&line, code);
        say_text(&line, "); its link is not watched until it is plugged in "
                        "again");
        say_send(console, &line);
        return;
    }

    got = left < e->notify_length ? e->notify_length - left : 0u;

    for (at = 0; at < got; at += note.length) {
        usb_decode_notify(e->note + at, got - at, &note);

        if (note.kind == USB_NOTIFY_MALFORMED || note.length == 0) {
            break;
        }

        if (note.kind == USB_NOTIFY_CONNECTION
            && (!e->link_said || e->link != note.up)) {
            e->link = note.up;
            e->link_said = true;

            about(&line, c);
            say_text(&line, " port ");
            say_dec(&line, e->port);
            say_text(&line, note.up ? ": its link is up" : ": its link is down");
            say_send(console, &line);
        } else if (note.kind == USB_NOTIFY_SPEED
                   && (!e->speed_said || e->upstream != note.upstream
                       || e->downstream != note.downstream)) {
            e->upstream = note.upstream;
            e->downstream = note.downstream;
            e->speed_said = true;

            about(&line, c);
            say_text(&line, " port ");
            say_dec(&line, e->port);
            say_text(&line, ": its link runs at ");
            say_rate(&line, note.downstream);
            say_text(&line, " in and ");
            say_rate(&line, note.upstream);
            say_text(&line, " out");
            say_send(console, &line);
        }
    }

    ask_for_note(c, slot);
}

/*
 * **A USB Ethernet adapter, configured** - `usb.md` steps 7a and 7b.
 *
 * `usb_decode_ecm` found a CDC-ECM function in one of its configurations.
 * This reads the MAC address out of the string that function names, puts the
 * adapter's three endpoints in the controller's hands, chooses the
 * configuration, takes the Data interface off its empty setting, asks for the
 * frames this machine wants, and starts listening for what it says about its
 * link.
 *
 * **The order matters and is the specification's.** Configure Endpoint gives
 * the *controller* the three endpoints - which is xHCI's business and has
 * nothing to do with what the device thinks - and SET_CONFIGURATION then puts
 * the device in that configuration, with every interface at setting 0.
 * ECM's Data interface at setting 0 has no endpoints at all (ECM 3.3), which
 * is the trap this specification is known for: a driver that stops here has
 * a device that answers every request and never delivers a frame. SET_INTERFACE
 * is what turns the endpoints on at the device's end.
 *
 * **The filter is asked for and not insisted on.** ECM 6.2.4 makes
 * SET_ETHERNET_PACKET_FILTER mandatory, and a device that refuses it is
 * likely to be passing everything anyway; a refusal is said and the adapter
 * is used. The link, on the other hand, is only ever heard about - nothing
 * is asked of it.
 *
 * The buffer is cleared before the MAC string is asked for, so a device that
 * answers with fewer bytes than its string's length claims is read as a short
 * string rather than as whatever the last request left behind.
 */
static void use_ethernet(struct controller *c, struct device *d,
                         const struct usb_ecm *ecm, struct say_line *line)
{
    struct ether *e = &c->ether[d->slot];
    uintptr_t page = PAGE_DEVICES + (d->slot - 1u) * PAGES_A_DEVICE;
    uint32_t done[4];
    uint32_t *icc, *slot, *ep;
    unsigned last, i, on;
    bool filtered, confirmed;

    memset(e, 0, sizeof(*e));
    e->dev = *d;
    d = &e->dev;                        /* the kept device, from here on */
    e->port = d->port;
    e->max_segment = ecm->max_segment;

    if (d->language == 0
        && control_in(c, d, GET_DESCRIPTOR, DESC_STRING, 0, 255u)
        && d->buffer[0] >= 4u && d->buffer[1] == 3u) {
        d->language = (uint16_t)(d->buffer[2] | (unsigned)d->buffer[3] << 8);
    }

    if (d->language != 0) {
        memset(d->buffer, 0, 256u);
        e->have_mac = control_in(c, d, GET_DESCRIPTOR,
                                 (uint16_t)(DESC_STRING | ecm->mac_string),
                                 d->language, 255u)
                   && usb_decode_mac(d->buffer, 256u, e->mac);
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": USB Ethernet, CDC-ECM, in configuration ");
    say_dec(line, ecm->configuration);

    if (e->have_mac) {
        say_text(line, ": MAC ");

        for (i = 0; i < 6u; i++) {
            if (i != 0) {
                say_text(line, ":");
            }

            say_hex(line, e->mac[i], 2);
        }
    } else {
        say_text(line, ": its MAC address string would not read");
    }

    say_text(line, ", frames up to ");
    say_dec(line, ecm->max_segment);
    say_text(line, " bytes");
    say_send(console, line);

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": its frames on interface ");
    say_dec(line, ecm->data);
    say_text(line, " setting ");
    say_dec(line, ecm->data_alternate);
    say_text(line, ", bulk IN ");
    say_dec(line, ecm->bulk_in);
    say_text(line, " and OUT ");
    say_dec(line, ecm->bulk_out);
    say_text(line, " of ");
    say_dec(line, ecm->bulk_in_packet);
    say_text(line, " bytes");

    if (ecm->notify != 0) {
        say_text(line, "; its link on interrupt IN ");
        say_dec(line, ecm->notify);
    }

    say_send(console, line);

    /*
     * The three endpoints, by their context indexes (4.5.1): twice the
     * number for an OUT, and one more for an IN.
     */
    e->out_dci = 2u * ecm->bulk_out;
    e->out_packet = ecm->bulk_out_packet;
    e->in_dci = 2u * ecm->bulk_in + 1u;
    e->notify_dci = ecm->notify != 0 ? 2u * ecm->notify + 1u : 0u;
    last = e->in_dci > e->out_dci ? e->in_dci : e->out_dci;

    if (e->notify_dci > last) {
        last = e->notify_dci;
    }

    ring_start(&e->out,
               (uint32_t *)(c->mem + (page + DEVICE_PAGE_OUT) * PAGE),
               c->bus + (page + DEVICE_PAGE_OUT) * PAGE);
    ring_start(&e->in,
               (uint32_t *)(c->mem + (page + DEVICE_PAGE_IN) * PAGE),
               c->bus + (page + DEVICE_PAGE_IN) * PAGE);

    icc = context(c, d->input, 0);
    slot = context(c, d->input, 1);

    icc[0] = 0;
    icc[1] = 1u | (1u << e->out_dci) | (1u << e->in_dci);
    slot[0] = (slot[0] & ~(0x1Fu << 27)) | (last << 27);

    ep = context(c, d->input, e->out_dci + 1u);
    memset(ep, 0, c->context);
    ep[1] = ((uint32_t)ecm->bulk_out_packet << 16)
          | ((uint32_t)ecm->bulk_out_burst << 8)
          | (EP_TYPE_BULK_OUT << 3) | (3u << 1);
    ep[2] = (uint32_t)e->out.bus | 1u;
    ep[3] = (uint32_t)(e->out.bus >> 32);
    ep[4] = ecm->max_segment;

    ep = context(c, d->input, e->in_dci + 1u);
    memset(ep, 0, c->context);
    ep[1] = ((uint32_t)ecm->bulk_in_packet << 16)
          | ((uint32_t)ecm->bulk_in_burst << 8)
          | (EP_TYPE_BULK_IN << 3) | (3u << 1);
    ep[2] = (uint32_t)e->in.bus | 1u;
    ep[3] = (uint32_t)(e->in.bus >> 32);
    ep[4] = ecm->max_segment;

    /*
     * And its interrupt IN, the way a mouse's is (4.8.2.4): three errors
     * allowed, its packet, its interval, and its largest payload an interval.
     */
    if (e->notify_dci != 0) {
        e->notify_length = ecm->notify_packet;
        e->note = (uint8_t *)(c->mem + (page + DEVICE_PAGE_NOTE) * PAGE);
        e->note_bus = c->bus + (page + DEVICE_PAGE_NOTE) * PAGE;
        ring_start(&e->notify,
                   (uint32_t *)(c->mem + (page + DEVICE_PAGE_NOTIFY) * PAGE),
                   c->bus + (page + DEVICE_PAGE_NOTIFY) * PAGE);

        icc[1] |= 1u << e->notify_dci;

        ep = context(c, d->input, e->notify_dci + 1u);
        memset(ep, 0, c->context);
        ep[0] = interval_for(d->speed, ecm->notify_interval) << 16;
        ep[1] = ((uint32_t)ecm->notify_packet << 16) | (7u << 3) | (3u << 1);
        ep[2] = (uint32_t)e->notify.bus | 1u;
        ep[3] = (uint32_t)(e->notify.bus >> 32);
        ep[4] = ((uint32_t)ecm->notify_packet << 16) | ecm->notify_packet;
    }

    if (!command(c, (uint32_t)d->input_bus, (uint32_t)(d->input_bus >> 32),
                 TRB_TYPE(TRB_CONFIGURE) | TRB_SLOT(d->slot), done)) {
        say_failure(c, d->port, "Configure Endpoint",
                    "; the adapter is not driven", line);
        return;
    }

    if (!control_nodata(c, d, SET_CONFIGURATION, ecm->configuration, 0)) {
        say_failure(c, d->port, "SET_CONFIGURATION",
                    "; the adapter is not driven", line);
        return;
    }

    if (!control_nodata(c, d, SET_INTERFACE, ecm->data_alternate, ecm->data)) {
        say_failure(c, d->port, "SET_INTERFACE",
                    "; its Data interface is on setting 0, which has no "
                    "endpoints, so no frame would ever arrive", line);
        return;
    }

    /*
     * **And the device is asked which setting it is on** (GET_INTERFACE,
     * 9.4.4), rather than the line printing what it was told to choose.
     *
     * This is the one request in the sequence whose effect is invisible: a
     * configuration that failed stops everything after it, and a filter that
     * was refused says so, but an interface left on setting 0 behaves
     * exactly like one that was set - until a frame is expected, which is
     * step 7c. So the number in the line is the device's answer, and a
     * driver that never sent SET_INTERFACE at all prints a 0.
     */
    on = ecm->data_alternate;
    confirmed = control_in(c, d, GET_INTERFACE, 0, ecm->data, 1u);

    if (confirmed) {
        on = d->buffer[0];
    }

    filtered = control_nodata(c, d, SET_ETH_FILTER, ETH_FILTER_WANTED,
                              ecm->control);

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": configured, ");
    say_text(line, confirmed ? "on setting " : "setting ");
    say_dec(line, on);
    say_text(line, confirmed ? " as it says itself, " : " chosen, ");

    if (filtered) {
        say_text(line, "taking frames addressed to it, broadcast and "
                       "multicast");
    } else {
        say_text(line, "and it refused a packet filter, so it decides what "
                       "reaches this machine");
    }

    if (e->notify_dci != 0) {
        say_text(line, "; listening for its link");
    } else {
        say_text(line, "; it has no interrupt endpoint, so nothing will say "
                       "whether its link is up");
    }

    say_send(console, line);

    if (e->notify_dci != 0) {
        ask_for_note(c, d->slot);
    }

    /*
     * And the frames: somewhere to put them, a read outstanding for the next
     * one to arrive, and - only if `opt/kosmos/ethprobe` asks - one ARP
     * request, which is the smallest traffic that makes something answer.
     */
    if (!ether_buffer(c, e)) {
        c->last_code = CC_SUCCESS;
        say_failure(c, d->port, "a frame buffer the controller can reach",
                    " could not be had; no frame moves", line);
        return;
    }

    ask_for_frame(c, d->slot);
    ether_probe(c, d->slot, ecm->bulk_out_packet, line);
}

/*
 * One configuration descriptor, whole, into the device's buffer: nine bytes
 * for its total length (9.4.3), then all of it, up to a page. `index` is the
 * descriptor's index, 0 to bNumConfigurations - 1, which is not its
 * bConfigurationValue.
 */
static bool read_configuration(struct controller *c, struct device *d,
                               unsigned index, struct say_line *line,
                               unsigned *total)
{
    uint16_t which = (uint16_t)(DESC_CONFIGURATION | (index & 0xFFu));

    if (!control_in(c, d, GET_DESCRIPTOR, which, 0, 9u)) {
        say_failure(c, d->port, "GET_DESCRIPTOR for its configuration", "",
                    line);
        return false;
    }

    *total = d->buffer[2] | (unsigned)d->buffer[3] << 8;
    *total = *total > PAGE ? PAGE : *total;

    if (*total > 9u && !control_in(c, d, GET_DESCRIPTOR, which, 0,
                                   (uint16_t)*total)) {
        say_failure(c, d->port, "GET_DESCRIPTOR for all its configuration",
                    "", line);
        return false;
    }

    return true;
}

/* Whether a configuration holds something `use_device` goes on to use. */
static bool usable(enum usb_config_kind kind)
{
    return kind == USB_CONFIG_BOOT_MOUSE || kind == USB_CONFIG_BULK_ONLY
        || kind == USB_CONFIG_XBOX360 || kind == USB_CONFIG_XBOXONE
        || kind == USB_CONFIG_BOOT_KEYBOARD;
}

/* How many of a device's configurations are asked for before it is said to
 * be none of these. Every device found so far has one or two. */
#define CONFIGS_TRIED       8u

/*
 * A device that has just said what it is, asked whether it is a mouse or a
 * stick this can use - and if it is, made ready to be used.
 *
 * **Every configuration it has, until one is of use.** The first is the one
 * nearly every device is used in, and the only one a mouse, a stick or a pad
 * has ever offered; so the others are asked for only when it is none of
 * those. The RTL8153 Ethernet adapter is why they are asked for at all: its
 * first configuration is Realtek's own interface, and the standard one,
 * CDC-ECM, is its second (`roadmap.md` 5m). A device that is nothing here in
 * any of them is said by its first interface's class, where it used to be
 * passed over in silence.
 *
 * **Its configuration**, asked for twice: nine bytes for the total length,
 * then all of it, walked by `usb_decode.c`. A HID boot mouse - subclass 1,
 * protocol 2 - with an interrupt IN endpoint is the one kind taken. Any other
 * HID device is said and left alone.
 *
 * **Then the controller before the device**, the order 4.3.5 gives: Configure
 * Endpoint with the endpoint's context first, because a SET_CONFIGURATION
 * after one that failed is undefined behaviour; then SET_CONFIGURATION.
 *
 * **Then its Report descriptor, and the protocol that follows from it.** A
 * boot mouse's boot report is fixed (HID 1.11 B.2), and this used to ask for
 * the boot protocol and read bytes 0, 1 and 2. QEMU's mouse obeys. The
 * ThinkPad's 04d9:fc38 took the request without an error and went on sending
 * its own reports, and the arrow went down when the mouse moved right. So the
 * descriptor is asked of the interface (7.1.1), and when it lays out relative
 * X and Y in a report that fits a packet the mouse is put in the report
 * protocol and read by what it said; when it does not, the boot protocol and
 * the boot report, as before. The host sets the protocol either way rather
 * than assume it (7.2.6). SET_IDLE is not sent: a boot mouse need not support
 * it (Appendix G), and a mouse's idle rate starts at infinity - a report only
 * when something changes - which is what is wanted (7.2.4).
 *
 * **A SuperSpeed mouse is said and not read**: its endpoint's largest payload
 * an interval comes from a companion descriptor this does not walk (4.14.2).
 *
 * **A stick** - mass storage, SCSI over Bulk-Only - goes to `use_stick`, and
 * mass storage it cannot speak to is said.
 */
static void use_device(struct controller *c, struct device *d,
                       struct say_line *line)
{
    struct mouse *m = &c->mouse[d->slot];
    uintptr_t page = PAGE_DEVICES + (d->slot - 1u) * PAGES_A_DEVICE;
    struct usb_config found, first;
    struct usb_ecm ecm;
    uint32_t done[4];
    uint32_t *icc, *slot, *ep;
    unsigned total, dci, interval, payload, index, count;
    const char *why = "it gave no Report descriptor";

    memset(m, 0, sizeof(*m));
    memset(&first, 0, sizeof(first));

    count = d->configurations == 0u ? 1u : d->configurations;
    count = count > CONFIGS_TRIED ? CONFIGS_TRIED : count;

    for (index = 0; index < count; index++) {
        if (!read_configuration(c, d, index, line, &total)) {
            return;
        }

        usb_decode_config(d->buffer, total, &found);

        if (index == 0) {
            first = found;
        }

        if (usable(found.kind)) {
            break;
        }

        usb_decode_ecm(d->buffer, total, &ecm);

        if (ecm.ok) {
            use_ethernet(c, d, &ecm, line);
            return;
        }
    }

    /* None of them: what is said is about the first, as it always was. */
    if (index == count) {
        found = first;
    }

    if (found.kind == USB_CONFIG_NEITHER) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);
        say_text(line, ": class ");
        say_hex(line, found.first_class, 2);
        say_text(line, "/");
        say_hex(line, found.first_subclass, 2);
        say_text(line, "/");
        say_hex(line, found.first_protocol, 2);

        if (count > 1u) {
            say_text(line, " first, of ");
            say_dec(line, count);
            say_text(line, " configurations");
        }

        say_text(line, " - nothing here reads it");
        say_send(console, line);
        return;
    }

    if (found.kind == USB_CONFIG_BULK_ONLY) {
        use_stick(c, d, &found, line);
        return;
    }

    if ((found.kind != USB_CONFIG_BOOT_MOUSE
         && found.kind != USB_CONFIG_BOOT_KEYBOARD
         && found.kind != USB_CONFIG_XBOX360
         && found.kind != USB_CONFIG_XBOXONE) || d->speed >= 4u) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);

        if (found.kind == USB_CONFIG_MALFORMED) {
            say_text(line, ": a configuration descriptor that does not add "
                           "up; not read");
        } else if (found.kind == USB_CONFIG_STORAGE_OTHER) {
            say_text(line, ": mass storage this does not speak to (subclass ");
            say_dec(line, found.storage_subclass);
            say_text(line, ", protocol ");
            say_dec(line, found.storage_protocol);
            say_text(line, "), or no bulk IN and OUT in it; not read");
        } else if (found.kind == USB_CONFIG_HID_OTHER) {
            say_text(line, ": a HID device, not a boot mouse (subclass ");
            say_dec(line, found.hid_subclass);
            say_text(line, ", protocol ");
            say_dec(line, found.hid_protocol);
            say_text(line, "); not read");
        } else if (found.kind == USB_CONFIG_XBOX360
                   || found.kind == USB_CONFIG_XBOXONE) {
            say_text(line, ": a SuperSpeed Xbox controller, which this "
                           "does not read yet");
        } else {
            say_text(line, ": a SuperSpeed boot mouse, which this does not "
                           "read yet");
        }

        say_send(console, line);
        return;
    }

    dci = 2u * found.endpoint + 1u;                 /* 4.5.1: an IN endpoint */
    interval = interval_for(d->speed, found.interval);
    payload = found.packet * (found.extra + 1u);    /* 4.14.2, for USB 2 */

    m->port = d->port;
    m->dci = dci;
    m->length = found.packet;
    m->report = (uint8_t *)(c->mem + (page + DEVICE_PAGE_REPORT) * PAGE);
    m->report_bus = c->bus + (page + DEVICE_PAGE_REPORT) * PAGE;
    ring_start(&m->ring,
               (uint32_t *)(c->mem + (page + DEVICE_PAGE_RING) * PAGE),
               c->bus + (page + DEVICE_PAGE_RING) * PAGE);

    icc = context(c, d->input, 0);
    slot = context(c, d->input, 1);
    ep = context(c, d->input, dci + 1u);

    /*
     * The slot and this endpoint added, nothing dropped, and endpoint 0 left
     * out, as Configure Endpoint wants (4.6.6, 6.2.5.1). The slot's Context
     * Entries raised to this endpoint, now the last one (6.2.2.2). And the
     * endpoint the way 4.8.2.4 describes an interrupt one: three errors
     * allowed, IN, its packet and its extra transactions, its interval, its
     * ring with the cycle bit at 1, and its largest payload an interval
     * (Tables 6-8 and 6-9). Its average TRB length is its one request's,
     * since every request it will have is that one (4.14.1.1).
     */
    icc[0] = 0;
    icc[1] = 1u | (1u << dci);
    slot[0] = (slot[0] & ~(0x1Fu << 27)) | (dci << 27);

    memset(ep, 0, c->context);
    ep[0] = interval << 16;
    ep[1] = ((uint32_t)found.packet << 16) | ((uint32_t)found.extra << 8)
          | (7u << 3) | (3u << 1);
    ep[2] = (uint32_t)m->ring.bus | 1u;
    ep[3] = (uint32_t)(m->ring.bus >> 32);
    ep[4] = (payload << 16) | m->length;

    /*
     * **An Xbox One pad's OUT as well**, in the same Configure Endpoint: its
     * context index is twice its number (4.5.1), its type 3, Interrupt OUT
     * (Table 6-9), and the slot's Context Entries the larger of the two. Its
     * ring and the page its messages are written from are two more of the
     * device's pages.
     */
    if (found.kind == USB_CONFIG_XBOXONE) {
        unsigned out_dci = 2u * found.out_endpoint;
        uint32_t *out_ep = context(c, d->input, out_dci + 1u);

        m->out_dci = out_dci;
        m->say = (uint8_t *)(c->mem + (page + DEVICE_PAGE_PAD_SAY) * PAGE);
        m->say_bus = c->bus + (page + DEVICE_PAGE_PAD_SAY) * PAGE;
        ring_start(&m->out_ring,
                   (uint32_t *)(c->mem + (page + DEVICE_PAGE_PAD_OUT) * PAGE),
                   c->bus + (page + DEVICE_PAGE_PAD_OUT) * PAGE);

        icc[1] |= 1u << out_dci;

        if (out_dci > dci) {
            slot[0] = (slot[0] & ~(0x1Fu << 27)) | (out_dci << 27);
        }

        memset(out_ep, 0, c->context);
        out_ep[0] = interval_for(d->speed, found.out_interval) << 16;
        out_ep[1] = ((uint32_t)found.out_packet << 16) | (3u << 3)
                  | (3u << 1);
        out_ep[2] = (uint32_t)m->out_ring.bus | 1u;
        out_ep[3] = (uint32_t)(m->out_ring.bus >> 32);
        out_ep[4] = ((uint32_t)found.out_packet << 16) | 16u;
    }

    if (!command(c, (uint32_t)d->input_bus, (uint32_t)(d->input_bus >> 32),
                 TRB_TYPE(TRB_CONFIGURE) | TRB_SLOT(d->slot), done)) {
        say_failure(c, d->port, "Configure Endpoint",
                    "; it is not read", line);
        return;
    }

    if (!control_nodata(c, d, SET_CONFIGURATION, found.configuration, 0)) {
        say_failure(c, d->port, "SET_CONFIGURATION",
                    "; it is not read", line);
        return;
    }

    /*
     * **An Xbox 360 controller needs nothing more**: no Report descriptor to
     * read and no protocol to set - it is not HID - and it sends its reports
     * once it is configured. The OUT endpoint beside the IN is for its lights
     * and its rumble, and neither is asked for.
     */
    /*
     * **An Xbox One or Series pad says nothing until spoken to**: reading
     * starts, then the start-up is sent (`pad_start`) - power on, the light,
     * authenticated - and it is sent again if the pad announces itself
     * after, which some pads wait for (`pad_report`).
     */
    if (found.kind == USB_CONFIG_XBOXONE) {
        m->pad = true;
        m->xone = true;
        m->vendor = c->vendor[d->slot];
        m->product = c->product[d->slot];
        m->reading = true;
        ask_for_report(c, d->slot);
        pad_start(c, d->slot);

        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);
        say_text(line, ": an Xbox One or Series controller, read from "
                       "endpoint ");
        say_dec(line, found.endpoint);
        say_text(line, " and started on endpoint ");
        say_dec(line, found.out_endpoint);
        say_text(line, "; its buttons are keys");
        say_send(console, line);
        return;
    }

    if (found.kind == USB_CONFIG_XBOX360) {
        m->pad = true;
        m->reading = true;
        ask_for_report(c, d->slot);

        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);
        say_text(line, ": an Xbox 360 controller, read from endpoint ");
        say_dec(line, found.endpoint);
        say_text(line, ", up to ");
        say_dec(line, found.packet);
        say_text(line, " bytes; its buttons are keys");
        say_send(console, line);
        return;
    }

    /*
     * **A boot keyboard** - `roadmap.md` 5zd-b. Diego's ThinkCentre M700 has
     * no PS/2 port, so this is the machine's only keyboard.
     *
     * Nothing to read and nothing to work out: the boot protocol's report is
     * eight bytes with a fixed layout (HID 1.11 B.1), which is what the
     * firmware types on and what every keyboard offers. A Report descriptor
     * would say more - media keys, a keypad's own usages - and reading it is
     * the same step a mouse takes and is not this one.
     */
    if (found.kind == USB_CONFIG_BOOT_KEYBOARD) {
        if (!control_nodata(c, d, SET_PROTOCOL, PROTOCOL_BOOT,
                            found.interface)) {
            say_failure(c, d->port, "SET_PROTOCOL for the boot protocol",
                        "; the keyboard is not read", line);
            return;
        }

        m->keyboard = true;
        m->reading = true;
        ask_for_report(c, d->slot);

        about(line, c);
        say_text(line, " port ");
        say_dec(line, d->port);
        say_text(line, ": a keyboard, read from endpoint ");
        say_dec(line, found.endpoint);
        say_text(line, ", up to ");
        say_dec(line, found.packet);
        say_text(line, " bytes every ");
        say_dec(line, found.interval);
        say_text(line, " ms; its keys are the machine's");
        say_send(console, line);
        return;
    }

    if (found.report_length > 0) {
        why = "its Report descriptor could not be read";
    }

    if (found.report_length > 0 && found.report_length <= PAGE
        && control_in(c, d, GET_HID_DESCRIPTOR, DESC_REPORT, found.interface,
                      found.report_length)) {
        say_descriptor(c, d->port, d->buffer, found.report_length, line);
        usb_decode_mouse_report(d->buffer, found.report_length, &m->layout);
        why = "its Report descriptor lays out no relative X and Y";

        /* One report to a request: a longer one would arrive as several,
         * which nothing here puts back together. */
        if (m->layout.ok
            && (m->layout.id != 0 ? 1u : 0u) + (m->layout.bits + 7u) / 8u
               > found.packet) {
            m->layout.ok = false;
            why = "its reports are longer than its packet";
        }
    }

    if (!control_nodata(c, d, SET_PROTOCOL,
                        m->layout.ok ? PROTOCOL_REPORT : PROTOCOL_BOOT,
                        found.interface)) {
        say_failure(c, d->port,
                    m->layout.ok ? "SET_PROTOCOL for the report protocol"
                                 : "SET_PROTOCOL for the boot protocol",
                    "; the mouse is not read", line);
        return;
    }

    m->reading = true;
    ask_for_report(c, d->slot);

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);
    say_text(line, ": a mouse, read from endpoint ");
    say_dec(line, found.endpoint);
    say_text(line, ", up to ");
    say_dec(line, found.packet);
    say_text(line, " bytes every ");

    if (interval >= 3u) {
        say_dec(line, (125ul << interval) / 1000ul);
        say_text(line, " ms");
    } else {
        say_dec(line, 125ul << interval);
        say_text(line, " us");
    }

    say_send(console, line);

    about(line, c);
    say_text(line, " port ");
    say_dec(line, d->port);

    if (m->layout.ok) {
        say_text(line, ": its reports, by its descriptor: ");
        say_dec(line, m->layout.buttons);
        say_text(line, " buttons from bit ");
        say_dec(line, m->layout.buttons_at);
        say_text(line, ", X from bit ");
        say_dec(line, m->layout.x_at);
        say_text(line, " in ");
        say_dec(line, m->layout.x_bits);
        say_text(line, ", Y from bit ");
        say_dec(line, m->layout.y_at);
        say_text(line, " in ");
        say_dec(line, m->layout.y_bits);

        if (m->layout.id != 0) {
            say_text(line, ", after report ID ");
            say_dec(line, m->layout.id);
        } else {
            say_text(line, ", no report ID");
        }
    } else {
        say_text(line, ": read as a boot mouse, because ");
        say_text(line, why);
    }

    say_send(console, line);
}

/*
 * Stopped, and reset, before this process ends: Run/Stop and the interrupt
 * enable cleared, Halted awaited, then HCRST - which also forgets the context
 * array and the rings. The pages those pointed at go back to the kernel when
 * this process exits, and a controller still running would write into them.
 */
static void stop(const struct controller *c, struct say_line *line)
{
    mmio_write32(c->rt + RT_IMAN, IMAN_IP);
    (void)reset(c, line);
}

/*
 * Port Power, and whichever change bits are set, written back as ones. The
 * change bits are write-1-to-clear and nothing else in the word is written as a
 * one: PED would disable the port and PR reset it (Table 5-27). **Until every
 * change bit of a port is clear it raises no further change events** (4.19.2).
 * This driver reads the ports on every pass as well, so one that forgot would
 * see the same change again 50 ms later: with the write taken out, both
 * devices were unplugged and named again on every pass from boot. Written only
 * to a running controller, as 5.4.8 asks.
 */
static void clear_changes(const struct controller *c, unsigned port,
                          uint32_t sc)
{
    if ((sc & PORTSC_CHANGES) != 0) {
        mmio_write32(c->op + OP_PORTSC(port),
                     PORTSC_PP | (sc & PORTSC_CHANGES));
    }
}

/*
 * Whether a device just plugged in stayed plugged in for USB 2.0's debounce
 * interval (7.1.7.3, `ATTACH_MS`), which starts again when the connection
 * drops in the meantime. Each drop's change is cleared here, so the watch does
 * not see it again, and a connection still bouncing after `ATTACH_TRIES`
 * intervals is said and left for its next change.
 */
static bool settled(struct controller *c, unsigned port,
                    struct say_line *line)
{
    unsigned tries;

    for (tries = 0; tries < ATTACH_TRIES; tries++) {
        uint32_t sc;

        (void)wait_serving(c, ATTACH_MS, NULL);
        sc = mmio_read32(c->op + OP_PORTSC(port));

        if ((sc & PORTSC_CSC) == 0) {
            return (sc & PORTSC_CCS) != 0;
        }

        clear_changes(c, port, sc);

        if ((sc & PORTSC_CCS) == 0) {
            about(line, c);
            say_text(line, " port ");
            say_dec(line, port);
            say_text(line, ": plugged in and pulled out again");
            say_send(console, line);
            return false;
        }
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, port);
    say_text(line, ": a connection that would not settle, left for its next "
                   "change");
    say_send(console, line);
    return false;
}

/*
 * A device on a port: its debounce if it was just plugged in, its reset if it
 * is USB 2, the port's line, a slot and an address, what it says it is - and,
 * if it is a mouse, the mouse. The port keeps its slot, or the fact that a
 * device is there and was not named, so an unplug later can say which.
 *
 * **Debounced only when just plugged in** (7.1.7.3): contacts bounce as a
 * plug goes in, and a reset sent during that resets a device that is not
 * properly there yet. One found at boot has been in its socket since before
 * the firmware ran.
 */
static void attach(struct controller *c, unsigned port, bool running,
                   bool plugged, struct say_line *line)
{
    uint32_t sc;
    struct device d;
    bool reset_done = false;

    if (plugged && running && !settled(c, port, line)) {
        return;
    }

    sc = mmio_read32(c->op + OP_PORTSC(port));
    c->port_slot[port] = PORT_FAILED;

    /*
     * **A speed only where the field holds one.** Table 5-27: the speed "is
     * invalid on a USB2 protocol port until after the port is reset", because
     * a USB 2 device says how fast it is during that reset. The ThinkPad
     * showed what printing it anyway looks like: four USB 2 ports, every one
     * of them "Full-speed", which nothing had yet asked.
     */
    if (running && c->usb[port] == 2) {
        reset_done = reset_port(c, port);
        sc = mmio_read32(c->op + OP_PORTSC(port));
    }

    about(line, c);
    say_text(line, " port ");
    say_dec(line, port);

    if (c->usb[port] >= 3 || reset_done) {
        say_text(line, ", USB ");
        say_dec(line, c->usb[port]);
        say_text(line, ": a ");
        say_text(line, speed_name(PORTSC_SPEED(sc)));
        say_text(line, " device (speed ID ");
        say_dec(line, PORTSC_SPEED(sc));
        say_text(line, reset_done ? "), after its reset" : ")");
    } else if (c->usb[port] == 2) {
        say_text(line, running ? ", USB 2: a device whose port would not "
                                 "reset"
                               : ", USB 2: a device, its speed unknown until "
                                 "the port is reset");
    } else {
        say_text(line, ": a device, on a port no protocol capability "
                       "describes");
    }

    say_send(console, line);

    if (!running || (c->usb[port] == 2 && !reset_done) || c->usb[port] < 2) {
        return;
    }

    memset(&d, 0, sizeof(d));
    d.port = port;
    d.speed = PORTSC_SPEED(sc);

    if (!address_device(c, &d, line)) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, port);
        say_text(line, ": no slot and address for the device");
        say_send(console, line);
        return;
    }

    c->port_slot[port] = (unsigned char)d.slot;
    c->described[d.slot] = false;

    if (!describe(c, &d, line)) {
        about(line, c);
        say_text(line, " port ");
        say_dec(line, port);
        say_text(line, ": the device did not say what it is");
        say_send(console, line);
        return;
    }

    use_device(c, &d, line);
}

/*
 * A device gone from a port: a line saying what left, then its slot given
 * back. A controller hands out only slots that are not enabled, so a slot kept
 * is one fewer for every device after it - QEMU forgets which port the slot
 * was for, addresses the next device on that port in another, and runs out.
 */
static void detach(struct controller *c, unsigned port, struct say_line *line)
{
    unsigned slot = c->port_slot[port];
    struct mouse *m = (slot != 0 && slot != PORT_FAILED) ? &c->mouse[slot]
                                                          : NULL;

    about(line, c);
    say_text(line, " port ");
    say_dec(line, port);
    say_text(line, ": unplugged");

    if (slot != 0 && slot != PORT_FAILED && c->described[slot]) {
        say_text(line, ", ");
        say_hex(line, c->vendor[slot], 4);
        say_text(line, ":");
        say_hex(line, c->product[slot], 4);

        if (c->name[slot][0] != '\0') {
            say_text(line, " \"");
            say_text(line, c->name[slot]);
            say_text(line, "\"");
        }
    }

    if (m != NULL && m->dci != 0) {
        say_text(line, ", after ");
        say_dec(line, m->reports);
        say_text(line, m->reports == 1 ? " report" : " reports");
        say_text(line, ", ");
        say_dec(line, m->looked);
        say_text(line, " found by looking");
    }

    say_send(console, line);

    /*
     * **A mouse stops being read before its slot goes**, so a report the
     * controller finishes as the slot is disabled is not answered with a
     * request on a ring whose endpoint is gone - and a button it was holding
     * is let go, or the pointer would go on holding it until something else
     * pressed and released that button.
     */
    if (m != NULL) {
        bool held = m->buttons != 0;

        if (m->pad) {
            pad_keys(c, m, 0);          /* a pad unplugged mid-press */
        }

        memset(m, 0, sizeof(*m));

        if (held) {
            to_pointer(0, 0);
        }
    }

    if (slot != 0 && slot != PORT_FAILED) {
        stick_release_buffer(&c->stick[slot]);
        memset(&c->stick[slot], 0, sizeof(c->stick[slot]));
        disable_slot(c, slot);
        c->described[slot] = false;
    }

    c->port_slot[port] = 0;
}

/* One controller, start to finish. Answers how many ports have a device. */
static unsigned bring_up(struct controller *c, const struct dev_info *dev,
                         struct say_line *line)
{
    uint32_t word, hcs1;
    unsigned port, plugged = 0;
    long mapped;
    bool running;

    memset(c, 0, sizeof(*c));
    c->where = dev->where;
    c->size = (unsigned long)dev->size;
    c->irq = -1;

    mapped = kosmos_dev_map((unsigned long)dev->base,
                            (c->size + 4095u) / 4096u);

    if (mapped < 0) {
        about(line, c);
        say_text(line, " could not be mapped");
        say_send(console, line);
        return 0;
    }

    c->base = (uintptr_t)mapped;
    word = mmio_read32(c->base + CAP_LENGTH_VERSION);
    hcs1 = mmio_read32(c->base + CAP_HCSPARAMS1);
    c->op = c->base + (word & 0xFFu);
    c->ports = HCS1_PORTS(hcs1);

    about(line, c);
    say_text(line, ", version ");
    say_dec(line, (word >> 24) & 0xFFu);
    say_text(line, ".");
    say_dec(line, (word >> 20) & 0xFu);
    say_text(line, ", ");
    say_dec(line, c->ports);
    say_text(line, " ports, ");
    say_dec(line, HCS1_SLOTS(hcs1));
    say_text(line, " slots");
    say_send(console, line);

    if ((word & 0xFFu) + OP_PORTSC(c->ports) + 4u > c->size) {
        about(line, c);
        say_text(line, "'s ports lie outside its window");
        say_send(console, line);
        return 0;
    }

    walk_capabilities(c, line);

    if (!reset(c, line)) {
        return 0;
    }

    /*
     * A moment for the ports. A port reports a device while the controller
     * is halted (4.19.2), but a USB 3 link that the reset retrained needs
     * time to come back, and reading at once would miss it.
     */
    kosmos_sleep(ticks_for(SETTLE_MS));

    running = start(c, dev, line) && no_op(c, line);

    for (port = 1; port <= c->ports; port++) {
        uint32_t sc = mmio_read32(c->op + OP_PORTSC(port));

        /*
         * Cleared before anything is done about the port, so that a change
         * after this instant is one the watch sees rather than one the boot
         * scan already had.
         */
        if (running) {
            clear_changes(c, port, sc);
        }

        if ((sc & PORTSC_CCS) == 0) {
            continue;
        }

        plugged++;
        attach(c, port, running, false, line);
    }

    /*
     * **A running controller is kept running**, and watched. One that did not
     * start is stopped: its rings' pages go back to the kernel with this
     * process, and a controller still running would write into them.
     */
    if (running) {
        c->running = true;
    } else if (c->bus != 0) {
        stop(c, line);
    }

    return plugged;
}

/*
 * One controller looked at: its interrupt acknowledged, its event ring
 * emptied - a mouse's report read as it is taken, anything else passed over -
 * and its ports read. **The ports say what happened, not the events**: 4.19.2
 * promises no agreement between a read of PORTSC and the events already
 * written, so a port's event is only a reason to look sooner.
 *
 * EINT is cleared before IP, the order 5.4.2 gives, and both before the ring
 * is emptied, so an event written after the emptying raises the interrupt
 * again rather than waiting for the next look.
 *
 * A connect change on a port with a device already recorded is that device
 * leaving, even if another has arrived by the time the port is read; so it is
 * detached first and whatever the port now holds attached after - and while
 * either waits, every controller's mice go on being read (`wait_serving`).
 */
static void service(struct controller *c, struct say_line *line)
{
    uint32_t trb[4];
    unsigned port;

    acknowledge(c);

    /*
     * **Every kind of unasked-for event, not only a mouse's report.**
     *
     * This drained into `take_report` alone, so an Ethernet adapter's frames
     * and its link notifications were read only when something else happened
     * to be inside `wait_serving` - which during a plug is often and once the
     * machine is idle is never. Exactly one frame ever reached the stack:
     * the one that arrived while the adapter was still being configured.
     *
     * The three are the three things that arrive because the *device* had
     * something to say. Each returns at once if the event is not its own.
     */
    while (take_event(c, trb)) {
        take_report(c, trb);
        take_note(c, trb);
        take_frame(c, trb);
    }

    for (port = 1; port <= c->ports; port++) {
        uint32_t sc = mmio_read32(c->op + OP_PORTSC(port));

        if ((sc & PORTSC_CHANGES) == 0) {
            continue;
        }

        clear_changes(c, port, sc);

        if ((sc & PORTSC_CSC) == 0) {
            continue;
        }

        if (c->port_slot[port] != 0) {
            detach(c, port, line);
        }

        if ((sc & PORTSC_CCS) != 0) {
            attach(c, port, true, true, line);
        }
    }
}

/*
 * **A client of the block protocol** (`blockproto.h`, USB step 5d): a region
 * handed over with `BLOCK_OP_OPEN`, mapped here once, and named by a handle
 * whose low byte is its place and whose other bits a generation - so a handle
 * kept past its close, or guessed, names nothing rather than another client's
 * region. `OPENS_MAX` at once; a client that ends without closing keeps its
 * slot, and the region's address here, for the life of this process.
 */
#define OPENS_MAX           8u

struct opened {
    bool          used;
    uint32_t      generation;
    long          cap;
    uintptr_t     at;
    unsigned long pages;
};

static struct opened opens[OPENS_MAX];
static long blocks_endpoint = -1;
static long writes_endpoint = -1;       /* the disk server's, and nobody else's */


/*
 * **The stick given the number `unit`**, while it is ready (`units_named`).
 *
 * Until 5e a unit was the `unit`th stick ready, counting controllers and then
 * slots, which is a position and not a name: a stick plugged into an earlier
 * controller moved every stick after it up one, and the disk server's writes
 * went to the stick just plugged in (`usb.md` §7, `testing.md` §18.60).
 */
static bool find_unit(uint32_t unit, struct controller **c_out,
                      struct stick **s_out)
{
    struct stick *s;
    unsigned i, slot;

    for (i = 0; i < controllers_found; i++) {
        for (slot = 1; slot <= DEVICES_MAX; slot++) {
            s = &controllers[i].stick[slot];

            if (s->ready && s->unit == unit) {
                *c_out = &controllers[i];
                *s_out = s;
                return true;
            }
        }
    }

    return false;
}

static struct opened *opened_by(uint32_t handle)
{
    uint32_t place = handle & 0xFFu;
    struct opened *o;

    if (place == 0u || place > OPENS_MAX) {
        return NULL;
    }

    o = &opens[place - 1u];
    return (o->used && o->generation == handle >> 8) ? o : NULL;
}

/* A region at least one read long, kept and mapped; its handle answered. */
static void block_open(long cap, struct block_reply *rep)
{
    long pages = cap < 0 ? -1 : kosmos_mem_size(cap);
    long at;
    unsigned i;

    if (pages < (long)(BLOCK_TRANSFER_MOST / PAGE)) {
        rep->error = BLOCK_ERR_NO_REGION;
        return;
    }

    for (i = 0; i < OPENS_MAX && opens[i].used; i++) {
    }

    if (i == OPENS_MAX) {
        rep->error = BLOCK_ERR_FULL;
        return;
    }

    at = kosmos_mem_map(cap);

    if (at < 0) {
        rep->error = BLOCK_ERR_NO_REGION;
        return;
    }

    opens[i].used = true;
    opens[i].generation = (opens[i].generation + 1u) & 0xFFFFFFu;
    opens[i].cap = cap;
    opens[i].at = (uintptr_t)at;
    opens[i].pages = (unsigned long)pages;
    rep->handle = (i + 1u) | (opens[i].generation << 8);
}

static void block_close(uint32_t handle, struct block_reply *rep)
{
    struct opened *o = opened_by(handle);

    if (o == NULL) {
        rep->error = BLOCK_ERR_NO_REGION;
        return;
    }

    (void)kosmos_share_unmap(o->at, o->pages);
    (void)kosmos_cap_drop(o->cap);
    o->used = false;
}

/*
 * **A read**: held to the unit, the handle, one read's size and the stick's
 * last block before the stick is asked anything - so a block past the end is
 * refused here, by name, rather than failed by the stick. Then READ (10)
 * through the stick's transfer buffer, with Reset Recovery if it goes wrong
 * (`transact`), and the blocks copied into the client's region.
 */
/*
 * **What a read and a write are both held to**, before the stick is asked
 * anything: a unit that is ready, a handle that names an open region, a count
 * from one to what one transfer can move, and a last block no further than the
 * stick's. One function, so the two cannot come to disagree. False, with the
 * reply's error set, when any is not so.
 */
static bool block_range(const struct block_request *req,
                        struct block_reply *rep, struct opened **o,
                        struct controller **c, struct stick **s)
{
    *o = opened_by(req->handle);

    if (!find_unit(req->unit, c, s)) {
        rep->error = BLOCK_ERR_NO_UNIT;
        return false;
    }

    rep->block_size = (*s)->block_size;
    rep->blocks = (*s)->blocks;

    if (*o == NULL) {
        rep->error = BLOCK_ERR_NO_REGION;
        return false;
    }

    if (req->count == 0u
        || req->count > BLOCK_TRANSFER_MOST / (*s)->block_size) {
        rep->error = BLOCK_ERR_TOO_MANY;
        return false;
    }

    if (req->lba >= (*s)->blocks || req->count > (*s)->blocks - req->lba) {
        rep->error = BLOCK_ERR_PAST_END;
        return false;
    }

    return true;
}

static void block_read(const struct block_request *req, struct say_line *line,
                       struct block_reply *rep)
{
    struct opened *o;
    struct controller *c;
    struct stick *s;
    uint8_t cdb[16];
    enum bot_status status;
    unsigned got, bytes;
    const char *failed;

    if (!block_range(req, rep, &o, &c, &s)) {
        return;
    }

    bytes = req->count * s->block_size;

    failed = transact(c, &s->dev, s, "READ (10)", cdb,
                      scsi_read_10(cdb, (uint32_t)req->lba,
                                   (uint16_t)req->count),
                      false, NULL, bytes, &got, &status, line);

    if (failed != NULL) {
        say_failure(c, s->dev.port, failed, "", line);
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    if (status != BOT_PASSED) {
        say_why_failed(c, &s->dev, s, "READ (10)", line);
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    if (got != bytes) {
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    memcpy((void *)o->at, (void *)s->transfer, bytes);
    rep->count = req->count;
}

/*
 * **A write** (USB step 5e): held to what a read is held to, then the client's
 * blocks copied out of its region into the stick's transfer buffer and sent
 * with WRITE (10). The copy runs the other way from a read's, and for the
 * same reason: a client's pages are never the controller's to touch. Reset
 * Recovery and a second try come with `transact`, and the buffer still holds
 * the same bytes when the command goes again.
 */
static void block_write(const struct block_request *req, struct say_line *line,
                        struct block_reply *rep)
{
    struct opened *o;
    struct controller *c;
    struct stick *s;
    uint8_t cdb[16];
    enum bot_status status;
    unsigned got, bytes;
    const char *failed;

    if (!block_range(req, rep, &o, &c, &s)) {
        return;
    }

    bytes = req->count * s->block_size;
    memcpy((void *)s->transfer, (void *)o->at, bytes);

    failed = transact(c, &s->dev, s, "WRITE (10)", cdb,
                      scsi_write_10(cdb, (uint32_t)req->lba,
                                    (uint16_t)req->count),
                      true, NULL, bytes, &got, &status, line);

    if (failed != NULL) {
        say_failure(c, s->dev.port, failed, "", line);
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    if (status != BOT_PASSED) {
        say_why_failed(c, &s->dev, s, "WRITE (10)", line);
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    rep->count = req->count;
}

/*
 * **A flush** (USB step 5e): SYNCHRONIZE CACHE (10) on every block the unit
 * holds, and no data either way - only the stick's word that what it has
 * acknowledged, it has kept. The disk server asks after each write to its
 * journal's header block, which is the instant the journal's promise is
 * about (`usb.md` §7). A stick that fails it says why, and the disk server is
 * told rather than left to believe it.
 *
 * **And a stick that does not do it is told once.** The ThinkPad's Kingston
 * answers every SYNCHRONIZE CACHE (10) with ILLEGAL REQUEST, 20h/00h - no such
 * command - and was asked twice a commit, each time with a REQUEST SENSE after
 * it and a line on the screen. The command block never changes, so neither
 * does the answer: once `scsi_not_supported` says so, the stick is sent no
 * other, and a flush is answered BLOCK_ERR_NO_FLUSH without a transfer.
 */
static void block_flush(const struct block_request *req, struct say_line *line,
                        struct block_reply *rep)
{
    struct controller *c;
    struct stick *s;
    struct scsi_sense sense;
    uint8_t cdb[16];
    enum bot_status status;
    unsigned got;
    const char *failed;
    bool known;

    if (!find_unit(req->unit, &c, &s)) {
        rep->error = BLOCK_ERR_NO_UNIT;
        return;
    }

    rep->block_size = s->block_size;
    rep->blocks = s->blocks;

    if (s->no_flush) {
        rep->error = BLOCK_ERR_NO_FLUSH;
        return;
    }

    failed = transact(c, &s->dev, s, "SYNCHRONIZE CACHE (10)", cdb,
                      scsi_synchronize_cache_10(cdb), false, NULL, 0u, &got,
                      &status, line);

    if (failed != NULL) {
        say_failure(c, s->dev.port, failed, "", line);
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    if (status != BOT_PASSED) {
        known = why_failed(c, &s->dev, s, &sense, line);

        if (known && scsi_not_supported(&sense)) {
            s->no_flush = true;
            about(line, c);
            say_text(line, " port ");
            say_dec(line, s->dev.port);
            say_text(line, ": the stick does not do SYNCHRONIZE CACHE (10), "
                           "so it is not asked again: ");
            say_sense(line, &sense);
            say_send(console, line);
            rep->error = BLOCK_ERR_NO_FLUSH;
            return;
        }

        say_failed(c, &s->dev, "SYNCHRONIZE CACHE (10)", known, &sense, line);
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    /*
     * Said once a stick. What a flush buys is invisible until the power goes,
     * so this line is the only sign one was ever sent and kept - on the
     * ThinkPad, whether its stick takes the command at all.
     */
    if (!s->flushed) {
        s->flushed = true;
        about(line, c);
        say_text(line, " port ");
        say_dec(line, s->dev.port);
        say_text(line, ": the stick wrote out its cache when asked, by "
                       "SYNCHRONIZE CACHE (10)");
        say_send(console, line);
    }
}

/*
 * One request, answered: exactly a `struct block_request` long, or refused as
 * `audio.c` refuses one. A capability that arrives with anything but an open
 * is given back rather than kept. `may_write` is which endpoint it came in on,
 * and a write or a flush from `/dev/blocks` is refused as read only.
 */
static void block_answer(const struct message *in, uint64_t sender, long cap,
                         bool may_write, struct say_line *line)
{
    struct message out;
    struct block_reply *rep = (struct block_reply *)(void *)out.data;
    const struct block_request *req =
        (const struct block_request *)(const void *)in->data;
    struct controller *c;
    struct stick *s;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length != sizeof(*req)) {
        rep->error = BLOCK_ERR_BAD_OP;
    } else {
        switch (req->op) {
        case BLOCK_OP_INFO:
            rep->count = units_named;   /* every unit is below it, there or not */
            if (find_unit(req->unit, &c, &s)) {
                rep->block_size = s->block_size;
                rep->blocks = s->blocks;
                memcpy(rep->vendor, s->vendor, sizeof(rep->vendor));
                memcpy(rep->product, s->product, sizeof(rep->product));
            } else {
                rep->error = BLOCK_ERR_NO_UNIT;
            }
            break;
        case BLOCK_OP_OPEN:
            block_open(cap, rep);
            if (rep->error == BLOCK_OK) {
                cap = -1;               /* kept, in `opens` */
            }
            break;
        case BLOCK_OP_READ:
            block_read(req, line, rep);
            break;
        case BLOCK_OP_WRITE:
            if (may_write) {
                block_write(req, line, rep);
            } else {
                rep->error = BLOCK_ERR_READ_ONLY;
            }
            break;
        case BLOCK_OP_FLUSH:
            if (may_write) {
                block_flush(req, line, rep);
            } else {
                rep->error = BLOCK_ERR_READ_ONLY;
            }
            break;
        case BLOCK_OP_CLOSE:
            block_close(req->handle, rep);
            break;
        default:
            rep->error = BLOCK_ERR_BAD_OP;
            break;
        }
    }

    if (cap >= 0) {
        (void)kosmos_cap_drop(cap);
    }

    (void)kosmos_reply(sender, &out);
}

/* Every request waiting on one endpoint, answered without blocking. */
static void drain(long endpoint, bool may_write, struct say_line *line)
{
    struct message msg;
    uint64_t sender = 0;

    while (endpoint >= 0
           && kosmos_receive(endpoint, &msg, &sender, 1, 0) == 0) {
        long cap = msg.cap_plus_one > 0 ? (long)msg.cap_plus_one - 1 : -1;

        block_answer(&msg, sender, cap, may_write, line);
    }
}

/*
 * **Both block endpoints, after every wake**: the write endpoint's requests,
 * which may write, then `/dev/blocks`', which may not. Both are on the watch's
 * wait, so a caller on either wakes it at once.
 */
static void serve_blocks(struct say_line *line)
{
    drain(writes_endpoint, true, line);
    drain(blocks_endpoint, false, line);
}

/*
 * **A driver with nothing to drive still answers.** The block endpoint is
 * init's, and it is in the capability list every program is started with; a
 * destroyed one makes the kernel refuse each of those spawns, so a machine
 * with no USB controller could start no program at all - which is what
 * `run_headless.py` found the first time this ended by destroying it. So with
 * no controller to watch, this stays, as the audio and network servers stay
 * on a machine with no card, and answers every request: no stick at that
 * unit. Only a receive the kernel refuses - an endpoint that is not one -
 * ends it.
 *
 * **`/dev/blocks` alone**, because with no interrupt line a wait can be on one
 * endpoint and no more. Nothing is lost by it: no stick can be ready here, and
 * the disk server finds its partition through `/dev/blocks` before it ever
 * calls the write endpoint (`usb.md` §7).
 */
static void serve_without_controllers(int code)
{
    struct say_line line;
    struct message msg;
    uint64_t sender = 0;
    const long ends[2] = { blocks_endpoint, frames_endpoint };

    say_begin(&line);

    if (blocks_endpoint < 0 && frames_endpoint < 0) {
        kosmos_exit(code);
    }

    for (;;) {
        /*
         * **Both endpoints, and no interrupt lines**, which the kernel's
         * wait takes since 22 September. The network stack asks this process
         * whether there is an adapter even on a machine with no USB
         * controller at all, and its call would wait for ever on a receive
         * that only watched `/dev/blocks` - so the stack would never
         * start. Polling the two instead would be wakes a second on a
         * machine where nothing is happening.
         */
        long woke = kosmos_irq_wait_any(NULL, 0, 0, ends, 2u);

        if (woke < 0 && woke != SYS_NO_INTERRUPT) {
            kosmos_exit(code);
        }

        while (blocks_endpoint >= 0
               && kosmos_receive(blocks_endpoint, &msg, &sender, 1, 0) == 0) {
            block_answer(&msg, sender,
                         msg.cap_plus_one > 0 ? (long)msg.cap_plus_one - 1 : -1,
                         false, &line);
        }

        serve_frames(&line);
    }
}

/*-------------------------------------------------------- the stack's frames */

/*
 * **The adapter the stack is holding**, or none - `usb.md` 7d.
 *
 * Found by walking the controllers rather than kept as a pointer, because an
 * adapter unplugged leaves its `struct ether` behind with nothing driving it:
 * `receiving` is what says it is still there.
 */
static struct ether *attached_adapter(struct controller **c_out,
                                      unsigned *slot_out)
{
    unsigned i, slot;

    for (i = 0; i < controllers_found; i++) {
        for (slot = 1; slot <= DEVICES_MAX; slot++) {
            struct ether *e = &controllers[i].ether[slot];

            if (e->frames != 0 && e->have_mac) {
                if (c_out != NULL) {
                    *c_out = &controllers[i];
                }

                if (slot_out != NULL) {
                    *slot_out = slot;
                }

                return e;
            }
        }
    }

    return NULL;
}

static void eth_about(struct eth_reply *rep, const struct ether *e)
{
    rep->present = 1;
    rep->mtu = e->max_segment;
    rep->link = (e->link_said && e->link) ? 1u : 0u;
    rep->sent = (uint32_t)e->sent;
    rep->received = (uint32_t)e->received;
    memcpy(rep->mac, e->mac, sizeof(rep->mac));
}

/*
 * **Everything in the ring's `out`, onto the wire.**
 *
 * The stack writes as many frames as it has and calls once, so this drains
 * rather than sending one: the number of messages is the number of times the
 * stack went idle rather than the number of frames.
 *
 * A frame that will not go out is counted and stepped over. There is nothing
 * else to do with it - the stack has moved on, and a driver that stopped
 * draining would hold every frame behind the one the wire refused.
 */
static void eth_drain_out(struct controller *c, unsigned slot,
                          struct ether *e, unsigned packet)
{
    uint32_t read = frames_ring->out_read;
    uint32_t write = eth_ring_acquire(&frames_ring->out_write);

    while (eth_ring_ready(write, read) > 0) {
        uint32_t length = frames_ring->out_length[read % ETH_RING_SLOTS];

        if (length >= 14u && length <= ETH_RING_SLOT) {
            (void)ether_send(c, slot, eth_ring_out(frames_ring, read),
                             length, packet);
        }

        read++;
        eth_ring_publish(&frames_ring->out_read, read);
    }

    (void)e;
}

/*
 * One request from the network stack, answered - `ethproto.h`.
 *
 * **Attach takes the region and keeps it**, mapped here for as long as this
 * process runs, exactly as the audio server keeps a stream's ring. A second
 * attach while one is held is refused rather than taken: two stacks on one
 * wire is not a thing this system has, and silently swapping the ring under
 * the first would be worse than saying no.
 */
static void eth_answer(const struct message *msg, uint64_t sender, long cap,
                       struct say_line *line)
{
    struct eth_request req;
    struct eth_reply rep;
    struct message out;
    struct controller *c = NULL;
    struct ether *e;
    unsigned slot = 0;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));

    if (msg->length >= sizeof(req)) {
        memcpy(&req, msg->data, sizeof(req));
    } else {
        rep.error = ETH_ERR_BAD_OP;
    }

    e = attached_adapter(&c, &slot);

    if (rep.error == ETH_OK && e == NULL) {
        rep.error = ETH_ERR_NO_ADAPTER;
    }

    if (rep.error == ETH_OK) {
        switch (req.op) {
        case ETH_OP_ATTACH: {
            long at;

            if (frames_ring != NULL) {
                rep.error = ETH_ERR_TAKEN;
                break;
            }

            at = cap < 0 ? -1 : kosmos_mem_map(cap);

            if (at < 0 || !eth_ring_valid((struct eth_ring *)(uintptr_t)at)) {
                rep.error = ETH_ERR_NO_RING;
                break;
            }

            frames_ring = (struct eth_ring *)(uintptr_t)at;
            frames_ring_cap = cap;
            frames_on = c;
            frames_slot = slot;
            eth_about(&rep, e);

            about(line, c);
            say_text(line, " port ");
            say_dec(line, e->port);
            say_text(line, ": the network stack has it; frames go through a "
                           "ring of ");
            say_dec(line, ETH_RING_SLOTS);
            say_text(line, " each way");
            say_send(console, line);
            break;
        }

        case ETH_OP_SEND:
            if (frames_ring == NULL) {
                rep.error = ETH_ERR_NO_RING;
                break;
            }

            eth_drain_out(frames_on, frames_slot,
                          &frames_on->ether[frames_slot],
                          frames_on->ether[frames_slot].out_packet);
            eth_about(&rep, e);
            break;

        case ETH_OP_INFO:
            eth_about(&rep, e);
            break;

        default:
            rep.error = ETH_ERR_BAD_OP;
            break;
        }
    }

    memset(&out, 0, sizeof(out));
    out.tag = msg->tag;
    out.length = sizeof(rep);
    memcpy(out.data, &rep, sizeof(rep));
    (void)kosmos_reply(sender, &out);
}

/* Whatever the stack has asked for since the last look, answered. */
static void serve_frames(struct say_line *line)
{
    struct message msg;
    uint64_t sender = 0;

    while (frames_endpoint >= 0
           && kosmos_receive(frames_endpoint, &msg, &sender, 1, 0) == 0) {
        eth_answer(&msg, sender,
                   msg.cap_plus_one > 0 ? (long)msg.cap_plus_one - 1 : -1,
                   line);
    }
}

/*
 * Until the machine stops: **every running controller's interrupt waited on
 * at once**, and every controller looked at after any of them.
 *
 * It waited on each in turn, `WATCH_MS` apiece, which a plug or an unplug can
 * afford and a mouse cannot: a mouse on the second controller would have its
 * reports sit behind the first controller's wait and reach the pointer in
 * bursts, ten a second. `SYS_IRQ_WAIT_ANY` is the kernel's wait on all of
 * them. A controller whose interrupt could not be claimed is not among them,
 * and is looked at when the wait's deadline comes round - polled, as its
 * line said at the start.
 *
 * **And whether each look was the controller's own interrupt**, kept in
 * `woken` for the pass: a mouse's report taken on any other look - a
 * deadline, or the other controller's interrupt - is counted as found by
 * looking, and the line when the mouse leaves says how many. A mouse whose
 * reports stop coming by interrupt still moves, only badly, and that count
 * is what a photograph can read it from.
 */
static void watch(struct controller *list, unsigned count,
                  struct say_line *line)
{
    long lines[IRQ_WAIT_ANY_MAX];
    unsigned owner[IRQ_WAIT_ANY_MAX] = { 0 };
    unsigned waited = 0, i;

    for (i = 0; i < count; i++) {
        if (list[i].running && list[i].irq >= 0
            && waited < IRQ_WAIT_ANY_MAX) {
            owner[waited] = i;
            lines[waited++] = list[i].irq;
        }
    }

    for (;;) {
        long woke = SYS_NO_INTERRUPT;

        /* And this driver's endpoints on the same wait: the disk server's
         * write endpoint (USB step 5c), `/dev/blocks`, and the network
         * stack's frames (7d). An endpoint not on the wait waits out
         * WATCH_MS - 17 a second, on the ThinkPad and under QEMU alike -
         * which is what watching one cost and is why the kernel now watches
         * three. A caller answers IRQ_WAIT_CALLER + its place, a line with
         * an interrupt first; all three are served after every wake. */
        const long ends[3] = { writes_endpoint, blocks_endpoint,
                               frames_endpoint };

        if (waited > 0) {
            woke = kosmos_irq_wait_any(lines, waited, ticks_for(WATCH_MS),
                                       ends, 3u);
        }

        /* Nothing to wait on, or a wait refused: slept instead, never spun. */
        if (waited == 0 || (woke < 0 && woke != SYS_NO_INTERRUPT)) {
            kosmos_sleep(ticks_for(WATCH_MS));
        }

        for (i = 0; i < count; i++) {
            if (list[i].running) {
                list[i].woken = woke >= 0 && (unsigned long)woke < waited
                                && owner[woke] == i;
                service(&list[i], line);
            }
        }

        serve_blocks(line);
        serve_frames(line);
    }
}

void xhci_server(long console_cap, long blocks_cap, long writes_cap,
                 long frames_cap)
{
    struct sysinfo info = { 0 };
    struct dev_info dev;
    struct say_line line;
    unsigned index, i, plugged = 0, named = 0;
    bool watching = false;
    long asked = 0;

    console = console_cap;
    blocks_endpoint = blocks_cap;
    writes_endpoint = writes_cap;
    frames_endpoint = frames_cap;

    if (kosmos_sysinfo(&info) == 0) {
        tick_hz = info.tick_hz != 0 ? info.tick_hz : tick_hz;
        counter_hz = info.counter_hz != 0 ? (unsigned long)info.counter_hz
                                          : counter_hz;
    }

    for (index = 0; index < NAMED_MAX
         && (asked = kosmos_dev_find(DEV_XHCI, index, &dev)) == 0; index++) {
        controllers_found = index + 1u;
        plugged += bring_up(&controllers[index], &dev, &line);
        named += controllers[index].named;
        watching = watching || controllers[index].running;
    }

    if (index < NAMED_MAX && asked != SYS_ERR_NO_DEVICE) {
        say_begin(&line);
        say_text(&line, "xhci: the board would not say where the "
                        "controllers are");
        say_send(console, &line);
        serve_without_controllers(1);
    }

    if (index == 0) {
        serve_without_controllers(0);   /* no USB controller: not an error */
    }

    /*
     * **The addresses as well as the count.** The ThinkPad's photograph
     * said "2 controllers" beside the lines of only one: the other's were
     * printed before the shell's banner and had scrolled away. A closing
     * line that names them is one photograph instead of two.
     */
    say_begin(&line);
    say_text(&line, "xhci: ");
    say_dec(&line, index);
    say_text(&line, index == 1 ? " controller (" : " controllers (");

    for (i = 0; i < index; i++) {
        if (i > 0) {
            say_text(&line, ", ");
        }

        address(&line, controllers[i].where);
    }

    say_text(&line, "), ");
    say_dec(&line, plugged);
    say_text(&line, plugged == 1 ? " port with something plugged in, "
                                 : " ports with something plugged in, ");
    say_dec(&line, named);
    say_text(&line, named == 1 ? " device named" : " devices named");
    say_send(console, &line);

    if (!watching) {
        serve_without_controllers(0);
    }

    say_begin(&line);
    say_text(&line, "xhci: watching for devices plugged in and out");
    say_send(console, &line);

    watch(controllers, index, &line);
}
