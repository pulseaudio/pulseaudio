/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#include <pulse/xmalloc.h>

#include <pulsecore/macro.h>
#include <pulsecore/strbuf.h>
#include <pulsecore/core-util.h>

#include "message-params.h"

/* Message parameter structure, a wrapper for pa_strbuf */
struct pa_message_params {
    pa_strbuf *buffer;
};

/* Split the specified string into elements. An element is defined as
 * a sub-string between curly braces. The function is needed to parse
 * the parameters of messages. Each time it is called it returns the
 * position of the current element in result and the state pointer is
 * advanced to the next list element. On return, the parameter
 * *is_unpacked indicates if the string is plain text or contains a
 * sub-list. is_unpacked may be NULL.
 *
 * The variable state points to, should be initialized to NULL before
 * the first call. The function returns 1 on success, 0 if end of string
 * is encountered and -1 on parse error.
 *
 * result is set to NULL on end of string or parse error. */
static int split_list(char *c, char **result, bool *is_unpacked, void **state) {
    char *current = *state ? *state : c;
    uint32_t open_braces;
    bool found_backslash = false;

    pa_assert(result);

    *result = NULL;

    /* Empty or no string */
    if (!current || *current == 0)
        return 0;

    /* Find opening brace */
    while (*current != 0) {

        /* Skip escaped curly braces. */
        if (*current == '\\' && !found_backslash) {
            found_backslash = true;
            current++;
            continue;
        }

        if (*current == '{' && !found_backslash)
            break;

        /* unexpected closing brace, parse error */
        if (*current == '}' && !found_backslash)
            return -1;

        found_backslash = false;
        current++;
    }

    /* No opening brace found, end of string */
    if (*current == 0)
        return 0;

    if (is_unpacked)
        *is_unpacked = true;
    *result = current + 1;
    found_backslash = false;
    open_braces = 1;

    while (open_braces != 0 && *current != 0) {
        current++;

        /* Skip escaped curly braces. */
        if (*current == '\\' && !found_backslash) {
            found_backslash = true;
            continue;
        }

        if (*current == '{' && !found_backslash) {
            open_braces++;
            if (is_unpacked)
                *is_unpacked = false;
        }
        if (*current == '}' && !found_backslash)
            open_braces--;

        found_backslash = false;
    }

    /* Parse error, closing brace missing */
    if (open_braces != 0) {
        *result = NULL;
        return -1;
    }

    /* Replace } with 0 */
    *current = 0;

    *state = current + 1;

    return 1;
}

/* Read functions */

/* Read a string from the parameter list. The state pointer is
 * advanced to the next element of the list. Returns a pointer
 * to a sub-string within c. Escape characters will be removed
 * from the string. The result must not be freed. */
int pa_message_params_read_string(char *c, const char **result, void **state) {
    char *start_pos;
    char *value = NULL;
    int r;
    bool is_unpacked = true;

    pa_assert(result);

    if ((r = split_list(c, &start_pos, &is_unpacked, state)) == 1)
        value = start_pos;

    /* Check if we got a plain string not containing further lists */
    if (!is_unpacked) {
        /* Parse error */
        r = -1;
        value = NULL;
    }

    if (value)
        *result = pa_unescape(value);

    return r;
}

/* A wrapper for split_list() to distinguish between reading pure
 * string data and raw data which may contain further lists. */
int pa_message_params_read_raw(char *c, char **result, void **state) {
    return split_list(c, result, NULL, state);
}

/* Write functions. The functions are wrapper functions around pa_strbuf,
 * so that the client does not need to use pa_strbuf directly. */

/* Creates a new pa_message_param structure */
pa_message_params *pa_message_params_new(void) {
    pa_message_params *params;

    params = pa_xnew(pa_message_params, 1);
    params->buffer = pa_strbuf_new();

    return params;
}

/* Frees a pa_message_params structure */
void pa_message_params_free(pa_message_params *params) {
    pa_assert(params);

    pa_strbuf_free(params->buffer);
    pa_xfree(params);
}

/* Converts a pa_message_param structure to string and frees the structure.
 * The returned string needs to be freed with pa_xree(). */
char *pa_message_params_to_string_free(pa_message_params *params) {
    char *result;

    pa_assert(params);

    result = pa_strbuf_to_string_free(params->buffer);

    pa_xfree(params);
    return result;
}

/* Writes an opening curly brace */
void pa_message_params_begin_list(pa_message_params *params) {

    pa_assert(params);

    pa_strbuf_putc(params->buffer, '{');
}

/* Writes a closing curly brace */
void pa_message_params_end_list(pa_message_params *params) {

    pa_assert(params);

    pa_strbuf_putc(params->buffer, '}');
}

/* Writes a string to a message_params structure, adding curly braces
 * around the string and escaping curly braces within the string. */
void pa_message_params_write_string(pa_message_params *params, const char *value) {
    char *output;

    pa_assert(params);

    /* Null value is written as empty element */
    if (!value)
        value = "";

    output = pa_escape(value, "{}");
    pa_strbuf_printf(params->buffer, "{%s}", output);

    pa_xfree(output);
}

/* Writes a raw string to a message_params structure, adding curly braces
 * around the string if add_braces is true. This function can be used to
 * write parts of a string or whole parameter lists that have been prepared
 * elsewhere (for example an array). */
void pa_message_params_write_raw(pa_message_params *params, const char *value, bool add_braces) {
    pa_assert(params);

    /* Null value is written as empty element */
    if (!value)
        value = "";

    if (add_braces)
        pa_strbuf_printf(params->buffer, "{%s}", value);
    else
        pa_strbuf_puts(params->buffer, value);
}
