/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WEB_SELECT_H
#define KOSMOS_WEB_SELECT_H

#include <stdbool.h>

#include <dom/dom.h>
#include <libcss/libcss.h>

/* The handler libcss asks its thirty-six questions through, or NULL when the
 * strings it needs could not be interned. */
css_select_handler *web_select_handler(void);

/* Whether a node is an element rather than text or a comment: the first of
 * those questions, and `web_paint.c`'s as it walks a document. */
bool web_is_element(dom_node *node);

#endif /* KOSMOS_WEB_SELECT_H */
