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

#include "message-params.h"

/* Split the specified string into elements. An element is defined as
 * a sub-string between curly braces. The function is needed to parse
 * the parameters of messages. Each time it is called it returns the
 * position of the current element in result and the state pointer is
 * advanced to the next list element.
 *
 * The variable state points to, should be initialized to NULL before
 * the first call. The function returns 1 on success, 0 if end of string
 * is encountered and -1 on parse error.
 *
 * result is set to NULL on end of string or parse error. */
static int split_list(char *c, char **result, void **state) {
    char *current = *state ? *state : c;
    uint32_t open_braces;

    pa_assert(result);

    *result = NULL;

    /* Empty or no string */
    if (!current || *current == 0)
        return 0;

    /* Find opening brace */
    while (*current != 0) {

        if (*current == '{')
            break;

        /* unexpected closing brace, parse error */
        if (*current == '}')
            return -1;

        current++;
    }

    /* No opening brace found, end of string */
    if (*current == 0)
         return 0;

    *result = current + 1;
    open_braces = 1;

    while (open_braces != 0 && *current != 0) {
        current++;
        if (*current == '{')
            open_braces++;
        if (*current == '}')
            open_braces--;
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

/* Read a string from the parameter list. The state pointer is
 * advanced to the next element of the list. Returns a pointer
 * to a sub-string within c. The result must not be freed. */
int pa_message_params_read_string(char *c, const char **result, void **state) {
    char *start_pos;
    int r;

    pa_assert(result);

    if ((r = split_list(c, &start_pos, state)) == 1)
        *result = start_pos;

    return r;
}

/* Another wrapper for split_list() to distinguish between reading
 * pure string data and raw data which may contain further lists. */
int pa_message_params_read_raw(char *c, char **result, void **state) {
    return split_list(c, result, state);
}
