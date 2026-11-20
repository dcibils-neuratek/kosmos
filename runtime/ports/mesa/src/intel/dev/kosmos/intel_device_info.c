/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The device, from Kosmos's GPU driver** (`intel_device_info.h` here,
 * `docs/gpu3d.md` G7a).
 *
 * i915 is asked a dozen things by number; Kosmos's driver is asked one -
 * `GPU_OP_STATS`, whose `device` is the PCI id - and the rest is either
 * Mesa's own table for that id, which knows Skylake's slices, subslices
 * and timestamp, or a fact of the protocol:
 *
 * - **a 48-bit address space per program**, the context's own four-level
 *   page table (G6a), with addresses the program chooses and binds;
 * - **context isolation**: one context's registers are not another's,
 *   since each begins from the engine's default state (G6e);
 * - **system memory only**, write-back, coherent with the processor through
 *   the last-level cache the HD 530 shares with it;
 * - and none of i915's own interfaces - tiling, userptr, caching, PAT -
 *   which `anv` asks about to choose a path, and never needs here.
 *
 * The topology is the table's, filled into masks by the caller as for a
 * kernel that predates topology queries; the revision is not in the
 * protocol yet, and is nought (`roadmap.md`, G7).
 */

#include <string.h>

#include "dev/intel_device_info.h"
#include "dev/kosmos/intel_device_info.h"

#include <kosmos_gpu.h>

bool
intel_device_info_kosmos_get_pci_id(int fd, uint32_t *device, uint32_t *revision)
{
   struct gpu_stats stats;

   if (fd < 0 || kg_stats(fd, &stats) != GPU_OK || stats.device == 0)
      return false;

   *device = stats.device;
   *revision = 0;
   return true;
}

bool
intel_device_info_kosmos_get_info_from_fd(int fd, struct intel_device_info *devinfo)
{
   (void)fd;

   devinfo->revision = 0;
   devinfo->gtt_size = 1ull << 48;
   devinfo->aperture_bytes = devinfo->gtt_size;
   devinfo->has_context_isolation = true;
   devinfo->has_mmap_offset = true;
   devinfo->has_partial_mmap_offset = true;
   devinfo->has_tiling_uapi = false;
   devinfo->has_userptr_uapi = false;
   devinfo->has_caching_uapi = false;
   devinfo->has_madvise_purgeable = false;
   devinfo->has_bit6_swizzle = false;
   devinfo->mem_alignment = 4096;

   return intel_device_info_compute_system_memory(devinfo, false);
}
