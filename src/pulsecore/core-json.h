#ifndef foocorejsonfoo
#define foocorejsonfoo

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#include <pulse/json.h>

#include <pulsecore/hashmap.h>

struct pa_json_object {
    pa_json_type type;

    union {
        int64_t int_value;
        double double_value;
        bool bool_value;
        char *string_value;
        pa_hashmap *object_values; /* name -> object */
        pa_idxset *array_values; /* objects */
    };
};

/** Returns pa_hashmap (char* -> const pa_json_object*) to iterate over object members. \since 15.0 */
const pa_hashmap *pa_json_object_get_object_member_hashmap(const pa_json_object *o);

#endif
