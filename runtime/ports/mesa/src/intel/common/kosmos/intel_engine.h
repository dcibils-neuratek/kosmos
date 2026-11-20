/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef INTEL_ENGINE_KOSMOS_H
#define INTEL_ENGINE_KOSMOS_H

/* The engines a program's contexts may be made on (`docs/gpu3d.md` G7a). */
struct intel_query_engine_info *intel_kosmos_engine_get_info(int fd);

#endif
