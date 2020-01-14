#ifndef foomessagehelperhfoo
#define foomessagehelperhfoo

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

#include <stddef.h>
#include <stdbool.h>
#include <inttypes.h>

#include <pulse/cdecl.h>
#include <pulse/version.h>

/** \file
 * Utility functions for reading and writing message parameters */

PA_C_DECL_BEGIN

/** Structure which holds a parameter list. Wrapper for pa_strbuf  \since 15.0 */
typedef struct pa_message_params pa_message_params;

/** @{ \name Read functions */

/** Read raw data from a parameter list. Used to split a message parameter
 * string into list elements. The string returned in *result must not be freed.  \since 15.0 */
int pa_message_params_read_raw(char *c, char **result, void **state);

/** Read a string from a parameter list. Escaped curly braces and backslashes
 * will be unescaped. \since 15.0 */
int pa_message_params_read_string(char *c, const char **result, void **state);

/** @} */

/** @{ \name Write functions */

/** Create a new pa_message_params structure  \since 15.0 */
pa_message_params *pa_message_params_new(void);

/** Free a pa_message_params structure.  \since 15.0 */
void pa_message_params_free(pa_message_params *params);

/** Convert pa_message_params to string, free pa_message_params structure.  \since 15.0 */
char *pa_message_params_to_string_free(pa_message_params *params);

/** Start a list by writing an opening brace.  \since 15.0 */
void pa_message_params_begin_list(pa_message_params *params);

/** End a list by writing a closing brace.  \since 15.0 */
void pa_message_params_end_list(pa_message_params *params);

/** Append string to parameter list. Curly braces and backslashes will be escaped.  \since 15.0 */
void pa_message_params_write_string(pa_message_params *params, const char *value);

/** Append raw string to parameter list. Used to write incomplete strings
 * or complete parameter lists (for example arrays). Adds curly braces around
 * the string if add_braces is true.  \since 15.0 */
void pa_message_params_write_raw(pa_message_params *params, const char *value, bool add_braces);

/** @} */

PA_C_DECL_END

#endif
