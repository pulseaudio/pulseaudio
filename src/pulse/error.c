/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/native-common.h>

#include "error.h"

const char*pa_strerror(int error) {

    static const char* const errortab[PA_ERR_MAX] = {
        [PA_OK] = "OK",
        [PA_ERR_ACCESS] = "Access denied",
        [PA_ERR_COMMAND] = "Unknown command",
        [PA_ERR_INVALID] = "Invalid argument",
        [PA_ERR_EXIST] = "Entity exists",
        [PA_ERR_NOENTITY] = "No such entity",
        [PA_ERR_CONNECTIONREFUSED] = "Connection refused",
        [PA_ERR_PROTOCOL] = "Protocol error",
        [PA_ERR_TIMEOUT] = "Timeout",
        [PA_ERR_AUTHKEY] = "No authorization key",
        [PA_ERR_INTERNAL] = "Internal error",
        [PA_ERR_CONNECTIONTERMINATED] = "Connection terminated",
        [PA_ERR_KILLED] = "Entity killed",
        [PA_ERR_INVALIDSERVER] = "Invalid server",
        [PA_ERR_MODINITFAILED] = "Module initalization failed",
        [PA_ERR_BADSTATE] = "Bad state",
        [PA_ERR_NODATA] = "No data",
        [PA_ERR_VERSION] = "Incompatible protocol version",
        [PA_ERR_TOOLARGE] = "Too large"
    };

    if (error < 0 || error >= PA_ERR_MAX)
        return NULL;

    return errortab[error];
}
