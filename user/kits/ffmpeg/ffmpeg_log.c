/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
/*
 * FFmpeg's log, kept. `ffmpeg_log.h` says why.
 */

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "libavutil/log.h"

#include "ffmpeg_log.h"

static char said[160];

static void keep(void *avcl, int level, const char *format, va_list vl)
{
    size_t n;

    (void)avcl;

    if (level > AV_LOG_ERROR) {
        return;
    }

    vsnprintf(said, sizeof said, format, vl);
    n = strlen(said);

    while (n > 0 && (said[n - 1] == '\n' || said[n - 1] == ' ')) {
        said[--n] = '\0';
    }
}

void ffmpeg_log_keep(void)
{
    av_log_set_callback(keep);
}

const char *ffmpeg_log_said(void)
{
    return said;
}

void ffmpeg_log_clear(void)
{
    said[0] = '\0';
}
