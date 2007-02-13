#ifndef foopulseoncehfoo
#define foopulseoncehfoo

/* $Id$ */

/***
  This file is part of PulseAudio.

  Copyright 2006 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#include <pulsecore/mutex.h>

typedef struct pa_once {
    unsigned int once_value;
    pa_mutex *mutex;
} pa_once_t;

#define PA_ONCE_INIT { .once_value = 0, .mutex = NULL }

typedef void (*pa_once_func_t) (void);

void pa_once(pa_once_t *o, pa_once_func_t f);

#endif
