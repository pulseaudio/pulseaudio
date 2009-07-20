/***
  This file is part of PulseAudio.

  Copyright 2009 Tanu Kaskinen

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
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

#include <pulsecore/core-util.h>

#include "iface-module.h"

#define OBJECT_NAME "module"

struct pa_dbusiface_module {
    pa_module *module;
    char *path;
};

pa_dbusiface_module *pa_dbusiface_module_new(pa_module *module, const char *path_prefix) {
    pa_dbusiface_module *m;

    pa_assert(module);
    pa_assert(path_prefix);

    m = pa_xnew(pa_dbusiface_module, 1);
    m->module = module;
    m->path = pa_sprintf_malloc("%s/%s%u", path_prefix, OBJECT_NAME, module->index);

    return m;
}

void pa_dbusiface_module_free(pa_dbusiface_module *m) {
    pa_assert(m);

    pa_xfree(m->path);
    pa_xfree(m);
}

const char *pa_dbusiface_module_get_path(pa_dbusiface_module *m) {
    pa_assert(m);

    return m->path;
}
