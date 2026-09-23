/* Kosmos. Copyright (c) 2026 Diego Cibils. MIT; see LICENSE. */
#ifndef KOSMOS_WEB_SELECT_H
#define KOSMOS_WEB_SELECT_H

#include <libcss/libcss.h>

/* The handler libcss asks its thirty-six questions through, or NULL when the
 * strings it needs could not be interned. */
css_select_handler *web_select_handler(void);

#endif /* KOSMOS_WEB_SELECT_H */
