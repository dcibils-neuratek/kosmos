/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_FFMPEG_LOG_H
#define KOSMOS_FFMPEG_LOG_H

/*
 * What FFmpeg says, kept rather than printed - one place for every kit
 * built on it, because FFmpeg's log is one per process: two kits each
 * installing a callback would each hide the other's complaints.
 *
 * FFmpeg's default is `fprintf(stderr, ...)`, which here would be the
 * console of whatever process decodes, and a damaged film says something
 * about every damaged slice - dozens of lines a second. So the last
 * complaint at error level is kept, and a kit answers "why" with it.
 */

/* Installs the callback; any kit calls it before its first decoder. */
void ffmpeg_log_keep(void);

/* The last complaint since `ffmpeg_log_clear`, or "". */
const char *ffmpeg_log_said(void);

void ffmpeg_log_clear(void);

#endif /* KOSMOS_FFMPEG_LOG_H */
