/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * The machine's name, from SMBIOS. `smbios.c` has the argument, and
 * `smbios_decode.h` the format.
 */
#ifndef KOSMOS_HAL_PC_SMBIOS_H
#define KOSMOS_HAL_PC_SMBIOS_H

/*
 * Reads System Information while firmware memory is still mapped, and keeps
 * a copy that `hal_machine_ident` answers from.
 *
 * `hal_early_init` calls it straight after `pc_capture_memory`, and the order
 * matters: the EFI System Table's address is one of the tags that call
 * captures.
 */
void pc_capture_machine(void);

#endif
