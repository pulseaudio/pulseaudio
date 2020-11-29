/***
  This file is part of PulseAudio.

  Copyright 2020 Igor V. Kovalenko <igor.v.kovalenko@gmail.com>

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

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>

#include "database.h"
#include "core-error.h"

pa_database* pa_database_open(const char *path, const char *fn, bool prependmid, bool for_write) {

    const char *arch_suffix = pa_database_get_arch_suffix();
    const char *filename_suffix = pa_database_get_filename_suffix();

    char *machine_id = NULL, *filename_prefix, *full_path;

    pa_database *f;

    pa_assert(!arch_suffix || arch_suffix[0]);
    pa_assert(filename_suffix && filename_suffix[0]);

    if (prependmid && !(machine_id = pa_machine_id())) {
        return NULL;
    }

    /* We include the host identifier in the file name because some database files are
     * CPU dependent, and we don't want things to go wrong if we are on a multiarch system. */
    filename_prefix = pa_sprintf_malloc("%s%s%s%s%s",
            machine_id?:"", machine_id?"-":"",
            fn,
            arch_suffix?".":"", arch_suffix?:"");

    full_path = pa_sprintf_malloc("%s" PA_PATH_SEP "%s%s", path, filename_prefix, filename_suffix);

    f = pa_database_open_internal(full_path, for_write);

    if (f)
        pa_log_info("Successfully opened '%s' database file '%s'.", fn, full_path);
    else
        pa_log("Failed to open '%s' database file '%s': %s", fn, full_path, pa_cstrerror(errno));

    pa_xfree(full_path);
    pa_xfree(filename_prefix);

    /* deallocate machine_id if it was used to construct file name */
    pa_xfree(machine_id);

    return f;
}
