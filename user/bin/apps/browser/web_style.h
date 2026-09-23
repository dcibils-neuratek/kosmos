/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WEB_STYLE_H
#define KOSMOS_WEB_STYLE_H

#include <stdbool.h>
#include <stdint.h>

/*
 * The cascade, behind a plain struct.
 *
 * libcss lives on one side of this header and the renderer on the other, so
 * layout asks "what does this element look like" and gets four numbers
 * rather than a computed style, a fixed-point length and a unit it would
 * have to convert.
 */
struct web_style;

struct web_look {
    uint32_t colour;        /* 0xAARRGGBB */
    int      px;            /* font-size, already in pixels */
    bool     bold;
    bool     italic;
    bool     mono;
    bool     hidden;        /* display: none */
};

/*
 * Opens the cascade for a document: the user-agent sheet, then every
 * `<style>` in the document itself. NULL if libcss would not start.
 */
struct web_style *web_style_open(void *document);

void web_style_close(struct web_style *s);

/* What `element` computes to. False when nothing came back, and `out` is
 * left alone - the caller keeps whatever it inherited. */
bool web_style_of(struct web_style *s, void *element, struct web_look *out);

#endif /* KOSMOS_WEB_STYLE_H */
