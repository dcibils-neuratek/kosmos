/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * **The engines a program may use** (`docs/gpu3d.md` G7a): the two
 * `GPU_OP_CONTEXT_NEW` makes contexts on, the render engine and the
 * blitter, one of each - `GPU_ENGINE_RENDER` and `GPU_ENGINE_COPY`. The
 * video engines run the driver's own work and are not offered.
 */

#include <stdlib.h>

#include "common/intel_engine.h"
#include "common/kosmos/intel_engine.h"

struct intel_query_engine_info *
intel_kosmos_engine_get_info(int fd)
{
   struct intel_query_engine_info *info;

   if (fd < 0)
      return NULL;

   info = calloc(1, sizeof(*info) + 2 * sizeof(info->engines[0]));
   if (info == NULL)
      return NULL;

   info->num_engines = 2;
   info->engines[0].engine_class = INTEL_ENGINE_CLASS_RENDER;
   info->engines[1].engine_class = INTEL_ENGINE_CLASS_COPY;
   return info;
}
