/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>

#include "polyplib-error.h"
#include "native-common.h"

static const char* const errortab[PA_ERROR_MAX] = {
    [PA_ERROR_OK] = "OK",
    [PA_ERROR_ACCESS] = "Access denied",
    [PA_ERROR_COMMAND] = "Unknown command",
    [PA_ERROR_INVALID] = "Invalid argument",
    [PA_ERROR_EXIST] = "Entity exists",
    [PA_ERROR_NOENTITY] = "No such entity",
    [PA_ERROR_CONNECTIONREFUSED] = "Connection refused",
    [PA_ERROR_PROTOCOL] = "Protocol corrupt",
    [PA_ERROR_TIMEOUT] = "Timeout",
    [PA_ERROR_AUTHKEY] = "Not authorization key",
    [PA_ERROR_INTERNAL] = "Internal error",
    [PA_ERROR_CONNECTIONTERMINATED] = "Connection terminated",
    [PA_ERROR_KILLED] = "Entity killed",
};

const char*pa_strerror(uint32_t error) {
    if (error >= PA_ERROR_MAX)
        return NULL;

    return errortab[error];
}
