#ifndef fooremapfoo
#define fooremapfoo

/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2009 Wim Taymans <wim.taymans@collabora.co.uk.com>

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

#include <pulse/sample.h>

typedef struct pa_remap pa_remap_t;

typedef void (*pa_do_remap_func_t) (pa_remap_t *m, void *d, const void *s, unsigned n);

struct pa_remap {
    pa_sample_format_t *format;
    pa_sample_spec *i_ss, *o_ss;
    float map_table_f[PA_CHANNELS_MAX][PA_CHANNELS_MAX];
    int32_t map_table_i[PA_CHANNELS_MAX][PA_CHANNELS_MAX];
    pa_do_remap_func_t do_remap;
};

void pa_init_remap (pa_remap_t *m);

/* custom installation of init functions */
typedef void (*pa_init_remap_func_t) (pa_remap_t *m);

pa_init_remap_func_t pa_get_init_remap_func(void);
void pa_set_init_remap_func(pa_init_remap_func_t func);

#endif /* fooremapfoo */
