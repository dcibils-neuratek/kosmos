/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_RECORD_CORE_H
#define KOSMOS_RECORD_CORE_H

/*
 * **A camera's frames into an MP4 of H.264** (`roadmap.md` 6d 8f), with no
 * Lua in it, so that `tools/test_record.c` runs this same code on the Mac
 * and the Record Kit (`record_kosmos.c`) runs it in a process.
 *
 * lieff's `minih264e` encodes and his `minimp4` writes the file, both CC0
 * and vendored unchanged in `runtime/upstream`. Diego chose H.264 "as all
 * modern video players are h264".
 *
 * **All of its memory is the caller's**: `work`, for the encoder's frames
 * and the writer's tables, and `out`, where the file is put together. A
 * process's heap is 2 MB and the encoder alone wants more than that at
 * 640 by 480, so both are regions the program made - and the file, whole in
 * `out`, is written to the disk in one write when the recording stops.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct recorder;

/* The work memory a recording `width` by `height` needs, or 0 when it
 * cannot be recorded: both even, and at most 4096 across or down. */
size_t recorder_work_bytes(unsigned width, unsigned height);

/*
 * A recording, laid out at the start of `work`: YUY2 frames `width` by
 * `height` about `fps` a second, the file put together in `out`. NULL, and
 * why in `*why`, when it cannot start.
 */
struct recorder *recorder_open(void *work, size_t work_bytes, uint8_t *out,
                               size_t out_bytes, unsigned width,
                               unsigned height, unsigned fps,
                               const char **why);

/* One frame, `width * 2` bytes a row, taken at `time_us` on any clock that
 * only rises: how long it is shown is the time to the next one. */
bool recorder_yuy2(struct recorder *r, const uint8_t *yuy2, uint64_t time_us,
                   const char **why);

/* How far it has got: the file's bytes so far, and the frames in it. */
size_t   recorder_bytes(const struct recorder *r);
unsigned recorder_frames(const struct recorder *r);

/* The file finished - its index written after its frames - and its length
 * in `out`; 0, and why, when it could not be. */
size_t recorder_close(struct recorder *r, const char **why);

#endif /* KOSMOS_RECORD_CORE_H */
