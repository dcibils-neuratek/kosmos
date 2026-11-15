/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **A SATA controller in AHCI mode** (`docs/roadmap.md`, *SATA (AHCI), and
 * `/Home` on the M700's own drive*): the M700's drive is behind an Intel
 * SATA controller (8086:a102) that nothing drove, and q35 has the ICH9's
 * (8086:2922), which is where this is tried first.
 *
 * A process, as the USB driver is: device authority to find the controller
 * and map its registers, memory whose bus address it knows for the
 * controller to read commands from and write data to, and the console to
 * say what it found.
 *
 * **It finds, then serves, and writes nothing** (A1, then A2). The controller
 * is put into AHCI mode and taken from the firmware where it offers that;
 * each implemented port with a disk on it has its command list and its
 * received-FIS area set and is started; each disk is asked IDENTIFY DEVICE
 * and its first two sectors are read - which says whether there is a GPT -
 * and all of it is said. Then its blocks are served in `blockproto.h`'s
 * shape - the protocol a USB stick's blocks are served in - on two
 * endpoints, as the USB driver's are: `/Devices/disks`, which every program
 * is given and which only reads (A2), and a write endpoint only the disk
 * servers are given (A3), which takes writes, flushes and guards.
 *
 * **A write lands only inside a partition a disk server has claimed**: the
 * range it guarded, which is the partition it serves. Windows is on the
 * M700's disk, and a request to write anywhere else is refused here, by the
 * driver, whoever sends it - so no bug in a disk server can reach Windows'
 * partitions or the table that names them. The disks in the suites have a
 * GPT and a partition like Windows', and the bytes outside Kosmos's
 * partition must be the same afterwards.
 *
 * **References**: Serial ATA AHCI 1.3.1 - §3 the registers, §4.2 the
 * command list and table, §10.1.2 the order a port is brought up in, §10.6
 * the BIOS/OS hand-off; ACS-3 for the commands. The offsets were checked
 * against QEMU's `hw/ide/ahci-internal.h`.
 *
 * **A command is polled, with a deadline**, at the probe and in a request:
 * watched closely for the first two milliseconds - an SSD answers a read in a
 * tenth of one - and then once a scheduler tick. That is a request waiting on
 * its hardware, which `CLAUDE.md` asks a driver not to do; it is the shape
 * the USB driver's bulk transfers have too, it is one disk's request at a
 * time, and the controller's interrupt, with requests held while a command
 * runs, is the step after (`roadmap.md`, *SATA*).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "kosmos.h"
#include "mmio.h"
#include "init/say.h"
#include "ahci_decode.h"
#include "blockproto.h"

/* The host's registers (AHCI 1.3.1 §3.1). */
#define HBA_CAP         0x00u
#define HBA_GHC         0x04u
#define HBA_IS          0x08u
#define HBA_PI          0x0Cu
#define HBA_VS          0x10u
#define HBA_CAP2        0x24u
#define HBA_BOHC        0x28u

#define CAP_S64A        (1u << 31)      /* 64-bit addresses */
#define CAP_NCS(c)      ((((c) >> 8) & 0x1Fu) + 1u)
#define GHC_AE          (1u << 31)      /* AHCI enable */
#define GHC_IE          (1u << 1)
#define CAP2_BOH        (1u << 0)       /* BIOS/OS hand-off supported */
#define BOHC_BOS        (1u << 0)       /* the BIOS owns it */
#define BOHC_OOS        (1u << 1)       /* the OS asks for it */
#define BOHC_BB         (1u << 4)       /* the BIOS is busy */

/* A port's registers, from 0x100, 0x80 apiece (§3.3). */
#define PORT_BASE(n)    (0x100u + 0x80u * (n))
#define PX_CLB          0x00u
#define PX_CLBU         0x04u
#define PX_FB           0x08u
#define PX_FBU          0x0Cu
#define PX_IS           0x10u
#define PX_IE           0x14u
#define PX_CMD          0x18u
#define PX_TFD          0x20u
#define PX_SIG          0x24u
#define PX_SSTS         0x28u
#define PX_SERR         0x30u
#define PX_CI           0x38u

#define CMD_ST          (1u << 0)
#define CMD_FRE         (1u << 4)
#define CMD_FR          (1u << 14)
#define CMD_CR          (1u << 15)

#define TFD_ERR         (1u << 0)
#define TFD_DRQ         (1u << 3)
#define TFD_BSY         (1u << 7)
#define IS_TFES         (1u << 30)       /* task file error */

#define SSTS_DET(s)     ((s) & 0xFu)
#define SSTS_IPM(s)     (((s) >> 8) & 0xFu)
#define DET_PRESENT     3u               /* a device, and Phy talking to it */
#define IPM_ACTIVE      1u

/*
 * **One port's memory, one page of it**: the command list (32 headers, 1 KB,
 * 1 KB aligned) at 0, the received-FIS area (256 bytes, 256 aligned) at 0x400
 * and one command table (128 aligned: the FIS at 0, the PRDT at 0x80) at
 * 0x800 - slot 0 is the only one used. The data goes through a buffer of
 * its own, BUFFER_PAGES - the most one request moves, `BLOCK_TRANSFER_MOST` -
 * which the controller reads and writes and nothing else ever hands it: a
 * client's pages are never given to the controller (`blockproto.h`).
 */
#define AT_LIST         0x000u
#define AT_FIS          0x400u
#define AT_TABLE        0x800u
#define TABLE_PRDT      0x80u
#define BUFFER_PAGES    (BLOCK_TRANSFER_MOST / 4096u)  /* one request's most */

#define PORTS_MOST      32u
#define WAIT_MS         2000u            /* a command, a port stopping, BSY */
#define CLOSE_US        2000u            /* a command watched closely this long */
#define UNITS_MOST      PORTS_MOST
#define OPENS_MAX       8u

#if defined(__x86_64__)
/* x86 keeps stores in order, so the command table is seen before the
 * uncached write to PxCI; what is wanted is the compiler not to move them. */
#define DMA_BARRIER()   __asm__ volatile("" ::: "memory")
#else
#define DMA_BARRIER()   __asm__ volatile("dsb sy" ::: "memory")
#endif

struct port {
    unsigned  n;
    uint8_t  *mem;          /* the page: list, FIS area, table */
    uint64_t  mem_bus;
    uint8_t  *data;
    uint64_t  data_bus;
    struct ahci_identity id;
};

static long console = -1;
static uintptr_t abar;
static uint32_t cap;
static unsigned long counter_hz = 62500000ul;

static uint32_t hba(uint32_t off) { return mmio_read32(abar + off); }
static void hba_set(uint32_t off, uint32_t v) { mmio_write32(abar + off, v); }
static uint32_t px(unsigned n, uint32_t off) { return mmio_read32(abar + PORT_BASE(n) + off); }
static void px_set(unsigned n, uint32_t off, uint32_t v) { mmio_write32(abar + PORT_BASE(n) + off, v); }

/* Whether `(read() & mask) == want` comes true within `ms`. */
static bool wait_until(unsigned n, uint32_t off, uint32_t mask, uint32_t want, unsigned ms)
{
    unsigned long start = kosmos_ticks(), span = counter_hz / 1000ul * ms;

    for (;;) {
        uint32_t v = (n == PORTS_MOST) ? hba(off) : px(n, off);

        if ((v & mask) == want) {
            return true;
        }

        if (kosmos_ticks() - start > span) {
            return false;
        }

        kosmos_sleep(1);
    }
}

static void tell(const char *a, unsigned long n, const char *b)
{
    struct say_line line;

    say_begin(&line);
    say_text(&line, "ahci: ");
    say_text(&line, a);

    if (b != NULL) {
        say_dec(&line, n);
        say_text(&line, b);
    }

    say_send(console, &line);
}

/* Memory the controller can reach: contiguous, its bus address known, and
 * below 4 GB unless the controller says it takes 64-bit addresses. */
static uint8_t *dma(unsigned pages, uint64_t *bus)
{
    long region = kosmos_mem_create_flags(pages, MEM_CONTIGUOUS);
    long at, phys;

    if (region < 0 || (at = kosmos_mem_map(region)) < 0 || (phys = kosmos_mem_phys(region)) < 0) {
        return NULL;
    }

    if ((cap & CAP_S64A) == 0 && (uint64_t)phys + (uint64_t)pages * 4096u > 0x100000000ull) {
        return NULL;
    }

    *bus = (uint64_t)phys;
    return (uint8_t *)(uintptr_t)at;
}

/*
 * **The firmware lets go** (§10.6.3), where the controller can say who owns
 * it: ask, wait for the BIOS to clear its bit, and if it was busy, give it
 * the two seconds the specification allows.
 */
static void take_from_firmware(void)
{
    if ((hba(HBA_CAP2) & CAP2_BOH) == 0) {
        return;
    }

    hba_set(HBA_BOHC, hba(HBA_BOHC) | BOHC_OOS);

    if (!wait_until(PORTS_MOST, HBA_BOHC, BOHC_BOS, 0, 25)) {
        tell("the firmware did not let go of the controller", 0, NULL);
    }

    if ((hba(HBA_BOHC) & BOHC_BB) != 0) {
        wait_until(PORTS_MOST, HBA_BOHC, BOHC_BB, 0, 2000);
    }
}

/* A port's engines off - ST, then FRE - before its memory is changed. */
static bool stop(unsigned n)
{
    uint32_t c = px(n, PX_CMD);

    if ((c & CMD_ST) != 0) {
        px_set(n, PX_CMD, c & ~CMD_ST);
    }

    if (!wait_until(n, PX_CMD, CMD_CR, 0, 500)) {
        return false;
    }

    c = px(n, PX_CMD);

    if ((c & CMD_FRE) != 0) {
        px_set(n, PX_CMD, c & ~CMD_FRE);
    }

    return wait_until(n, PX_CMD, CMD_FR, 0, 500);
}

/*
 * **A port brought up** (§10.1.2): stopped, its command list and FIS area
 * set, FIS receive on, its errors cleared, and - once the disk is neither
 * busy nor asking for data - started.
 */
static bool bring_up(struct port *p)
{
    unsigned n = p->n;

    if (!stop(n)) {
        tell("port ", n, " would not stop; left alone");
        return false;
    }

    p->mem = dma(1, &p->mem_bus);
    p->data = dma(BUFFER_PAGES, &p->data_bus);

    if (p->mem == NULL || p->data == NULL) {
        tell("no memory the controller can reach for port ", n, "");
        return false;
    }

    px_set(n, PX_CLB, (uint32_t)(p->mem_bus + AT_LIST));
    px_set(n, PX_CLBU, (uint32_t)((p->mem_bus + AT_LIST) >> 32));
    px_set(n, PX_FB, (uint32_t)(p->mem_bus + AT_FIS));
    px_set(n, PX_FBU, (uint32_t)((p->mem_bus + AT_FIS) >> 32));
    px_set(n, PX_CMD, px(n, PX_CMD) | CMD_FRE);
    px_set(n, PX_SERR, 0xFFFFFFFFu);        /* write-one-to-clear */
    px_set(n, PX_IS, 0xFFFFFFFFu);
    px_set(n, PX_IE, 0);                     /* polled, for now */

    if (!wait_until(n, PX_TFD, TFD_BSY | TFD_DRQ, 0, WAIT_MS)) {
        tell("the disk on port ", n, " stayed busy; left alone");
        return false;
    }

    px_set(n, PX_CMD, px(n, PX_CMD) | CMD_ST);
    return true;
}

/*
 * **One command in slot 0**: its FIS, one PRDT entry over the buffer, and
 * the header pointing at the table; issued, and waited for until the
 * controller clears the slot or the disk reports an error. `bytes` is what
 * moves through the buffer - none for FLUSH CACHE EXT, which then has no
 * PRDT entry at all.
 */
static bool command(struct port *p, uint8_t ata, uint64_t lba, uint32_t count,
                    uint32_t bytes, bool write)
{
    unsigned n = p->n;
    uint8_t *list = p->mem + AT_LIST;
    uint8_t *table = p->mem + AT_TABLE;
    uint8_t *prdt = table + TABLE_PRDT;
    uint64_t table_bus = p->mem_bus + AT_TABLE;
    uint32_t word0;

    if (bytes > BUFFER_PAGES * 4096u) {
        return false;
    }

    memset(table, 0, TABLE_PRDT + 16u);
    ahci_fis_command(table, ata, lba, count);

    /* PRDT entry: the buffer's address, then its count less one (§4.2.3.3). */
    for (unsigned i = 0; i < 4; i++) {
        prdt[i] = (uint8_t)(p->data_bus >> (8 * i));
        prdt[4 + i] = (uint8_t)(p->data_bus >> (32 + 8 * i));
    }

    {
        uint32_t dbc = ahci_prdt_count(bytes);

        memcpy(prdt + 12, &dbc, 4);
    }

    /* The header in slot 0: its first word, PRDBC zero, the table's address. */
    memset(list, 0, 32);
    word0 = ahci_header_word(write, bytes != 0 ? 1u : 0u);
    memcpy(list, &word0, 4);

    for (unsigned i = 0; i < 4; i++) {
        list[8 + i] = (uint8_t)(table_bus >> (8 * i));
        list[12 + i] = (uint8_t)(table_bus >> (32 + 8 * i));
    }

    px_set(n, PX_IS, 0xFFFFFFFFu);
    DMA_BARRIER();
    px_set(n, PX_CI, 1u);

    {
        unsigned long start = kosmos_ticks(), span = counter_hz / 1000ul * WAIT_MS;
        unsigned long close = counter_hz / 1000000ul * CLOSE_US;

        for (;;) {
            if ((px(n, PX_IS) & IS_TFES) != 0 || (px(n, PX_TFD) & TFD_ERR) != 0) {
                return false;
            }

            if ((px(n, PX_CI) & 1u) == 0) {
                break;
            }

            if (kosmos_ticks() - start > span) {
                return false;
            }

            /* Closely at first, then once a tick (the top of this file). */
            if (kosmos_ticks() - start > close) {
                kosmos_sleep(1);
            }
        }
    }

    DMA_BARRIER();
    return (px(n, PX_TFD) & TFD_ERR) == 0;
}

/* What a disk is, and its first two sectors, said; whether it can be
 * served - its sectors 512 bytes, and read. */
static bool look(struct port *p)
{
    struct say_line line;
    bool gpt;

    if (!command(p, ATA_IDENTIFY, 0, 0, 512, false) || !ahci_identity(p->data, &p->id)) {
        tell("the disk on port ", p->n, " did not answer IDENTIFY");
        return false;
    }

    say_begin(&line);
    say_text(&line, "ahci: port ");
    say_dec(&line, p->n);
    say_text(&line, ", ");
    say_text(&line, p->id.model);
    say_text(&line, ", ");
    say_dec(&line, (unsigned long)(p->id.sectors * p->id.sector_bytes / (1024u * 1024u)));
    say_text(&line, " MB in ");
    say_dec(&line, (unsigned long)p->id.sectors);
    say_text(&line, " sectors of ");
    say_dec(&line, p->id.sector_bytes);
    say_send(console, &line);

    if (p->id.sector_bytes != 512u) {
        tell("a disk whose sectors are not 512 bytes is not read yet, port ", p->n, "");
        return false;
    }

    if (!command(p, ATA_READ_DMA_EXT, 0, 2, 1024, false)) {
        tell("the disk on port ", p->n, " did not read its first sectors");
        return false;
    }

    gpt = ahci_is_gpt_header(p->data + 512);

    say_begin(&line);
    say_text(&line, "ahci: port ");
    say_dec(&line, p->n);
    say_text(&line, gpt ? " holds a GPT" : " holds no GPT");
    say_text(&line, p->data[510] == 0x55 && p->data[511] == 0xAA ? ", its first sector a protective MBR"
                                                                   : ", its first sector no MBR");
    say_send(console, &line);
    return true;
}

/*
 * ---- Serving the blocks (A2) ---------------------------------------------
 *
 * **A unit is a disk found**, numbered in the order its port is, from 0. A
 * SATA disk does not come and go as a stick does, so the numbers never have
 * gaps; INFO answers how many there are in `count`, as the USB driver's does.
 *
 * **A region is opened once** and named by a handle after, as
 * `/Devices/blocks`' are: the handle is its slot and a generation, so a stale
 * one is refused rather than taken for somebody else's.
 */
struct opened {
    bool          used;
    uint32_t      generation;
    long          cap;
    uintptr_t     at;
    unsigned long pages;
};

static struct port *units[UNITS_MOST];
static unsigned units_found;
static struct opened opens[OPENS_MAX];

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

static void block_open(long cap, struct block_reply *rep)
{
    long pages = cap < 0 ? -1 : kosmos_mem_size(cap);
    long at;
    unsigned i;

    if (pages < (long)(BLOCK_TRANSFER_MOST / 4096u)) {
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

/* A disk's names as INQUIRY's would be: "ATA", and its model's first
 * sixteen characters, space-padded. */
static void names(const struct port *p, struct block_reply *rep)
{
    size_t n = strlen(p->id.model);

    memset(rep->vendor, ' ', sizeof(rep->vendor));
    memcpy(rep->vendor, "ATA", 3);
    memset(rep->product, ' ', sizeof(rep->product));
    memcpy(rep->product, p->id.model, n < sizeof(rep->product) ? n : sizeof(rep->product));
}

/*
 * **A read**: held to the unit, the handle, one transfer's size and the
 * disk's last sector before the disk is asked anything, then READ DMA EXT
 * through the port's buffer and the sectors copied into the client's region.
 */
static void block_read(const struct block_request *req, struct block_reply *rep)
{
    struct opened *o = opened_by(req->handle);
    struct port *p;

    if (req->unit >= units_found) {
        rep->error = BLOCK_ERR_NO_UNIT;
        return;
    }

    p = units[req->unit];
    rep->block_size = p->id.sector_bytes;
    rep->blocks = p->id.sectors;

    if (o == NULL) {
        rep->error = BLOCK_ERR_NO_REGION;
        return;
    }

    if (req->count == 0u || req->count > BLOCK_TRANSFER_MOST / p->id.sector_bytes) {
        rep->error = BLOCK_ERR_TOO_MANY;
        return;
    }

    if (req->lba >= p->id.sectors || req->count > p->id.sectors - req->lba) {
        rep->error = BLOCK_ERR_PAST_END;
        return;
    }

    if (!command(p, ATA_READ_DMA_EXT, req->lba, req->count,
                 req->count * p->id.sector_bytes, false)) {
        tell("a read failed on port ", p->n, "");
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    memcpy((void *)o->at, p->data, (size_t)req->count * p->id.sector_bytes);
    rep->count = req->count;
}

/* One request, answered: exactly a `struct block_request` long, or refused.
 * A capability with anything but an open is given back rather than kept. */
/*
 * **The ranges a disk server claimed** (`BLOCK_OP_GUARD`, on the write
 * endpoint): refused to reads on `/Devices/disks`, as the USB driver refuses
 * a stick's, so `/Keyring` is no more readable underneath the disk server
 * than through it - and the only ranges a write may touch.
 */
#define GUARDS_MOST     4u

struct guard {
    uint64_t lba, count;
};

static struct guard guards[UNITS_MOST][GUARDS_MOST];
static unsigned guards_held[UNITS_MOST];

static bool overlaps(unsigned unit, uint64_t lba, uint64_t count)
{
    for (unsigned i = 0; i < guards_held[unit]; i++) {
        if (lba < guards[unit][i].lba + guards[unit][i].count && lba + count > guards[unit][i].lba) {
            return true;
        }
    }

    return false;
}

static bool inside(unsigned unit, uint64_t lba, uint64_t count)
{
    for (unsigned i = 0; i < guards_held[unit]; i++) {
        if (lba >= guards[unit][i].lba && lba + count <= guards[unit][i].lba + guards[unit][i].count) {
            return true;
        }
    }

    return false;
}

/* What a read and a write are both held to: the unit, the handle, one
 * transfer's size and the disk's last sector. */
static bool in_range(const struct block_request *req, struct block_reply *rep,
                     struct opened **o, struct port **p)
{
    *o = opened_by(req->handle);

    if (req->unit >= units_found) {
        rep->error = BLOCK_ERR_NO_UNIT;
        return false;
    }

    *p = units[req->unit];
    rep->block_size = (*p)->id.sector_bytes;
    rep->blocks = (*p)->id.sectors;

    if (*o == NULL) {
        rep->error = BLOCK_ERR_NO_REGION;
        return false;
    }

    if (req->count == 0u || req->count > BLOCK_TRANSFER_MOST / (*p)->id.sector_bytes) {
        rep->error = BLOCK_ERR_TOO_MANY;
        return false;
    }

    if (req->lba >= (*p)->id.sectors || req->count > (*p)->id.sectors - req->lba) {
        rep->error = BLOCK_ERR_PAST_END;
        return false;
    }

    return true;
}

/* A write: inside a claimed range, or refused; then the sectors copied into
 * the port's buffer and WRITE DMA EXT. */
static void block_write(const struct block_request *req, struct block_reply *rep)
{
    struct opened *o;
    struct port *p;

    if (!in_range(req, rep, &o, &p)) {
        return;
    }

    if (!inside(req->unit, req->lba, req->count)) {
        rep->error = BLOCK_ERR_GUARDED;
        return;
    }

    memcpy(p->data, (const void *)o->at, (size_t)req->count * p->id.sector_bytes);

    if (!command(p, ATA_WRITE_DMA_EXT, req->lba, req->count,
                 req->count * p->id.sector_bytes, true)) {
        tell("a write failed on port ", p->n, "");
        rep->error = BLOCK_ERR_DEVICE;
        return;
    }

    rep->count = req->count;
}

static void block_flush(const struct block_request *req, struct block_reply *rep)
{
    if (req->unit >= units_found) {
        rep->error = BLOCK_ERR_NO_UNIT;
        return;
    }

    if (!command(units[req->unit], ATA_FLUSH_CACHE_EXT, 0, 0, 0, false)) {
        rep->error = BLOCK_ERR_DEVICE;
    }
}

static void block_guard(const struct block_request *req, struct block_reply *rep)
{
    struct say_line line;
    unsigned u = req->unit;

    if (u >= units_found) {
        rep->error = BLOCK_ERR_NO_UNIT;
        return;
    }

    if (req->count == 0u || req->lba >= units[u]->id.sectors
        || req->count > units[u]->id.sectors - req->lba) {
        rep->error = BLOCK_ERR_PAST_END;
        return;
    }

    if (!inside(u, req->lba, req->count)) {
        if (guards_held[u] == GUARDS_MOST) {
            rep->error = BLOCK_ERR_FULL;
            return;
        }

        guards[u][guards_held[u]].lba = req->lba;
        guards[u][guards_held[u]].count = req->count;
        guards_held[u]++;
    }

    say_begin(&line);
    say_text(&line, "ahci: disk ");
    say_dec(&line, u);
    say_text(&line, "'s sectors ");
    say_dec(&line, (unsigned long)req->lba);
    say_text(&line, " on, ");
    say_dec(&line, (unsigned long)req->count);
    say_text(&line, " of them, a disk server's: written only by it, and kept from /Devices/disks");
    say_send(console, &line);
}

static void block_answer(const struct message *in, uint64_t sender, long cap, bool may_write)
{
    struct message out;
    struct block_reply *rep = (struct block_reply *)(void *)out.data;
    const struct block_request *req = (const struct block_request *)(const void *)in->data;

    memset(&out, 0, sizeof(out));
    out.tag = in->tag;
    out.length = (uint32_t)sizeof(*rep);

    if (in->length != sizeof(*req)) {
        rep->error = BLOCK_ERR_BAD_OP;
    } else {
        switch (req->op) {
        case BLOCK_OP_INFO:
            rep->count = units_found;
            if (req->unit < units_found) {
                rep->block_size = units[req->unit]->id.sector_bytes;
                rep->blocks = units[req->unit]->id.sectors;
                names(units[req->unit], rep);
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
            /* A disk server's sectors, to anybody but a disk server: refused. */
            if (!may_write && req->unit < units_found && overlaps(req->unit, req->lba, req->count)) {
                rep->error = BLOCK_ERR_GUARDED;
            } else {
                block_read(req, rep);
            }
            break;
        case BLOCK_OP_CLOSE:
            block_close(req->handle, rep);
            break;
        case BLOCK_OP_WRITE:
            if (may_write) {
                block_write(req, rep);
            } else {
                rep->error = BLOCK_ERR_READ_ONLY;
            }
            break;
        case BLOCK_OP_FLUSH:
            if (may_write) {
                block_flush(req, rep);
            } else {
                rep->error = BLOCK_ERR_READ_ONLY;
            }
            break;
        case BLOCK_OP_GUARD:
            if (may_write) {
                block_guard(req, rep);
            } else {
                rep->error = BLOCK_ERR_READ_ONLY;
            }
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
static void drain(long endpoint, bool may_write)
{
    struct message msg;
    uint64_t sender = 0;

    while (endpoint >= 0 && kosmos_receive(endpoint, &msg, &sender, 1, 0) == 0) {
        long cap = msg.cap_plus_one > 0 ? (long)msg.cap_plus_one - 1 : -1;

        block_answer(&msg, sender, cap, may_write);
    }
}

/*
 * **Served for ever, disks or none.** `/Devices/disks` is in the list every
 * program is started with, so this answers on a machine with no SATA
 * controller too - no disk at that unit - as the USB driver does with no
 * controller. Both endpoints are waited on at once, the write endpoint's
 * requests answered first, and nothing but a wait the kernel refuses ends
 * it.
 */
static void serve(long reads, long writes)
{
    const long ends[2] = { writes, reads };

    for (;;) {
        long woke = kosmos_irq_wait_any(NULL, 0, 0, ends, 2u);

        if (woke < 0 && woke != SYS_NO_INTERRUPT) {
            kosmos_exit(1);
        }

        drain(writes, true);
        drain(reads, false);
    }
}

void ahci_server(long console_cap, long disks_cap, long writes_cap)
{
    struct dev_info dev;
    struct sysinfo info;
    static struct port ports[PORTS_MOST];
    uint32_t implemented;
    unsigned found = 0;
    long mapped;

    console = console_cap;

    if (kosmos_sysinfo(&info) == 0 && info.counter_hz != 0) {
        counter_hz = (unsigned long)info.counter_hz;
    }

    if (kosmos_dev_find(DEV_AHCI, 0, &dev) != 0) {
        serve(disks_cap, writes_cap);           /* no SATA controller here: no disks, said */
    }

    /*
     * The ABAR need not start a page - Intel's is 2 KB, aligned to 2 KB - so
     * the page holding it is mapped and the offset kept.
     */
    mapped = kosmos_dev_map(dev.base & ~0xFFFul,
                            ((dev.base & 0xFFFul) + dev.size + 4095ul) / 4096ul);

    if (mapped < 0) {
        tell("the controller's registers could not be mapped", 0, NULL);
        serve(disks_cap, writes_cap);
    }

    abar = (uintptr_t)mapped + (dev.base & 0xFFFul);

    take_from_firmware();
    hba_set(HBA_GHC, hba(HBA_GHC) | GHC_AE);
    cap = hba(HBA_CAP);
    implemented = hba(HBA_PI);

    {
        struct say_line line;

        say_begin(&line);
        say_text(&line, "ahci: ");
        say_hex(&line, dev.id >> 16, 4);
        say_text(&line, ":");
        say_hex(&line, dev.id & 0xFFFFu, 4);
        say_text(&line, ", AHCI ");
        say_dec(&line, hba(HBA_VS) >> 16);
        say_text(&line, ".");
        say_dec(&line, (hba(HBA_VS) >> 8) & 0xFFu);
        say_text(&line, ", ports 0x");
        say_hex(&line, implemented, 8);
        say_text(&line, ", ");
        say_dec(&line, CAP_NCS(cap));
        say_text(&line, " command slots, ");
        say_text(&line, (cap & CAP_S64A) ? "64-bit" : "32-bit");
        say_send(console, &line);
    }

    for (unsigned n = 0; n < PORTS_MOST; n++) {
        uint32_t ssts;

        if ((implemented & (1u << n)) == 0) {
            continue;
        }

        ssts = px(n, PX_SSTS);

        if (SSTS_DET(ssts) != DET_PRESENT || SSTS_IPM(ssts) != IPM_ACTIVE) {
            continue;
        }

        ports[n].n = n;

        if (!bring_up(&ports[n])) {
            continue;
        }

        /* Its signature, which the first register FIS it sent set - so
         * read once FIS receive is on. */
        if (px(n, PX_SIG) != AHCI_SIG_DISK) {
            tell("port ", n, " holds something that is not a disk; left alone");
            (void)stop(n);
            continue;
        }

        if (look(&ports[n]) && units_found < UNITS_MOST) {
            units[units_found++] = &ports[n];
        }

        found++;
    }

    tell("", found, found == 1 ? " disk found" : " disks found");
    tell("serving ", units_found, units_found == 1 ? " disk at /Devices/disks"
                                                   : " disks at /Devices/disks");

    /*
     * **Served, never returned**: a role that returns falls through `main.c`
     * into init's own chunk and runs a second init - which, on 13 October,
     * took the screen from the shell and failed thirty suites at once.
     */
    serve(disks_cap, writes_cap);
}
