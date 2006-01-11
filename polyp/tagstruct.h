#ifndef footagstructhfoo
#define footagstructhfoo

/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <inttypes.h>
#include <sys/types.h>
#include <sys/time.h>
#include <time.h>

#include "sample.h"

typedef struct pa_tagstruct pa_tagstruct;

pa_tagstruct *pa_tagstruct_new(const uint8_t* data, size_t length);
void pa_tagstruct_free(pa_tagstruct*t);
uint8_t* pa_tagstruct_free_data(pa_tagstruct*t, size_t *l);

void pa_tagstruct_puts(pa_tagstruct*t, const char *s);
void pa_tagstruct_putu8(pa_tagstruct*t, uint8_t c);
void pa_tagstruct_putu32(pa_tagstruct*t, uint32_t i);
void pa_tagstruct_putu64(pa_tagstruct*t, uint64_t i);
void pa_tagstruct_put_sample_spec(pa_tagstruct *t, const pa_sample_spec *ss);
void pa_tagstruct_put_arbitrary(pa_tagstruct*t, const void *p, size_t length);
void pa_tagstruct_put_boolean(pa_tagstruct*t, int b);
void pa_tagstruct_put_timeval(pa_tagstruct*t, const struct timeval *tv);
void pa_tagstruct_put_usec(pa_tagstruct*t, pa_usec_t u);

int pa_tagstruct_gets(pa_tagstruct*t, const char **s);
int pa_tagstruct_getu8(pa_tagstruct*t, uint8_t *c);
int pa_tagstruct_getu32(pa_tagstruct*t, uint32_t *i);
int pa_tagstruct_getu64(pa_tagstruct*t, uint64_t *i);
int pa_tagstruct_get_sample_spec(pa_tagstruct *t, pa_sample_spec *ss);
int pa_tagstruct_get_arbitrary(pa_tagstruct *t, const void **p, size_t length);
int pa_tagstruct_get_boolean(pa_tagstruct *t, int *b);
int pa_tagstruct_get_timeval(pa_tagstruct*t, struct timeval *tv);
int pa_tagstruct_get_usec(pa_tagstruct*t, pa_usec_t *u);

int pa_tagstruct_eof(pa_tagstruct*t);
const uint8_t* pa_tagstruct_data(pa_tagstruct*t, size_t *l);

#endif
