/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef INTEL_DEVICE_INFO_KOSMOS_H
#define INTEL_DEVICE_INFO_KOSMOS_H

/*
 * **An Intel GPU as Kosmos's driver describes it** (`docs/gpu3d.md` G7a):
 * the `fd` Mesa carries is the process's capability to `/Devices/gpu`
 * (the GPU Kit's `kg_device`), and what i915 answers by `getparam` and
 * queries is answered here from the driver's stats and Mesa's own table.
 */

#include <stdbool.h>
#include <stdint.h>

struct intel_device_info;

/* The PCI device id and revision the driver reports. */
bool intel_device_info_kosmos_get_pci_id(int fd, uint32_t *device, uint32_t *revision);

/* What a kernel's queries fill in, for a device whose table is loaded. */
bool intel_device_info_kosmos_get_info_from_fd(int fd, struct intel_device_info *devinfo);

#endif
