#ifndef foopolyplibsimplehfoo
#define foopolyplibsimplehfoo

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

#include <sys/types.h>

#include "sample.h"
#include "polyplib-def.h"

struct pa_simple;

struct pa_simple* pa_simple_new(
    const char *server,
    const char *name,
    enum pa_stream_direction dir,
    const char *dev,
    const char *stream_name,
    const struct pa_sample_spec *ss,
    const struct pa_buffer_attr *attr,
    int *error);

void pa_simple_free(struct pa_simple *s);

int pa_simple_write(struct pa_simple *s, const void*data, size_t length, int *error);
int pa_simple_drain(struct pa_simple *s, int *error);

int pa_simple_read(struct pa_simple *s, void*data, size_t length, int *error);

#endif
