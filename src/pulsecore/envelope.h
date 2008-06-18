#ifndef foopulseenvelopehfoo
#define foopulseenvelopehfoo

/***
  This file is part of PulseAudio.

  Copyright 2007 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulsecore/macro.h>
#include <pulsecore/memchunk.h>

#include <pulse/sample.h>

#define PA_ENVELOPE_POINTS_MAX 4U

typedef struct pa_envelope pa_envelope;
typedef struct pa_envelope_item pa_envelope_item;

typedef struct pa_envelope_def {
    unsigned n_points;

    pa_usec_t points_x[PA_ENVELOPE_POINTS_MAX];
    struct {
        int32_t i[PA_ENVELOPE_POINTS_MAX];
        float f[PA_ENVELOPE_POINTS_MAX];
    } points_y;
} pa_envelope_def;

pa_envelope *pa_envelope_new(const pa_sample_spec *ss);
void pa_envelope_free(pa_envelope *e);
pa_envelope_item *pa_envelope_add(pa_envelope *e, const pa_envelope_def *def);
pa_envelope_item *pa_envelope_replace(pa_envelope *e, pa_envelope_item *i, const pa_envelope_def *def);
void pa_envelope_remove(pa_envelope *e, pa_envelope_item *i);
void pa_envelope_apply(pa_envelope *e, pa_memchunk *chunk);
void pa_envelope_rewind(pa_envelope *e, size_t n_bytes);

#endif
