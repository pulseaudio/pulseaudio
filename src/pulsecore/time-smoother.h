#ifndef foopulsetimesmootherhfoo
#define foopulsetimesmootherhfoo

/* $Id$ */

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
#include <pulse/sample.h>

typedef struct pa_smoother pa_smoother;

pa_smoother* pa_smoother_new(pa_usec_t adjust_time, pa_usec_t history_time, pa_bool_t monotonic);
void pa_smoother_free(pa_smoother* s);

void pa_smoother_put(pa_smoother *s, pa_usec_t x, pa_usec_t y);
pa_usec_t pa_smoother_get(pa_smoother *s, pa_usec_t x);

void pa_smoother_set_time_offset(pa_smoother *s, pa_usec_t offset);

void pa_smoother_pause(pa_smoother *s, pa_usec_t x);
void pa_smoother_resume(pa_smoother *s, pa_usec_t x);

#endif
