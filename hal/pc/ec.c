/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The embedded controller, watched and not touched.
 *
 * A ThinkPad's F5 and F6 are not keys (`docs/thinkpad.md` 8b): the embedded
 * controller takes them and raises an event, which reaches an operating
 * system as a general-purpose event - GPE 0x6E on the T14 - and a query the
 * system then asks the controller for: 0x14 brighter, 0x15 dimmer.
 *
 * **Who hears that event is the question this file exists to answer.** An
 * operating system switches the machine into ACPI mode at boot - it writes
 * the FADT's "ACPI enable" value to its SMI command port, and the chipset
 * sets SCI_EN - and until one does, the firmware's own hidden handler (SMM)
 * services the controller's events itself. Kosmos has never switched. So an
 * F5 may be handled, and swallowed, before anything here could see it; and
 * taking the controller's events away from the firmware blind would lose
 * the ones it needs, the lid and the power button among them.
 *
 * So this only reads, the way the backlight was read before it was written:
 *
 *   at boot   what the FADT says - the SCI, the SMI command port and the
 *             enable value, where PM1a control and GPE0 are - whether
 *             SCI_EN is set, and whether a controller answers at all;
 *   each tick on core 0, its status byte and every GPE status byte, and
 *             a line in the log whenever one changes - the first sixty-four.
 *
 * Nothing is written to any port. A press of F5 that sets the controller's
 * SCI_EVT, or a GPE status bit, and the firmware clearing it again, is what
 * the log will show - or nothing, which says the firmware owns it.
 *
 * **Offsets are the spec's and ACPICA's.** The FADT fields and the ECDT's
 * are in `acpi.c`, taken from `iasl -T` templates compiled and disassembled.
 * The status bits are the ACPI specification's embedded controller
 * interface: OBF 0, IBF 1, CMD 3, BURST 4, SCI_EVT 5, SMI_EVT 6; SCI_EN is
 * bit 0 of PM1 control. The ports, when the machine has no ECDT, are
 * 62h and 66h, which is what the T14's own DSDT gives its controller's
 * `_CRS` - and a controller that is not there reads as FFh, which is the
 * test for there being one.
 */

#include <stdbool.h>
#include <stdint.h>

#include "acpi.h"
#include "console.h"
#include "ec.h"
#include "hal.h"
#include "pc.h"

#define EC_SCI_EVT      0x20u
#define PM1_SCI_EN      0x0001u

#define EC_DATA_DEFAULT 0x62u
#define EC_CMD_DEFAULT  0x66u

/* The status half of GPE0, at most this much of it watched. */
#define GPE_WATCHED     32u

/* Lines of change said, at most - a controller that toggles a bit on every
 * tick would otherwise fill the log in a second. */
#define CHANGES_SAID    64u

static bool     watching;
static uint16_t ec_cmd;
static uint16_t gpe0;
static unsigned gpe_bytes;
static uint8_t  ec_last;
static uint8_t  gpe_last[GPE_WATCHED];
static unsigned said;

static void put_hex8(uint8_t v)
{
    kputx(v, 2);
}

void ec_watch_init(void)
{
    struct acpi_ec_facts f;
    uint8_t status;
    unsigned i;

    if (!acpi_ec_facts(&f)) {
        kputs("ec: no FADT, so nothing to say about events\n");
        return;
    }

    kputs("ec: the FADT: SCI ");
    kputu(f.sci_int);
    kputs(", SMI command port 0x");
    kputx(f.smi_cmd, 2);
    kputs(" (0x");
    kputx(f.acpi_enable, 2);
    kputs(" enables ACPI), PM1a control at 0x");
    kputx(f.pm1a_cnt, 4);
    kputs(", GPE0 at 0x");
    kputx(f.gpe0_blk, 4);
    kputs(", ");
    kputu(f.gpe0_len);
    kputs(" bytes");
    kputs(f.hardware_reduced ? ", hardware reduced\n" : "\n");

    if (f.pm1a_cnt != 0) {
        uint16_t cnt = pc_in16((uint16_t)f.pm1a_cnt);

        kputs((cnt & PM1_SCI_EN) != 0
              ? "ec: SCI_EN is set - ACPI mode: events come to the system\n"
              : "ec: SCI_EN is clear - the firmware's SMM owns events until "
                "a system asks for them, and Kosmos has not\n");
    }

    ec_cmd = f.ec_cmd != 0 ? (uint16_t)f.ec_cmd : EC_CMD_DEFAULT;

    kputs(f.ec_cmd != 0 ? "ec: the ECDT puts the controller at 0x"
                        : "ec: no ECDT; the controller looked for at 0x");
    kputx(ec_cmd, 2);

    status = pc_in8(ec_cmd);

    if (status == 0xffu) {
        kputs(" - nothing answers there\n");
        return;
    }

    kputs(", status 0x");
    put_hex8(status);
    kputs("\n");

    ec_last = status;
    gpe0 = (uint16_t)f.gpe0_blk;
    gpe_bytes = f.gpe0_len / 2u;

    if (gpe_bytes > GPE_WATCHED) {
        gpe_bytes = GPE_WATCHED;
    }

    for (i = 0; gpe0 != 0 && i < gpe_bytes; i++) {
        gpe_last[i] = pc_in8((uint16_t)(gpe0 + i));
    }

    kputs("ec: watching its status and GPE0's status bits, reading only - "
          "a line for each change\n");
    watching = true;
}

void ec_watch_tick(void)
{
    uint8_t status;
    unsigned i;

    if (!watching || said >= CHANGES_SAID) {
        return;
    }

    status = pc_in8(ec_cmd);

    if (status != ec_last) {
        kputs("ec: status 0x");
        put_hex8(ec_last);
        kputs(" -> 0x");
        put_hex8(status);
        kputs(((status & ~ec_last) & EC_SCI_EVT) != 0 ? " - SCI_EVT set\n"
              : ((ec_last & ~status) & EC_SCI_EVT) != 0 ? " - SCI_EVT cleared\n"
              : "\n");
        ec_last = status;
        said++;
    }

    for (i = 0; gpe0 != 0 && i < gpe_bytes && said < CHANGES_SAID; i++) {
        uint8_t now = pc_in8((uint16_t)(gpe0 + i));
        uint8_t changed = (uint8_t)(now ^ gpe_last[i]);
        unsigned bit;

        for (bit = 0; bit < 8 && changed != 0; bit++) {
            if ((changed & (1u << bit)) == 0) {
                continue;
            }

            kputs("ec: GPE 0x");
            put_hex8((uint8_t)(i * 8u + bit));
            kputs((now & (1u << bit)) != 0 ? " status set\n"
                                           : " status cleared\n");
            said++;
        }

        gpe_last[i] = now;
    }

    if (said >= CHANGES_SAID) {
        kputs("ec: sixty-four changes said, and no more\n");
    }
}
