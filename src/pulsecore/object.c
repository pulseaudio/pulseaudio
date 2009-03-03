/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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

#include "object.h"

pa_object *pa_object_new_internal(size_t size, const char *type_name, int (*check_type)(const char *type_name)) {
    pa_object *o;

    pa_assert(size > sizeof(pa_object));
    pa_assert(type_name);

    if (!check_type)
        check_type = pa_object_check_type;

    pa_assert(check_type(type_name));
    pa_assert(check_type("pa_object"));

    o = pa_xmalloc(size);
    PA_REFCNT_INIT(o);
    o->type_name = type_name;
    o->free = pa_object_free;
    o->check_type = check_type;

    return o;
}

pa_object *pa_object_ref(pa_object *o) {
    pa_object_assert_ref(o);

    PA_REFCNT_INC(o);
    return o;
}

void pa_object_unref(pa_object *o) {
    pa_object_assert_ref(o);

    if (PA_REFCNT_DEC(o) <= 0) {
        pa_assert(o->free);
        o->free(o);
    }
}

int pa_object_check_type(const char *type_name) {
    pa_assert(type_name);

    return strcmp(type_name, "pa_object") == 0;
}
