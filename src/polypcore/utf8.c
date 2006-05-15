/* $Id */

/* This file is based on the GLIB utf8 validation functions. The
 * original license text follows. */

/* gutf8.c - Operations on UTF-8 strings.
 *
 * Copyright (C) 1999 Tom Tromey
 * Copyright (C) 2000 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.	 See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 02111-1307, USA.
 */

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <inttypes.h>

#include "utf8.h"

static inline int is_unicode_valid(uint32_t ch) {
    if (ch >= 0x110000) /* End of unicode space */
        return 0;
    if ((ch & 0xFFFFF800) == 0xD800) /* Reserved area for UTF-16 */
        return 0;
    if ((ch >= 0xFDD0) && (ch <= 0xFDEF)) /* Reserved */
        return 0;
    if ((ch & 0xFFFE) == 0xFFFE) /* BOM (Byte Order Mark) */
        return 0;
    return 1;
}

static inline int is_continuation_char(uint8_t ch) {
    if ((ch & 0xc0) != 0x80) /* 10xxxxxx */
        return 0;
    return 1;
}

static inline void merge_continuation_char(uint32_t *u_ch, uint8_t ch) {
    *u_ch <<= 6;
    *u_ch |= ch & 0x3f;
}

const char* pa_utf8_valid (const char *str) {
    uint32_t val = 0;
    uint32_t min = 0;
    const uint8_t *p, *last;

    for (p = (uint8_t*)str; *p; p++) {
        if (*p < 128)
            /* done */;
        else {
            last = p;

            if ((*p & 0xe0) == 0xc0) { /* 110xxxxx two-char seq. */
                min = 128;
                val = *p & 0x1e;
                goto ONE_REMAINING;
            } else if ((*p & 0xf0) == 0xe0) { /* 1110xxxx three-char seq.*/
                min = (1 << 11);
                val = *p & 0x0f;
                goto TWO_REMAINING;
            } else if ((*p & 0xf8) == 0xf0) { /* 11110xxx four-char seq */
                min = (1 << 16);
                val = *p & 0x07;
            } else
                goto error;

            p++;
            if (!is_continuation_char(*p))
                goto error;
            merge_continuation_char(&val, *p);

TWO_REMAINING:
            p++;
            if (!is_continuation_char(*p))
                goto error;
            merge_continuation_char(&val, *p);

ONE_REMAINING:
            p++;
            if (!is_continuation_char(*p))
                goto error;
            merge_continuation_char(&val, *p);

            if (val < min)
                goto error;

            if (!is_unicode_valid(val))
                goto error;
        }
    }

    return str;

error:
    return NULL;
}
