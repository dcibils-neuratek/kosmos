/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * NVMe: the disk a machine built this decade actually has.
 *
 * **This is what `docs/thinkpad.md` records as the reason `/home` is in
 * memory.** The T14 has no SATA port and no virtio anything; its storage is
 * an NVMe drive on PCIe, and with no driver for it the machine reaches a
 * desktop and forgets everything the moment it is switched off. Every other
 * board here has virtio-blk, which is why this is the first real storage
 * driver in the tree.
 *
 * **It is also, unusually, a driver that can be finished before the
 * hardware is available.** QEMU has `-device nvme`, and it is not a
 * simplified model - it is the same register file, the same queue
 * discipline and the same command set. So the whole of this can be brought
 * up, and the filesystem's thirty-three disk checks run against it, on this
 * desk. `simulate-dont-wait` is the standing rule and this is the case it
 * was written for.
 *
 * --------------------------------------------------------------------
 *
 * **The shape of NVMe, and why it is less work than it sounds.**
 *
 * A disk controller used to be a register file you poked. NVMe is a pair of
 * *rings in the guest's own memory*: a submission queue the driver writes
 * commands into, and a completion queue the device writes results into. The
 * only registers that matter are the handful that say where those rings are
 * and a doorbell per ring saying "I have moved my index".
 *
 * That is the same idea as virtio, and the same idea as the HDA driver's
 * CORB and RIRB. Three devices in this tree now work this way, which is
 * worth noticing: it is what a fast device looks like, and the differences
 * between them are naming rather than structure.
 *
 * There are two pairs of queues here. The **admin** pair is created by
 * writing its addresses into registers, and exists to ask the controller
 * what it is and to create the other pair. The **I/O** pair is created by
 * an admin command, and is what reads and writes actually travel on. A
 * controller may have many I/O pairs, one per processor, which is where
 * NVMe's parallelism comes from; this has one, because `hal_blk_read` is
 * synchronous and a second queue would be capacity nothing asks for.
 *
 * --------------------------------------------------------------------
 *
 * **Polled, with no interrupt at all**, and that is a decision rather than
 * a shortcut. `hal.h` declares reading and writing as synchronous calls
 * that return when the bytes are there; the filesystem is a server that
 * blocks on them. An interrupt would let this yield the processor while the
 * disk works, which is worth having and is a change to the *interface*
 * rather than to this file - the completion path would be the same ring
 * with a wake at the end of it. Until `hal_blk_read` can return "not yet",
 * an interrupt would be a handler that sets a flag this loop is already
 * spinning on.
 *
 * --------------------------------------------------------------------
 *
 * **Written against the specification from knowledge rather than from a
 * copy of it**, exactly as `virtio/blk.c` was, and the same warning
 * applies: the field offsets and bit positions below are the part to
 * distrust. What establishes them is the test that writes a sector and
 * reads it back, because every one of these numbers is on the path between
 * those two operations and a wrong one cannot produce the bytes that went
 * in. A driver that merely *initialises* proves almost nothing.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "hal.h"
#include "mmio.h"
#include "mmu.h"
#include "pci.h"
#include "nvme.h"
#include "spinlock.h"

/* `pc_timer_wait_ms`, which is declared in an odd place - see `apic.c`,
 * which uses it for the same reason: a millisecond of real time during
 * bring-up, before the scheduler exists to sleep on. */
void pc_timer_wait_ms(unsigned ms);

/*--------------------------------------------------------------------------
 * The register file. NVMe 1.4 section 3.1.
 *
 * The 64-bit ones are read and written as two 32-bit halves, which the
 * specification explicitly permits and which is what this tree's MMIO rule
 * gives us: `mmio_read32` and `mmio_write32` carry the barriers, and there
 * is no 64-bit pair to reach for.
 *------------------------------------------------------------------------*/

#define REG_CAP     0x00u           /* capabilities, 64-bit */
#define REG_VS      0x08u           /* version */
#define REG_INTMS   0x0cu           /* interrupt mask set */
#define REG_CC      0x14u           /* controller configuration */
#define REG_CSTS    0x1cu           /* controller status */
#define REG_AQA     0x24u           /* admin queue attributes */
#define REG_ASQ     0x28u           /* admin submission queue base, 64-bit */
#define REG_ACQ     0x30u           /* admin completion queue base, 64-bit */

#define CC_EN       (1u << 0)
#define CC_CSS_NVM  (0u << 4)       /* the NVM command set */
#define CC_MPS_4K   (0u << 7)       /* 2^(12 + n) bytes; n = 0 is 4 KB */
#define CC_AMS_RR   (0u << 11)      /* round robin arbitration */
#define CC_IOSQES   (6u << 16)      /* 2^6 = 64 bytes, the size of an SQE */
#define CC_IOCQES   (4u << 20)      /* 2^4 = 16 bytes, the size of a CQE */

#define CSTS_RDY    (1u << 0)
#define CSTS_CFS    (1u << 1)       /* fatal: the controller has given up */

/* Admin opcodes. NVMe 1.4 figure 139. */
#define ADMIN_CREATE_SQ 0x01u
#define ADMIN_CREATE_CQ 0x05u
#define ADMIN_IDENTIFY  0x06u

/* NVM command set opcodes. NVMe 1.4 figure 346. */
#define NVM_WRITE       0x01u
#define NVM_READ        0x02u

#define IO_QID          1u          /* the one I/O queue pair */

/*
 * How deep the rings are.
 *
 * Thirty-two, and the number is almost arbitrary because exactly one
 * command is ever in flight: this driver submits, rings the doorbell and
 * waits. What the depth actually buys is nothing today and everything the
 * moment reading becomes asynchronous, which is why it is not two.
 *
 * It must not exceed CAP.MQES + 1, which is checked at bring-up rather than
 * assumed - a controller is entitled to offer less.
 */
#define QUEUE_SLOTS 32u

struct sqe { uint32_t dw[16]; };     /* 64 bytes */
struct cqe { uint32_t dw[4];  };     /* 16 bytes */

_Static_assert(sizeof(struct sqe) == 64, "a submission entry is 64 bytes");
_Static_assert(sizeof(struct cqe) == 16, "a completion entry is 16 bytes");

/*
 * The rings, and the one buffer everything travels through.
 *
 * Statically declared and page aligned, because the kernel has no allocator
 * and because the controller is given *physical* addresses for all of them.
 * The kernel is identity mapped, so the address of one of these arrays is
 * the address the device is told - the same assumption `virtio/blk.c` and
 * `hda.c` already make, and the one line to revisit if that ever stops
 * being true.
 */
static _Alignas(4096) struct sqe admin_sq[QUEUE_SLOTS];
static _Alignas(4096) struct cqe admin_cq[QUEUE_SLOTS];
static _Alignas(4096) struct sqe io_sq[QUEUE_SLOTS];
static _Alignas(4096) struct cqe io_cq[QUEUE_SLOTS];

/*
 * **A bounce buffer, and it is not a performance apology.**
 *
 * A command describes its data with PRPs - physical region pages - and the
 * rules are strict: the first may begin part-way into a page, every one
 * after it must be page aligned, and a transfer crossing more than two
 * pages needs a *list* of them in yet another page. A caller's buffer is a
 * `void *` of any alignment.
 *
 * So one page-aligned page here, filled or drained by `memcpy`, turns every
 * transfer into the one case that is provably right: a single PRP, no list,
 * no crossing. It costs a copy per four kilobytes, and it means the whole
 * PRP-list path - the part of NVMe most likely to be got subtly wrong -
 * does not exist to be got wrong. When there is a measurement that says
 * this copy matters, the list is the thing to add, with the copy still
 * there for the unaligned case.
 */
static _Alignas(4096) uint8_t bounce[4096];

/* Where `identify` puts its 4096-byte answer. */
static _Alignas(4096) uint8_t identity[4096];

static struct {
    uintptr_t base;                 /* the mapped register file */
    unsigned  doorbell_stride;      /* bytes between doorbells: 4 << CAP.DSTRD */
    unsigned  admin_tail, admin_head;
    unsigned  io_tail, io_head;
    bool      admin_phase, io_phase;
    uint16_t  next_id;
    uint64_t  sectors;              /* in HAL_BLK_SECTOR units */
    uint32_t  lba_bytes;
    bool      present;
} nvme;

static const char *description = "no NVMe controller on the bus";

/*--------------------------------------------------------------------------
 * Doorbells.
 *
 * One per queue, submission and completion alternating, starting at 0x1000.
 * The stride between them is `4 << CAP.DSTRD` - almost always 4, and
 * *almost* is why it is read rather than assumed: a controller that spaces
 * them further apart would have this driver ringing the wrong queue, which
 * looks exactly like a device that never answers.
 *------------------------------------------------------------------------*/

static uintptr_t doorbell(unsigned queue, bool completion)
{
    return nvme.base + 0x1000u
         + (uintptr_t)(2u * queue + (completion ? 1u : 0u))
           * nvme.doorbell_stride;
}

/*
 * The data pointer of a command, both halves of it.
 *
 * Written as a function because the high word was `0` at three call sites,
 * with the excuse that everything this kernel maps is below four gigabytes.
 * That is true today and is a property of where the image happens to load
 * rather than anything this driver arranges - and the failure it buys, if
 * it ever stops being true, is a controller told to write into physical
 * memory at an address missing its top half.
 */
static void set_prp1(struct sqe *command, const void *at)
{
    uint64_t pa = (uint64_t)(uintptr_t)at;

    command->dw[6] = (uint32_t)pa;
    command->dw[7] = (uint32_t)(pa >> 32);
}

static uint64_t read64(unsigned reg)
{
    uint64_t low = mmio_read32(nvme.base + reg);

    return low | ((uint64_t)mmio_read32(nvme.base + reg + 4) << 32);
}

static void write64(unsigned reg, uint64_t value)
{
    mmio_write32(nvme.base + reg, (uint32_t)value);
    mmio_write32(nvme.base + reg + 4, (uint32_t)(value >> 32));
}

/*
 * Wait for CSTS.RDY to become what is wanted, or give up.
 *
 * The deadline is the controller's own: CAP.TO is in 500 ms units and is
 * the longest it may take. Waiting less is a driver that fails on a slow
 * drive; waiting for ever is a machine that hangs at boot stage seven with
 * nothing on the screen, which is the failure this whole tree has spent a
 * week learning to avoid.
 */
static bool wait_ready(bool want, unsigned timeout_ms)
{
    unsigned waited;

    for (waited = 0; waited <= timeout_ms; waited += 10) {
        uint32_t csts = mmio_read32(nvme.base + REG_CSTS);

        if ((csts & CSTS_CFS) != 0) {
            return false;           /* it has declared itself broken */
        }

        if (((csts & CSTS_RDY) != 0) == want) {
            return true;
        }

        pc_timer_wait_ms(10);
    }

    return false;
}

/*--------------------------------------------------------------------------
 * Submitting one command and waiting for it.
 *
 * **The phase bit is the whole trick, and it is why the completion queue
 * needs no doorbell to be read.** The controller flips a bit in each entry
 * it writes; the driver keeps its own idea of what "new" looks like and
 * flips that every time it wraps. So a completion is recognised by the
 * entry disagreeing with memory the driver already had, rather than by an
 * index the device would have to publish.
 *
 * The same idea as virtio's used index and HDA's write pointer, arrived at
 * from the other direction.
 *------------------------------------------------------------------------*/

static bool submit(unsigned queue, struct sqe *ring, struct cqe *completions,
                   unsigned *tail, unsigned *head, bool *phase,
                   const struct sqe *command, uint32_t *result)
{
    volatile struct cqe *entry;
    unsigned waited;
    uint16_t id = nvme.next_id++;
    struct sqe out = *command;

    out.dw[0] = (out.dw[0] & 0x0000ffffu) | ((uint32_t)id << 16);

    ring[*tail] = out;
    *tail = (*tail + 1) % QUEUE_SLOTS;

    mmio_write32(doorbell(queue, false), *tail);

    entry = (volatile struct cqe *)&completions[*head];

    /*
     * **Spun on tightly, and only then slept on.**
     *
     * A millisecond between looks would put a floor of a millisecond under
     * every single disk read, which for a filesystem doing a few hundred of
     * them is most of a second of pure waiting. A finished command is
     * usually there within microseconds, so the inner loop simply looks -
     * `entry` is volatile, so the read is real every time - and the
     * millisecond only happens when it was not.
     *
     * Five seconds in total, which is far longer than any command here
     * should need and far shorter than for ever. A command that never
     * answers is a controller this driver has misconfigured, and the useful
     * outcome is "no disk" rather than a boot that stops with nothing on
     * the screen.
     */
    for (waited = 0; waited < 5000u; waited += 1) {
        unsigned spin;

        for (spin = 0; spin < 20000u; spin++) {
            uint32_t status = entry->dw[3];

            if (((status >> 16) & 1u) == (*phase ? 1u : 0u)) {
                unsigned code = (status >> 17) & 0x7ffu;

                if (result != NULL) {
                    *result = entry->dw[0];
                }

                *head = (*head + 1) % QUEUE_SLOTS;

                if (*head == 0) {
                    *phase = !*phase;   /* wrapped: "new" is the other bit */
                }

                mmio_write32(doorbell(queue, true), *head);

                return code == 0;
            }
        }

        pc_timer_wait_ms(1);
    }

    return false;
}

static bool admin(const struct sqe *command, uint32_t *result)
{
    return submit(0, admin_sq, admin_cq, &nvme.admin_tail, &nvme.admin_head,
                  &nvme.admin_phase, command, result);
}

/*--------------------------------------------------------------------------
 * Bring-up.
 *------------------------------------------------------------------------*/

static bool identify(uint32_t nsid, uint32_t cns)
{
    struct sqe command;

    memset(&command, 0, sizeof(command));
    memset(identity, 0, sizeof(identity));

    command.dw[0] = ADMIN_IDENTIFY;
    command.dw[1] = nsid;
    set_prp1(&command, identity);
    command.dw[10] = cns;

    return admin(&command, NULL);
}

static bool create_queues(void)
{
    struct sqe command;

    /* The completion queue first: a submission queue names the completion
     * queue its results go to, so the other order asks the controller to
     * point at something that does not exist yet. */
    memset(&command, 0, sizeof(command));
    command.dw[0] = ADMIN_CREATE_CQ;
    set_prp1(&command, io_cq);
    command.dw[10] = IO_QID | ((QUEUE_SLOTS - 1u) << 16);
    command.dw[11] = 1u;            /* physically contiguous; interrupts off */

    if (!admin(&command, NULL)) {
        return false;
    }

    memset(&command, 0, sizeof(command));
    command.dw[0] = ADMIN_CREATE_SQ;
    set_prp1(&command, io_sq);
    command.dw[10] = IO_QID | ((QUEUE_SLOTS - 1u) << 16);
    command.dw[11] = 1u | (IO_QID << 16);   /* contiguous, into that CQ */

    return admin(&command, NULL);
}

bool nvme_init(struct blkdev *out)
{
    struct pci_device dev;
    uint64_t cap;
    uint32_t lba_format;
    unsigned mqes;

    /*
     * Class 1 subclass 8 programming interface 2 is "NVM Express", and
     * finding it by class is the only way that works: every drive has its
     * maker's own vendor and device identifiers, so matching on those would
     * be a driver for exactly one model of disk. The same argument
     * `pci.c` makes for the sound controller.
     */
    if (!pci_find_class(0x01, 0x08, 0, &dev, NULL)) {
        return false;
    }

    pci_enable(&dev);

    if (dev.bar[0] == 0) {
        description = "an NVMe controller with no register window";
        return false;
    }

    nvme.base = mmu_map_device((uintptr_t)dev.bar[0], 0x2000);

    if (nvme.base == 0) {
        description = "an NVMe controller and no room to map it";
        return false;
    }

    cap = read64(REG_CAP);
    mqes = (unsigned)(cap & 0xffffu) + 1u;
    nvme.doorbell_stride = 4u << ((cap >> 32) & 0xfu);

    if (mqes < QUEUE_SLOTS) {
        description = "an NVMe controller whose queues are shorter than ours";
        return false;
    }

    /*
     * Off, before anything is configured. A controller left enabled by the
     * firmware is pointing at the firmware's queues, and writing new
     * addresses underneath a running controller is undefined in the exact
     * way that corrupts a disk.
     */
    mmio_write32(nvme.base + REG_CC, 0);

    if (!wait_ready(false, 500u * (unsigned)((cap >> 24) & 0xffu) + 500u)) {
        description = "an NVMe controller that would not reset";
        return false;
    }

    memset(admin_sq, 0, sizeof(admin_sq));
    memset(admin_cq, 0, sizeof(admin_cq));
    memset(io_sq, 0, sizeof(io_sq));
    memset(io_cq, 0, sizeof(io_cq));

    nvme.admin_tail = nvme.admin_head = 0;
    nvme.io_tail = nvme.io_head = 0;
    nvme.admin_phase = nvme.io_phase = true;
    nvme.next_id = 1;

    mmio_write32(nvme.base + REG_AQA,
                 (QUEUE_SLOTS - 1u) | ((QUEUE_SLOTS - 1u) << 16));
    write64(REG_ASQ, (uint64_t)(uintptr_t)admin_sq);
    write64(REG_ACQ, (uint64_t)(uintptr_t)admin_cq);

    mmio_write32(nvme.base + REG_CC,
                 CC_EN | CC_CSS_NVM | CC_MPS_4K | CC_AMS_RR
                 | CC_IOSQES | CC_IOCQES);

    if (!wait_ready(true, 500u * (unsigned)((cap >> 24) & 0xffu) + 500u)) {
        description = "an NVMe controller that would not come ready";
        return false;
    }

    /*
     * Every interrupt masked, because nothing here handles one.
     *
     * `pci_enable` switches a device to MSI where it can, which is right for
     * every other driver on this bus and is a loaded gun here: this one
     * polls, so a completion interrupt would arrive at a vector with no
     * handler behind it. The I/O queue is created with its interrupts
     * disabled a few lines below; this covers the admin queue, which has no
     * such field and is masked through the register instead.
     */
    mmio_write32(nvme.base + REG_INTMS, 0xffffffffu);

    /*
     * Namespace 1, which is the one every consumer drive has and the only
     * one this asks for. A controller may present several - that is what
     * the identifier is for - and choosing between them is a policy
     * question the HAL has nowhere to put: `struct blkdev` is one disk.
     */
    if (!identify(1, 0)) {
        description = "an NVMe controller that would not describe namespace 1";
        return false;
    }

    /*
     * NSZE at byte 0 is the namespace size in logical blocks. FLBAS at 26
     * says which of the LBA formats at 128 is in use, and each format's
     * LBADS is the power of two its blocks are.
     */
    {
        uint64_t blocks = 0;
        unsigned which;
        unsigned i;

        for (i = 0; i < 8; i++) {
            blocks |= (uint64_t)identity[i] << (i * 8);
        }

        which = identity[26] & 0x0fu;

        lba_format = (uint32_t)identity[128 + which * 4]
                   | ((uint32_t)identity[128 + which * 4 + 1] << 8)
                   | ((uint32_t)identity[128 + which * 4 + 2] << 16)
                   | ((uint32_t)identity[128 + which * 4 + 3] << 24);

        nvme.lba_bytes = 1u << ((lba_format >> 16) & 0xffu);

        /*
         * **512-byte blocks only, and said out loud rather than assumed.**
         *
         * `hal.h` counts in 512-byte sectors, so a drive formatted with
         * 4096-byte blocks needs every request translated and every partial
         * write turned into a read-modify-write. That is real work and it
         * is not this evening's: a drive that reports anything else is
         * refused here, the machine says so on its boot line, and `/home`
         * stays in memory - which is exactly what happens today and is
         * therefore no worse than before.
         */
        if (nvme.lba_bytes != HAL_BLK_SECTOR) {
            description = "an NVMe drive whose blocks are not 512 bytes";
            return false;
        }

        nvme.sectors = blocks;
    }

    if (!create_queues()) {
        description = "an NVMe controller that would not make an I/O queue";
        return false;
    }

    nvme.present = true;
    description = "NVMe";

    out->sectors = nvme.sectors;
    out->sector_size = HAL_BLK_SECTOR;

    return true;
}

/*--------------------------------------------------------------------------
 * Reading and writing.
 *------------------------------------------------------------------------*/

/*
 * **One command at a time.** The I/O queue pair, its doorbells and the
 * bounce buffer are one of each for the machine, so a read on one core and a
 * write on another would share all three. Taken per chunk rather than for a
 * whole transfer, because a lock masks interrupts and a large read is many
 * commands: held for one command, polled to its completion.
 */
static struct spinlock nvme_lock = SPINLOCK("nvme");

static bool transfer(uint64_t sector, void *buf, uint32_t bytes, bool write)
{
    uint8_t *p = buf;

    if (!nvme.present || bytes == 0 || (bytes % HAL_BLK_SECTOR) != 0) {
        return false;
    }

    if (sector + bytes / HAL_BLK_SECTOR > nvme.sectors) {
        return false;
    }

    while (bytes > 0) {
        uint32_t chunk = bytes > sizeof(bounce) ? (uint32_t)sizeof(bounce)
                                                : bytes;
        uint32_t blocks = chunk / HAL_BLK_SECTOR;
        struct sqe command;
        unsigned long flags;
        bool done;

        flags = spin_lock(&nvme_lock);

        if (write) {
            memcpy(bounce, p, chunk);
        }

        memset(&command, 0, sizeof(command));
        command.dw[0] = write ? NVM_WRITE : NVM_READ;
        command.dw[1] = 1;                          /* namespace 1 */
        set_prp1(&command, bounce);
        command.dw[10] = (uint32_t)sector;
        command.dw[11] = (uint32_t)(sector >> 32);
        command.dw[12] = blocks - 1u;               /* zero-based count */

        done = submit(IO_QID, io_sq, io_cq, &nvme.io_tail, &nvme.io_head,
                      &nvme.io_phase, &command, NULL);

        if (done && !write) {
            memcpy(p, bounce, chunk);
        }

        spin_unlock(&nvme_lock, flags);

        if (!done) {
            return false;
        }

        p += chunk;
        bytes -= chunk;
        sector += blocks;
    }

    return true;
}

bool nvme_read(uint64_t sector, void *buf, uint32_t bytes)
{
    return transfer(sector, buf, bytes, false);
}

bool nvme_write(uint64_t sector, const void *buf, uint32_t bytes)
{
    /* The cast is safe: `transfer` only reads through it when writing. */
    return transfer(sector, (void *)(uintptr_t)buf, bytes, true);
}

bool nvme_present(void)
{
    return nvme.present;
}

const char *nvme_describe(void)
{
    return description;
}
