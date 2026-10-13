/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef USER_INCLUDE_VMMPROTO_H
#define USER_INCLUDE_VMMPROTO_H

/*
 * **What a sandbox's monitor is handed** - `docs/virtualization.md` V3.
 *
 * `vmm` is started with three capabilities - `/Running`, where it registers
 * itself, a region that is the guest's RAM with its kernel (and initramfs)
 * already in it, and a page holding one of these - and a fourth, `/Home`,
 * when it is given folders. Whoever starts it - the
 * `sandbox` program, through `fs.vm_start` - reads the files into the RAM
 * region itself, so a kernel's bytes go from the disk server into a region
 * and are never a Lua string.
 *
 * Offsets are into the RAM region, which the guest sees at `VMM_RAM_BASE`.
 */

#include <stdint.h>

#define VMM_SETUP_MAGIC     0x4b766d6du     /* "Kvmm" */
#define VMM_SETUP_VERSION   1u
#define VMM_CMDLINE_MAX     512u

/* Where things are in every guest's physical address space - QEMU `virt`'s
 * map, which Linux has no opinion about and a reader of a boot log does. */
#define VMM_RAM_BASE        0x40000000ul
#define VMM_GICD_BASE       0x08000000ul
#define VMM_GICR_BASE       0x080a0000ul
#define VMM_UART_BASE       0x09000000ul

#define VMM_NAME_MAX        16u         /* `/Running/sandbox-<name>` fits APP_NAME_MAX */
#define VMM_TOKEN_MAX       24u
#define VMM_SHARES          4u
#define VMM_SHARE_BASE      0x0a000000ul    /* virtio-mmio, 0x200 apart */
#define VMM_SHARE_IRQ       48u             /* SPI 16, as QEMU's first virtio-mmio */

struct vmm_setup {
    uint32_t magic;
    uint32_t version;
    uint64_t ram_bytes;                 /* the region's size, its pages' */
    uint64_t kernel_offset;             /* an arm64 Image, 2 MB aligned */
    uint64_t kernel_bytes;
    uint64_t initrd_offset;             /* 0 bytes: none */
    uint64_t initrd_bytes;
    char     cmdline[VMM_CMDLINE_MAX];
    char     name[VMM_NAME_MAX];        /* the sandbox's: `sandbox-<name>` in /Running */

    /*
     * **How the guest's command says it ended**: a line on its console
     * beginning with this, then its exit code. Made by whoever starts the
     * sandbox and put on the guest's command line as `kosmos.end=`, so the
     * guest's own `/init` knows it and a program inside the guest printing
     * something that looks like it does not end anything - it would have
     * to read `/proc/cmdline` first, and a guest lying about its own exit
     * code is a guest's business. The line is the monitor's, not the
     * console's. Empty: none.
     */
    char     end_token[VMM_TOKEN_MAX];

    /*
     * **Its folders** (`docs/sandboxes.md` S3): each a path in `/Home` the
     * monitor serves to the guest as virtio-fs, tagged `share<i>` - where
     * the guest mounts it is the guest's command line's to say
     * (`kosmos.share<i>=`). An empty `root` is none. The monitor refuses
     * anything outside it, and every change when `read_only`.
     */
    struct vmm_share {
        char     root[256];
        uint32_t read_only;
        uint32_t reserved;
    } shares[VMM_SHARES];
};

_Static_assert(sizeof(struct vmm_setup) == 600 + VMM_SHARES * 264,
               "six words, a magic, a version, a line, a name, a token and its folders");
_Static_assert(sizeof(struct vmm_setup) <= 4096, "a setup is a page");

/*
 * **What a running sandbox answers** (`docs/sandboxes.md` S1). Each `vmm`
 * registers its own endpoint in `/Running` as `sandbox-<name>`, and the
 * window and `sandbox` ask it:
 *
 * - `VMM_OP_STATE` - running or ended, its exit code, its RAM, how long it
 *   has run and how much of that was the guest's, and how much its console
 *   has said;
 * - `VMM_OP_READ` - its console from `offset`: up to `VMM_TEXT_MAX` bytes,
 *   and where they began - later than asked, when the ring has moved past
 *   it, so a reader that fell behind learns it and carries on;
 * - `VMM_OP_KEYS` - bytes typed, to its serial port's receiver;
 * - `VMM_OP_STOP` - end it now, or, once it has ended, let its monitor go.
 *
 * The console is text a person reads, asked for by offset - a message,
 * because it recurs when a program prints rather than when a clock comes
 * round; the guest's RAM, which does stream, is a region.
 */
#define VMM_OP_STATE        1u
#define VMM_OP_READ         2u
#define VMM_OP_KEYS         3u
#define VMM_OP_STOP         4u

#define VMM_OK              0u
#define VMM_ERR_BAD_OP      1u

#define VMM_RUNNING         1u
#define VMM_ENDED           2u

#define VMM_TEXT_MAX        1536u

struct vmm_request {
    uint32_t op;
    uint32_t length;                    /* bytes of `text`, for keys */
    uint64_t offset;                    /* where to read from */
    char     text[VMM_TEXT_MAX];
};

struct vmm_reply {
    uint32_t error;
    uint32_t state;                     /* VMM_RUNNING, VMM_ENDED */
    int32_t  exit_code;                 /* the command's, once known; -1 */
    uint32_t length;                    /* bytes of `text` */
    uint64_t offset;                    /* where `text` begins */
    uint64_t written;                   /* all its console has ever said */
    uint64_t ram_bytes;
    uint64_t up_counter_ticks;          /* since it started */
    uint64_t guest_counter_ticks;       /* of those, in the guest */
    uint64_t exits;
    char     name[VMM_NAME_MAX];
    char     text[VMM_TEXT_MAX];
};

_Static_assert(sizeof(struct vmm_reply) <= 2048, "a reply is one message");
_Static_assert(sizeof(struct vmm_request) <= 2048, "a request is one message");

#endif /* USER_INCLUDE_VMMPROTO_H */
