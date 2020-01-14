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

/** Read raw data from a parameter list. Used to split a message parameter
 * string into list elements  \since 15.0 */
int pa_message_params_read_raw(char *c, char **result, void **state);

/** Read a string from a parameter list. \since 15.0 */
int pa_message_params_read_string(char *c, const char **result, void **state);

PA_C_DECL_END

#endif
