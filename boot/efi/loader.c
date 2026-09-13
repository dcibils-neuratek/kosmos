/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * Kosmos's own UEFI loader: `\EFI\BOOT\BOOTX64.EFI` on the stick, where GRUB
 * used to be.
 *
 * **Why it exists.** On the ThinkPad T14 an image GRUB loaded arrived with
 * bytes already changed, before Kosmos ran an instruction - eleven pages of
 * the userland image on 11 September, and a machine dead before its first
 * screen on the 12th and the 13th - and only in some layouts: a disk a few
 * megabytes bigger, or a kernel a few kilobytes bigger, decided it. GRUB
 * reported nothing. And on its first run this loader found that under OVMF
 * the kernel's old place, 1 MB to 12 MB, holds ACPI NVS and the firmware's
 * boot-time data, which GRUB had been loading the kernel over. `thinkpad.md`
 * §6a has the whole account.
 *
 * **What this does instead, and why each step is here:**
 *
 *   - **The kernel's range is checked against the firmware's map** before
 *     anything large is allocated. Free pages in it are claimed by address,
 *     so nothing the firmware does later can be given them. Pages the
 *     firmware is only borrowing until ExitBootServices are left to it and
 *     filled after. Anything else - memory the firmware keeps, or memory
 *     already holding a loaded image - is refused on the screen, with the
 *     firmware's own entries, instead of being loaded over.
 *   - **The kernel is read into two copies, with a checksum per page**, and
 *     every buffer this loader allocates is checked to lie outside the
 *     kernel's range.
 *   - **Both copies are checked, and repaired from each other, before
 *     ExitBootServices and again after it.** Then the kernel is copied into
 *     place and every page of it is checked once more. What was repaired, and
 *     what could not be, reaches the kernel on its command line - so the
 *     fault that used to be silent is survived and reported.
 *   - The information structure carries the seven tags GRUB's did, built by
 *     `mbi.c`, so the kernel does not change; the jump is `trampoline.S`.
 *
 * **Every structure layout is the UEFI specification's**, checked against
 * gnu-efi's transcription of it (`efiapi.h`, `efiprot.h`, `eficon.h`,
 * `efidef.h`, `efierr.h`), with the offsets that matter asserted below.
 * Nothing is copied from gnu-efi.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mbi.h"

#define EFIAPI __attribute__((ms_abi))

typedef uint64_t efi_status;
typedef uint16_t char16;
typedef void *efi_handle;

/* Status codes: an error has the top bit set (efibind.h, efierr.h). */
#define EFI_SUCCESS             0ull
#define EFI_ERROR               0x8000000000000000ull
#define EFI_LOAD_ERROR          (EFI_ERROR | 1u)
#define EFI_BUFFER_TOO_SMALL    (EFI_ERROR | 5u)
#define EFI_NOT_READY           (EFI_ERROR | 6u)

/* EFI_ALLOCATE_TYPE, and EFI_MEMORY_TYPE values (efidef.h). */
#define ALLOCATE_MAX_ADDRESS    1u
#define ALLOCATE_ADDRESS        2u
#define LOADER_CODE             1u
#define LOADER_DATA             2u
#define BOOT_SERVICES_CODE      3u
#define BOOT_SERVICES_DATA      4u
#define CONVENTIONAL            7u

#define FILE_MODE_READ          1ull
#define PAGE                    4096u
#define BELOW_4G                0xFFFFFFFFull

/*------------------------------------------------------------------------
 * The firmware's tables, as far as this loader reads them.
 *----------------------------------------------------------------------*/

struct efi_guid {
    uint32_t a;
    uint16_t b;
    uint16_t c;
    uint8_t  d[8];
};

struct efi_table_header {
    uint64_t signature;
    uint32_t revision;
    uint32_t header_size;
    uint32_t crc32;
    uint32_t reserved;
};

struct efi_text_out {
    efi_status (EFIAPI *reset)(struct efi_text_out *self, uint8_t verify);
    efi_status (EFIAPI *output_string)(struct efi_text_out *self,
                                       const char16 *text);
};

struct efi_input_key {
    uint16_t scan_code;
    char16   unicode;
};

struct efi_text_in {
    efi_status (EFIAPI *reset)(struct efi_text_in *self, uint8_t verify);
    efi_status (EFIAPI *read_key)(struct efi_text_in *self,
                                  struct efi_input_key *key);
};

struct efi_configuration_table {
    struct efi_guid guid;
    void *table;
};

struct efi_boot_services;

struct efi_system_table {
    struct efi_table_header hdr;
    char16 *firmware_vendor;
    uint32_t firmware_revision;
    efi_handle con_in_handle;
    struct efi_text_in *con_in;
    efi_handle con_out_handle;
    struct efi_text_out *con_out;
    efi_handle std_err_handle;
    struct efi_text_out *std_err;
    void *runtime_services;
    struct efi_boot_services *boot;
    uint64_t table_entries;
    struct efi_configuration_table *tables;
};

/* The same two offsets `hal/pc/smbios.c` reads the table at. */
_Static_assert(offsetof(struct efi_system_table, table_entries) == 104,
               "EFI_SYSTEM_TABLE.NumberOfTableEntries");
_Static_assert(offsetof(struct efi_system_table, tables) == 112,
               "EFI_SYSTEM_TABLE.ConfigurationTable");

struct efi_boot_services {
    struct efi_table_header hdr;
    void *raise_tpl;
    void *restore_tpl;
    efi_status (EFIAPI *allocate_pages)(uint32_t how, uint32_t type,
                                        uint64_t pages, uint64_t *address);
    efi_status (EFIAPI *free_pages)(uint64_t address, uint64_t pages);
    efi_status (EFIAPI *get_memory_map)(uint64_t *map_bytes, uint8_t *map,
                                        uint64_t *key,
                                        uint64_t *descriptor_bytes,
                                        uint32_t *descriptor_version);
    efi_status (EFIAPI *allocate_pool)(uint32_t type, uint64_t bytes,
                                       void **buffer);
    efi_status (EFIAPI *free_pool)(void *buffer);
    void *create_event;
    void *set_timer;
    void *wait_for_event;
    void *signal_event;
    void *close_event;
    void *check_event;
    void *install_protocol_interface;
    void *reinstall_protocol_interface;
    void *uninstall_protocol_interface;
    efi_status (EFIAPI *handle_protocol)(efi_handle handle,
                                         const struct efi_guid *protocol,
                                         void **interface);
    void *reserved;
    void *register_protocol_notify;
    void *locate_handle;
    void *locate_device_path;
    void *install_configuration_table;
    void *load_image;
    void *start_image;
    void *exit;
    void *unload_image;
    efi_status (EFIAPI *exit_boot_services)(efi_handle image,
                                            uint64_t map_key);
    void *get_next_monotonic_count;
    efi_status (EFIAPI *stall)(uint64_t microseconds);
    efi_status (EFIAPI *set_watchdog_timer)(uint64_t timeout, uint64_t code,
                                            uint64_t data_bytes,
                                            char16 *data);
    void *connect_controller;
    void *disconnect_controller;
    void *open_protocol;
    void *close_protocol;
    void *open_protocol_information;
    void *protocols_per_handle;
    void *locate_handle_buffer;
    efi_status (EFIAPI *locate_protocol)(const struct efi_guid *protocol,
                                         void *registration,
                                         void **interface);
};

_Static_assert(offsetof(struct efi_boot_services, allocate_pages) == 40,
               "EFI_BOOT_SERVICES.AllocatePages");
_Static_assert(offsetof(struct efi_boot_services, handle_protocol) == 152,
               "EFI_BOOT_SERVICES.HandleProtocol");
_Static_assert(offsetof(struct efi_boot_services, exit_boot_services) == 232,
               "EFI_BOOT_SERVICES.ExitBootServices");
_Static_assert(offsetof(struct efi_boot_services, locate_protocol) == 320,
               "EFI_BOOT_SERVICES.LocateProtocol");

struct efi_loaded_image {
    uint32_t revision;
    efi_handle parent;
    struct efi_system_table *system;
    efi_handle device;          /* the volume this file was read from */
};

_Static_assert(offsetof(struct efi_loaded_image, device) == 24,
               "EFI_LOADED_IMAGE_PROTOCOL.DeviceHandle");

struct efi_file {
    uint64_t revision;
    efi_status (EFIAPI *open)(struct efi_file *self, struct efi_file **out,
                              const char16 *name, uint64_t mode,
                              uint64_t attributes);
    efi_status (EFIAPI *close)(struct efi_file *self);
    void *delete_file;
    efi_status (EFIAPI *read)(struct efi_file *self, uint64_t *bytes,
                              void *buffer);
    void *write;
    void *get_position;
    void *set_position;
    efi_status (EFIAPI *get_info)(struct efi_file *self,
                                  const struct efi_guid *type,
                                  uint64_t *bytes, void *buffer);
};

_Static_assert(offsetof(struct efi_file, get_info) == 64,
               "EFI_FILE_PROTOCOL.GetInfo");

struct efi_file_system {
    uint64_t revision;
    efi_status (EFIAPI *open_volume)(struct efi_file_system *self,
                                     struct efi_file **root);
};

/* EFI_FILE_INFO: Size, then FileSize, then the rest (efiprot.h). */
#define FILE_INFO_FILE_SIZE     8u

struct efi_gop_mode_info {
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t pixel_format;
    uint32_t red_mask;
    uint32_t green_mask;
    uint32_t blue_mask;
    uint32_t reserved_mask;
    uint32_t pixels_per_scan_line;
};

struct efi_gop_mode {
    uint32_t max_mode;
    uint32_t mode;
    struct efi_gop_mode_info *info;
    uint64_t size_of_info;
    uint64_t frame_buffer_base;
    uint64_t frame_buffer_size;
};

struct efi_gop {
    void *query_mode;
    void *set_mode;
    void *blt;
    struct efi_gop_mode *mode;
};

_Static_assert(offsetof(struct efi_gop_mode, frame_buffer_base) == 24,
               "EFI_GRAPHICS_OUTPUT_PROTOCOL_MODE.FrameBufferBase");
_Static_assert(offsetof(struct efi_gop_mode_info, pixels_per_scan_line) == 32,
               "EFI_GRAPHICS_OUTPUT_MODE_INFORMATION.PixelsPerScanLine");

/* EFI_GRAPHICS_PIXEL_FORMAT */
#define PIXEL_RGBX      0u
#define PIXEL_BGRX      1u
#define PIXEL_BITMASK   2u

static const struct efi_guid LOADED_IMAGE_GUID =
    { 0x5B1B31A1, 0x9562, 0x11d2, { 0x8E, 0x3F, 0x00, 0xA0, 0xC9, 0x69, 0x72, 0x3B } };
static const struct efi_guid FILE_SYSTEM_GUID =
    { 0x964e5b22, 0x6459, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const struct efi_guid FILE_INFO_GUID =
    { 0x09576e92, 0x6d3f, 0x11d2, { 0x8e, 0x39, 0x00, 0xa0, 0xc9, 0x69, 0x72, 0x3b } };
static const struct efi_guid GOP_GUID =
    { 0x9042a9de, 0x23dc, 0x4a38, { 0x96, 0xfb, 0x7a, 0xde, 0xd0, 0x80, 0x51, 0x6a } };
static const struct efi_guid ACPI_20_GUID =
    { 0x8868e871, 0xe4f1, 0x11d3, { 0xbc, 0x22, 0x00, 0x80, 0xc7, 0x3c, 0x88, 0x81 } };
static const struct efi_guid ACPI_10_GUID =
    { 0xeb9d2d30, 0x2d88, 0x11d3, { 0x9a, 0x16, 0x00, 0x90, 0x27, 0x3f, 0xc1, 0x4d } };

/* The trampoline's bytes, from `trampoline.S`. Hidden, so they are reached
 * PC-relative rather than through a GOT entry the image would have to fix up. */
extern const uint8_t efi_trampoline_start[] __attribute__((visibility("hidden")));
extern const uint8_t efi_trampoline_end[] __attribute__((visibility("hidden")));

/*------------------------------------------------------------------------
 * The three byte functions a compiler may call on its own, even here.
 *----------------------------------------------------------------------*/

void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n-- > 0) {
        *d++ = *s++;
    }

    return dst;
}

void *memset(void *dst, int value, size_t n)
{
    uint8_t *d = dst;

    while (n-- > 0) {
        *d++ = (uint8_t)value;
    }

    return dst;
}

int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = a;
    const uint8_t *y = b;

    for (; n > 0; n--, x++, y++) {
        if (*x != *y) {
            return *x < *y ? -1 : 1;
        }
    }

    return 0;
}

static bool same_guid(const struct efi_guid *a, const struct efi_guid *b)
{
    return memcmp(a, b, sizeof(*a)) == 0;
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8)
           | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t get64(const uint8_t *p)
{
    return (uint64_t)get32(p) | ((uint64_t)get32(p + 4) << 32);
}

/* FNV-1a: a changed byte changes the answer, and it needs no tables. */
static uint64_t fingerprint(const uint8_t *bytes, uint64_t size)
{
    uint64_t h = 0xcbf29ce484222325ull;
    uint64_t i;

    for (i = 0; i < size; i++) {
        h = (h ^ bytes[i]) * 0x100000001b3ull;
    }

    return h;
}

/*------------------------------------------------------------------------
 * Saying what it is doing, while there is a console to say it on.
 *----------------------------------------------------------------------*/

static struct efi_system_table *sys;
static struct efi_boot_services *boot;

/* False from the first ExitBootServices call: after it nothing may print. */
static bool console_up = true;

#define LINE_MAX 220u

struct line {
    char16   text[LINE_MAX + 3];
    unsigned n;
};

static void add_text(struct line *l, const char *s)
{
    while (*s != '\0' && l->n < LINE_MAX) {
        l->text[l->n++] = (char16)(uint8_t)*s++;
    }
}

static void add_text16(struct line *l, const char16 *s)
{
    while (s != NULL && *s != 0 && l->n < LINE_MAX) {
        l->text[l->n++] = *s++;
    }
}

static void add_hex(struct line *l, uint64_t v, unsigned digits)
{
    add_text(l, "0x");

    while (digits-- > 0 && l->n < LINE_MAX) {
        l->text[l->n++] = (char16)"0123456789abcdef"[(v >> (digits * 4)) & 0xF];
    }
}

static void add_dec(struct line *l, uint64_t v)
{
    char digits[21];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);

    while (n > 0 && l->n < LINE_MAX) {
        l->text[l->n++] = (char16)digits[--n];
    }
}

static void begin(struct line *l)
{
    l->n = 0;
    add_text(l, "kosmos-boot: ");
}

static void send(struct line *l)
{
    l->text[l->n] = '\r';
    l->text[l->n + 1] = '\n';
    l->text[l->n + 2] = 0;

    if (console_up && sys->con_out != NULL) {
        sys->con_out->output_string(sys->con_out, l->text);
    }
}

static void say(const char *text)
{
    struct line l;

    begin(&l);
    add_text(&l, text);
    send(&l);
}

/*
 * **A refusal waits for a key.** It is the last thing this machine will
 * show before the firmware takes it back, and a laptop has nothing else to
 * read it on.
 */
static efi_status refuse(const char *why, efi_status status)
{
    struct efi_input_key key;
    struct line l;

    begin(&l);
    add_text(&l, why);

    if (status != EFI_SUCCESS) {
        add_text(&l, " (status ");
        add_hex(&l, status, 16);
        add_text(&l, ")");
    }

    send(&l);
    say("Kosmos cannot start from this stick. Press a key to return to the "
        "firmware.");

    if (sys->con_in != NULL) {
        sys->con_in->reset(sys->con_in, 0);

        while (sys->con_in->read_key(sys->con_in, &key) == EFI_NOT_READY) {
            boot->stall(50000);
        }
    }

    return EFI_LOAD_ERROR;
}

/*------------------------------------------------------------------------
 * The kernel's range, against the firmware's map.
 *----------------------------------------------------------------------*/

/*
 * A map read into the loader's own memory, not a pool: this runs before the
 * kernel's range is claimed, and a pool allocated now could be a piece of it.
 */
static uint8_t claim_map[64u * 1024];

static const char *type_name(uint32_t type)
{
    switch (type) {
    case 0:  return "reserved";
    case 1:  return "a loaded image's code";
    case 2:  return "a loaded image's data";
    case 3:  return "boot services code";
    case 4:  return "boot services data";
    case 5:  return "runtime services code";
    case 6:  return "runtime services data";
    case 7:  return "free";
    case 8:  return "unusable";
    case 9:  return "ACPI tables";
    case 10: return "ACPI NVS";
    case 11: return "a device window";
    default: return "another kind";
    }
}

/*
 * **Claim what is free, leave what is borrowed, refuse what is kept.**
 *
 * Free pages are allocated by address, so nothing the firmware allocates
 * later lands in them. Boot services memory is the firmware's until
 * ExitBootServices and nobody's after, so the kernel is copied over it then.
 * Everything else - ACPI NVS, runtime services, reserved memory, device
 * windows, and a loaded image's pages, which may be this loader's own - is
 * refused, with each entry printed, because loading over it is what the
 * old loader did.
 */
static bool claim_range(uint64_t lo, uint64_t hi, uint64_t *borrowed,
                        efi_status *status)
{
    uint64_t bytes = sizeof(claim_map), key, descriptor = 0, at;
    uint32_t version;
    bool ok = true;

    *borrowed = 0;
    *status = boot->get_memory_map(&bytes, claim_map, &key, &descriptor,
                                   &version);

    if (*status != EFI_SUCCESS || descriptor < EFI_DESCRIPTOR_MIN) {
        say("the firmware will not describe its memory");
        return false;
    }

    for (at = 0; at + descriptor <= bytes; at += descriptor) {
        uint32_t type = get32(claim_map + at);
        uint64_t base = get64(claim_map + at + 8);
        uint64_t end = base + get64(claim_map + at + 24) * PAGE;

        if (end <= lo || base >= hi || type == CONVENTIONAL
            || type == BOOT_SERVICES_CODE || type == BOOT_SERVICES_DATA) {
            continue;
        }

        if (ok) {
            struct line l;

            begin(&l);
            add_text(&l, "the kernel must be at ");
            add_hex(&l, lo, 8);
            add_text(&l, "..");
            add_hex(&l, hi, 8);
            add_text(&l, ", and the firmware keeps part of it:");
            send(&l);
            ok = false;
        }

        {
            struct line l;

            begin(&l);
            add_text(&l, "  ");
            add_hex(&l, base, 8);
            add_text(&l, "..");
            add_hex(&l, end, 8);
            add_text(&l, "  ");
            add_text(&l, type_name(type));
            send(&l);
        }
    }

    if (!ok) {
        return false;
    }

    for (at = 0; at + descriptor <= bytes; at += descriptor) {
        uint32_t type = get32(claim_map + at);
        uint64_t base = get64(claim_map + at + 8);
        uint64_t end = base + get64(claim_map + at + 24) * PAGE;
        uint64_t from, to;

        if (end <= lo || base >= hi) {
            continue;
        }

        from = base > lo ? base : lo;
        to = end < hi ? end : hi;

        if (type != CONVENTIONAL) {
            *borrowed += to - from;
            continue;
        }

        *status = boot->allocate_pages(ALLOCATE_ADDRESS, LOADER_CODE,
                                       (to - from) / PAGE, &from);

        if (*status != EFI_SUCCESS) {
            say("the firmware would not let the kernel's free pages be "
                "claimed");
            return false;
        }
    }

    return true;
}

/*------------------------------------------------------------------------
 * Files.
 *----------------------------------------------------------------------*/

static efi_status open_file(struct efi_file *root, const char16 *name,
                            struct efi_file **file, uint64_t *size)
{
    uint8_t info[512];
    uint64_t bytes = sizeof(info);
    efi_status status;

    status = root->open(root, file, name, FILE_MODE_READ, 0);

    if (status != EFI_SUCCESS) {
        return status;
    }

    status = (*file)->get_info(*file, &FILE_INFO_GUID, &bytes, info);

    if (status != EFI_SUCCESS) {
        (*file)->close(*file);
        return status;
    }

    *size = get64(info + FILE_INFO_FILE_SIZE);
    return EFI_SUCCESS;
}

/* In pieces, because some firmware reads a large file badly in one call. */
static efi_status read_all(struct efi_file *file, uint8_t *into, uint64_t size)
{
    uint64_t done = 0;

    while (done < size) {
        uint64_t n = size - done;
        efi_status status;

        if (n > 16u * 1024 * 1024) {
            n = 16u * 1024 * 1024;
        }

        status = file->read(file, &n, into + done);

        if (status != EFI_SUCCESS) {
            return status;
        }

        if (n == 0) {
            return EFI_LOAD_ERROR;      /* the file ended before its size */
        }

        done += n;
    }

    return EFI_SUCCESS;
}

/*------------------------------------------------------------------------
 * The kernel: two copies, a checksum a page, and the place it goes.
 *----------------------------------------------------------------------*/

struct kernel {
    struct mb2_image img;
    uint64_t lo, hi;            /* its range, in whole pages */
    const uint8_t *a;           /* the file's bytes to load, twice */
    uint8_t *copy_a, *copy_b;
    uint64_t *prints;           /* one fingerprint per page of them */
    uint64_t pages;
};

static bool outside(const struct kernel *k, uint64_t at, uint64_t bytes)
{
    return at + bytes <= k->lo || at >= k->hi;
}

static uint64_t page_bytes(const struct kernel *k, uint64_t page)
{
    uint64_t left = k->img.load_bytes - page * PAGE;

    return left < PAGE ? left : PAGE;
}

/*
 * Both copies against the checksums taken when the file was read, each
 * page repaired from the other where only one is wrong. Answers the pages
 * repaired; `lost` counts those wrong in both.
 */
static uint64_t mend_copies(struct kernel *k, uint64_t *lost)
{
    uint64_t page, mended = 0;

    for (page = 0; page < k->pages; page++) {
        uint8_t *a = k->copy_a + page * PAGE;
        uint8_t *b = k->copy_b + page * PAGE;
        uint64_t n = page_bytes(k, page);
        bool good_a = fingerprint(a, n) == k->prints[page];
        bool good_b = fingerprint(b, n) == k->prints[page];

        if (good_a && !good_b) {
            memcpy(b, a, n);
            mended++;
        } else if (!good_a && good_b) {
            memcpy(a, b, n);
            mended++;
        } else if (!good_a && !good_b) {
            (*lost)++;
        }
    }

    return mended;
}

/*
 * The kernel in its place, checked page by page and repaired from a copy.
 * Answers the pages repaired; `lost` counts those that stayed wrong.
 */
static uint64_t mend_placed(struct kernel *k, uint64_t *lost)
{
    uint64_t page, mended = 0;

    for (page = 0; page < k->pages; page++) {
        uint8_t *placed = (uint8_t *)(uintptr_t)(k->img.load_addr + page * PAGE);
        uint64_t n = page_bytes(k, page);

        if (fingerprint(placed, n) == k->prints[page]) {
            continue;
        }

        memcpy(placed, k->copy_a + page * PAGE, n);
        mended++;

        if (fingerprint(placed, n) != k->prints[page]) {
            (*lost)++;
        }
    }

    return mended;
}

/* Five digits into a fixed field, so it can be written after the structure
 * holding it is finished. */
static void digits5(char *at, uint64_t v)
{
    int i;

    if (v > 99999) {
        v = 99999;
    }

    for (i = 4; i >= 0; i--) {
        at[i] = (char)('0' + v % 10);
        v /= 10;
    }
}

/*------------------------------------------------------------------------
 * The command line.
 *----------------------------------------------------------------------*/

#define CMDLINE_MAX 384u

/*
 * `\boot\kosmos.cmdline`, when there is one: the words GRUB's `multiboot2`
 * line used to carry. Only the characters `mkusb_image.py` allows through;
 * anything else ends the line.
 */
static unsigned read_cmdline(struct efi_file *root, char *out, unsigned max)
{
    struct efi_file *file;
    uint64_t size;
    uint8_t text[200];
    unsigned n = 0, i;

    if (open_file(root, u"\\boot\\kosmos.cmdline", &file, &size) != EFI_SUCCESS) {
        return 0;
    }

    if (size > sizeof(text)) {
        size = sizeof(text);
    }

    if (read_all(file, text, size) == EFI_SUCCESS) {
        for (i = 0; i < size && n + 1 < max; i++) {
            char c = (char)text[i];

            if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
                  || (c >= '0' && c <= '9') || c == '_' || c == '.'
                  || c == '/' || c == '=' || c == ',' || c == ':'
                  || c == '-' || c == ' ')) {
                break;
            }

            out[n++] = c;
        }
    }

    file->close(file);

    while (n > 0 && out[n - 1] == ' ') {
        n--;
    }

    return n;
}

static void append(char *line, unsigned *n, const char *text)
{
    while (*text != '\0' && *n + 1 < CMDLINE_MAX) {
        line[(*n)++] = *text++;
    }

    line[*n] = '\0';
}

/*------------------------------------------------------------------------
 * The screen.
 *----------------------------------------------------------------------*/

static void mask_shape(uint32_t mask, uint8_t *at, uint8_t *bits)
{
    uint8_t a = 0, b = 0;

    while (mask != 0 && (mask & 1u) == 0) {
        mask >>= 1;
        a++;
    }

    while ((mask & 1u) != 0) {
        mask >>= 1;
        b++;
    }

    *at = a;
    *bits = b;
}

struct screen {
    bool     valid;
    uint64_t addr;
    uint32_t pitch, width, height;
    uint8_t  red_at, red_bits, green_at, green_bits, blue_at, blue_bits;
};

/* The firmware's current mode, as it is: that is the mode GRUB passed on. */
static void find_screen(struct screen *s)
{
    struct efi_gop *gop;
    struct efi_gop_mode_info *info;
    struct line l;

    s->valid = false;

    if (boot->locate_protocol(&GOP_GUID, NULL, (void **)&gop) != EFI_SUCCESS
        || gop == NULL || gop->mode == NULL || gop->mode->info == NULL) {
        say("the firmware has no graphics output; Kosmos starts without a "
            "screen");
        return;
    }

    info = gop->mode->info;

    switch (info->pixel_format) {
    case PIXEL_RGBX:
        s->red_at = 0, s->green_at = 8, s->blue_at = 16;
        s->red_bits = s->green_bits = s->blue_bits = 8;
        break;

    case PIXEL_BGRX:
        s->blue_at = 0, s->green_at = 8, s->red_at = 16;
        s->red_bits = s->green_bits = s->blue_bits = 8;
        break;

    case PIXEL_BITMASK:
        mask_shape(info->red_mask, &s->red_at, &s->red_bits);
        mask_shape(info->green_mask, &s->green_at, &s->green_bits);
        mask_shape(info->blue_mask, &s->blue_at, &s->blue_bits);
        break;

    default:
        say("the firmware's screen cannot be drawn into directly; Kosmos "
            "starts without one");
        return;
    }

    s->addr = gop->mode->frame_buffer_base;
    s->width = info->width;
    s->height = info->height;
    s->pitch = info->pixels_per_scan_line * 4u;
    s->valid = s->addr != 0 && s->width != 0 && s->height != 0;

    begin(&l);
    add_text(&l, "the screen: ");
    add_dec(&l, s->width);
    add_text(&l, "x");
    add_dec(&l, s->height);
    add_text(&l, ", ");
    add_dec(&l, s->pitch);
    add_text(&l, " bytes a row, at ");
    add_hex(&l, s->addr, 16);
    add_text(&l, ", mode ");
    add_dec(&l, gop->mode->mode);
    add_text(&l, " of ");
    add_dec(&l, gop->mode->max_mode);
    send(&l);
}

/*------------------------------------------------------------------------
 * The loader.
 *----------------------------------------------------------------------*/

/* Enough of the file to find the header in: it is inside the first 32 KB. */
static uint8_t head[36u * 1024];

EFIAPI
efi_status efi_main(efi_handle image_handle, struct efi_system_table *table)
{
    struct efi_loaded_image *image;
    struct efi_file_system *volume;
    struct efi_file *root, *file;
    struct kernel k;
    struct screen screen;
    struct line l;
    const char *why;
    efi_status status;
    uint64_t kernel_size, head_bytes, borrowed, disk_size = 0;
    uint64_t disk_at = BELOW_4G, disk_print = 0, tramp_at = BELOW_4G;
    uint64_t mbi_at = BELOW_4G, map_bytes = 0, map_key, descriptor = 0;
    uint64_t map_cap, tramp_bytes, i, page;
    uint64_t before = 0, after = 0, lost = 0;
    uint32_t descriptor_version, mbi_bytes, mbi_pages;
    uint8_t *map = NULL, *rsdp = NULL;
    struct mb2_range *ranges = NULL;
    unsigned ranges_cap, count = 0, cmdline_n = 0, attempt;
    bool rsdp_v2 = false, exited = false;
    char cmdline[CMDLINE_MAX];
    char *before_digits, *after_digits, *lost_digits, *disk_word;
    void *pool;
    struct mbi m;

    sys = table;
    boot = table->boot;

    /* The five-minute watchdog would reset a machine waiting at a refusal. */
    boot->set_watchdog_timer(0, 0, 0, NULL);

    begin(&l);
    add_text(&l, "Kosmos's own loader, on ");
    add_text16(&l, table->firmware_vendor);
    add_text(&l, " firmware revision ");
    add_hex(&l, table->firmware_revision, 8);
    send(&l);

    /* The volume this file came from, which is where the kernel is. */
    status = boot->handle_protocol(image_handle, &LOADED_IMAGE_GUID,
                                   (void **)&image);

    if (status != EFI_SUCCESS) {
        return refuse("the firmware will not say where this loader came from",
                      status);
    }

    status = boot->handle_protocol(image->device, &FILE_SYSTEM_GUID,
                                   (void **)&volume);

    if (status == EFI_SUCCESS) {
        status = volume->open_volume(volume, &root);
    }

    if (status != EFI_SUCCESS) {
        return refuse("the stick's partition cannot be read as a filesystem",
                      status);
    }

    /* The header first, out of a buffer of this loader's own. */
    status = open_file(root, u"\\boot\\kosmos.bin", &file, &kernel_size);

    if (status != EFI_SUCCESS) {
        return refuse("there is no \\boot\\kosmos.bin on this stick", status);
    }

    head_bytes = kernel_size < sizeof(head) ? kernel_size : sizeof(head);
    status = read_all(file, head, head_bytes);
    file->close(file);

    if (status != EFI_SUCCESS) {
        return refuse("the kernel could not be read off the stick", status);
    }

    why = mb2_image_parse(head, head_bytes, kernel_size, &k.img);

    if (why != NULL) {
        return refuse(why, EFI_SUCCESS);
    }

    k.lo = k.img.load_addr & ~(uint64_t)(PAGE - 1);
    k.hi = ((uint64_t)k.img.bss_end + PAGE - 1) & ~(uint64_t)(PAGE - 1);

    /* Its range, before anything big is allocated. */
    if (!claim_range(k.lo, k.hi, &borrowed, &status)) {
        return refuse("the kernel's memory is not the loader's to give",
                      status);
    }

    begin(&l);
    add_text(&l, "the kernel's place: ");
    add_hex(&l, k.lo, 8);
    add_text(&l, "..");
    add_hex(&l, k.hi, 8);
    add_text(&l, ", ");
    add_dec(&l, (k.hi - k.lo - borrowed) / 1024);
    add_text(&l, " KB claimed now and ");
    add_dec(&l, borrowed / 1024);
    add_text(&l, " KB the firmware's until it lets go");
    send(&l);

    /* The kernel, into two copies with a fingerprint a page. */
    status = open_file(root, u"\\boot\\kosmos.bin", &file, &kernel_size);

    if (status == EFI_SUCCESS) {
        status = boot->allocate_pool(LOADER_DATA, kernel_size, &pool);
    }

    if (status == EFI_SUCCESS) {
        k.a = pool;
        status = read_all(file, pool, kernel_size);
        file->close(file);
    }

    if (status != EFI_SUCCESS) {
        return refuse("the kernel could not be read off the stick", status);
    }

    k.pages = ((uint64_t)k.img.load_bytes + PAGE - 1) / PAGE;

    if (boot->allocate_pool(LOADER_DATA, k.pages * PAGE, &pool) != EFI_SUCCESS) {
        return refuse("no memory for a copy of the kernel", EFI_SUCCESS);
    }

    k.copy_a = pool;

    if (boot->allocate_pool(LOADER_DATA, k.pages * PAGE, &pool) != EFI_SUCCESS) {
        return refuse("no memory for a second copy of the kernel", EFI_SUCCESS);
    }

    k.copy_b = pool;

    if (boot->allocate_pool(LOADER_DATA, k.pages * sizeof(uint64_t), &pool)
        != EFI_SUCCESS) {
        return refuse("no memory for the kernel's fingerprints", EFI_SUCCESS);
    }

    k.prints = pool;
    memset(k.copy_a, 0, k.pages * PAGE);
    memcpy(k.copy_a, k.a + k.img.load_offset, k.img.load_bytes);
    memcpy(k.copy_b, k.copy_a, k.pages * PAGE);

    for (page = 0; page < k.pages; page++) {
        k.prints[page] = fingerprint(k.copy_a + page * PAGE, page_bytes(&k, page));
    }

    if (!outside(&k, (uint64_t)(uintptr_t)k.a, kernel_size)
        || !outside(&k, (uint64_t)(uintptr_t)k.copy_a, k.pages * PAGE)
        || !outside(&k, (uint64_t)(uintptr_t)k.copy_b, k.pages * PAGE)
        || !outside(&k, (uint64_t)(uintptr_t)k.prints, k.pages * 8)) {
        return refuse("the firmware gave the loader memory inside the kernel's "
                      "range", EFI_SUCCESS);
    }

    begin(&l);
    add_text(&l, "the kernel: ");
    add_dec(&l, k.img.load_bytes / 1024);
    add_text(&l, " KB in two copies, ");
    add_dec(&l, k.pages);
    add_text(&l, " pages fingerprinted, entry ");
    add_hex(&l, k.img.entry, 8);
    send(&l);

    /* The disk, below 4 GB because a module's addresses are 32 bits. */
    if (open_file(root, u"\\boot\\disk.img", &file, &disk_size) == EFI_SUCCESS
        && disk_size > 0) {
        status = boot->allocate_pages(ALLOCATE_MAX_ADDRESS, LOADER_DATA,
                                      (disk_size + PAGE - 1) / PAGE, &disk_at);

        if (status == EFI_SUCCESS) {
            status = read_all(file, (uint8_t *)(uintptr_t)disk_at, disk_size);
        }

        file->close(file);

        if (status != EFI_SUCCESS) {
            return refuse("the disk image could not be read off the stick",
                          status);
        }

        if (!outside(&k, disk_at, disk_size)) {
            return refuse("the firmware put the disk inside the kernel's range",
                          EFI_SUCCESS);
        }

        disk_print = fingerprint((uint8_t *)(uintptr_t)disk_at, disk_size);

        begin(&l);
        add_text(&l, "the disk: ");
        add_hex(&l, disk_at, 8);
        add_text(&l, "..");
        add_hex(&l, disk_at + disk_size, 8);
        add_text(&l, ", ");
        add_dec(&l, disk_size / 1024);
        add_text(&l, " KB");
        send(&l);
    } else {
        disk_size = 0;
        say("no \\boot\\disk.img on this stick; Kosmos keeps /home in memory");
    }

    /* The command line, and four fields filled in as the facts arrive. */
    cmdline_n = read_cmdline(root, cmdline, CMDLINE_MAX);
    cmdline[cmdline_n] = '\0';

    if (cmdline_n > 0) {
        append(cmdline, &cmdline_n, " ");
    }

    append(cmdline, &cmdline_n, "kosmos-boot/before=");
    before_digits = cmdline + cmdline_n;
    append(cmdline, &cmdline_n, "00000 kosmos-boot/after=");
    after_digits = cmdline + cmdline_n;
    append(cmdline, &cmdline_n, "00000 kosmos-boot/lost=");
    lost_digits = cmdline + cmdline_n;
    append(cmdline, &cmdline_n, "00000 kosmos-boot/disk=");
    disk_word = cmdline + cmdline_n;
    append(cmdline, &cmdline_n, disk_size > 0 ? "same" : "none");

    find_screen(&screen);

    /* ACPI's root pointer, the newer revision first (`acpi.c`'s preference). */
    for (i = 0; i < table->table_entries; i++) {
        if (same_guid(&table->tables[i].guid, &ACPI_20_GUID)) {
            rsdp = table->tables[i].table;
            rsdp_v2 = true;
            break;
        }

        if (rsdp == NULL && same_guid(&table->tables[i].guid, &ACPI_10_GUID)) {
            rsdp = table->tables[i].table;
        }
    }

    if (rsdp == NULL) {
        say("the firmware lists no ACPI tables; Kosmos will see one processor");
    }

    /* The trampoline, on its own page below 4 GB. */
    tramp_bytes = (uint64_t)(efi_trampoline_end - efi_trampoline_start);
    status = boot->allocate_pages(ALLOCATE_MAX_ADDRESS, LOADER_CODE, 1,
                                  &tramp_at);

    if (status != EFI_SUCCESS || tramp_bytes > PAGE - 64
        || !outside(&k, tramp_at, PAGE)) {
        return refuse("no page below 4 GB for the jump into the kernel",
                      status);
    }

    memcpy((void *)(uintptr_t)tramp_at, efi_trampoline_start, tramp_bytes);

    /*
     * Room for the memory map, sized now and with slack, because nothing may
     * be allocated between the last GetMemoryMap and ExitBootServices - an
     * allocation is a change to the map that call is checked against.
     */
    if (boot->get_memory_map(&map_bytes, NULL, &map_key, &descriptor,
                             &descriptor_version) != EFI_BUFFER_TOO_SMALL
        || descriptor < EFI_DESCRIPTOR_MIN) {
        return refuse("the firmware will not describe its memory", EFI_SUCCESS);
    }

    ranges_cap = (unsigned)(map_bytes / descriptor) + 64u;
    map_cap = (uint64_t)ranges_cap * descriptor;

    if (boot->allocate_pool(LOADER_DATA, map_cap, &pool) != EFI_SUCCESS) {
        return refuse("no memory for the memory map", EFI_SUCCESS);
    }

    map = pool;

    if (boot->allocate_pool(LOADER_DATA, (uint64_t)ranges_cap * sizeof(*ranges),
                            &pool) != EFI_SUCCESS) {
        return refuse("no memory for the memory map", EFI_SUCCESS);
    }

    ranges = pool;
    mbi_bytes = 1024u + CMDLINE_MAX + 24u * ranges_cap;
    mbi_pages = (mbi_bytes + PAGE - 1) / PAGE;
    status = boot->allocate_pages(ALLOCATE_MAX_ADDRESS, LOADER_DATA, mbi_pages,
                                  &mbi_at);

    if (status != EFI_SUCCESS) {
        return refuse("no memory below 4 GB for the information structure",
                      status);
    }

    if (!outside(&k, (uint64_t)(uintptr_t)map, map_cap)
        || !outside(&k, (uint64_t)(uintptr_t)ranges,
                    (uint64_t)ranges_cap * sizeof(*ranges))
        || !outside(&k, mbi_at, (uint64_t)mbi_pages * PAGE)) {
        return refuse("the firmware gave the loader memory inside the kernel's "
                      "range", EFI_SUCCESS);
    }

    /*
     * **The first check**, after every file read and every allocation. A page
     * wrong in one copy is repaired from the other and counted; it is said
     * while it can be.
     */
    before = mend_copies(&k, &lost);
    digits5(before_digits, before);

    begin(&l);

    if (before == 0 && lost == 0) {
        add_text(&l, "both copies of the kernel are the file, page for page");
    } else {
        add_dec(&l, before);
        add_text(&l, " pages of the kernel changed in memory and were repaired, ");
        add_dec(&l, lost);
        add_text(&l, " could not be");
    }

    send(&l);

    begin(&l);
    add_text(&l, "handing over: entry ");
    add_hex(&l, k.img.entry, 8);
    add_text(&l, ", information at ");
    add_hex(&l, mbi_at, 8);
    add_text(&l, ", trampoline at ");
    add_hex(&l, tramp_at, 8);
    send(&l);

    /*
     * ExitBootServices, until the map it is given is the current one. The
     * specification allows the first call to fail when the map changed after
     * GetMemoryMap - printing the line above can do that - and then only
     * GetMemoryMap and ExitBootServices may be called again, which is all
     * this loop does.
     */
    for (attempt = 0; attempt < 8 && !exited; attempt++) {
        map_bytes = map_cap;
        status = boot->get_memory_map(&map_bytes, map, &map_key, &descriptor,
                                      &descriptor_version);

        if (status != EFI_SUCCESS) {
            break;
        }

        count = efi_map_convert(map, map_bytes, descriptor, ranges, ranges_cap);

        if (count > ranges_cap) {
            break;
        }

        mbi_begin(&m, (uint8_t *)(uintptr_t)mbi_at, mbi_pages * PAGE);
        mbi_cmdline(&m, cmdline);

        if (disk_size > 0) {
            mbi_module(&m, (uint32_t)disk_at, (uint32_t)(disk_at + disk_size),
                       "disk.img");
        }

        mbi_mmap(&m, ranges, count);

        if (screen.valid) {
            mbi_framebuffer(&m, screen.addr, screen.pitch, screen.width,
                            screen.height, screen.red_at, screen.red_bits,
                            screen.green_at, screen.green_bits,
                            screen.blue_at, screen.blue_bits);
        }

        mbi_efi64(&m, (uint64_t)(uintptr_t)table);

        if (rsdp != NULL) {
            mbi_acpi(&m, rsdp_v2, rsdp, rsdp_v2 ? 36u : 20u);
        }

        if (mbi_end(&m) == 0) {
            break;
        }

        console_up = false;
        status = boot->exit_boot_services(image_handle, map_key);
        exited = status == EFI_SUCCESS;
    }

    if (!exited) {
        if (attempt == 0) {
            return refuse("the memory map could not be turned into the "
                          "kernel's", status);
        }

        for (;;) {
            /* The firmware is half gone and nothing can be said. */
        }
    }

    /*
     * **With the firmware gone**: the copies checked again, the kernel copied
     * into its place - the borrowed pages are nobody's now - its tail zeroed,
     * and every page of it checked against the fingerprints. Anything
     * repaired or lost goes into the command line the structure already
     * holds, the first tag, so its text begins sixteen bytes in.
     */
    after = mend_copies(&k, &lost);
    memcpy((void *)(uintptr_t)k.img.load_addr, k.copy_a, k.img.load_bytes);
    memset((void *)(uintptr_t)k.img.load_end, 0,
           (uint64_t)k.img.bss_end - k.img.load_end);
    after += mend_placed(&k, &lost);

    {
        char *text = (char *)(uintptr_t)mbi_at + 16;

        digits5(text + (after_digits - cmdline), after);
        digits5(text + (lost_digits - cmdline), lost);

        if (disk_size > 0
            && fingerprint((uint8_t *)(uintptr_t)disk_at, disk_size)
               != disk_print) {
            memcpy(text + (disk_word - cmdline), "diff", 4);
        }
    }

    ((void (*)(uint32_t, uint32_t))(uintptr_t)tramp_at)((uint32_t)mbi_at,
                                                        k.img.entry);

    for (;;) {
    }
}
