#ifndef foopulsecorememtraphfoo
#define foopulsecorememtraphfoo

/***
  This file is part of PulseAudio.

  Copyright 2009 Lennart Poettering

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
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

#include <sys/types.h>

#include <pulsecore/macro.h>

typedef struct pa_memtrap pa_memtrap;

pa_memtrap* pa_memtrap_add(const void *start, size_t size);
pa_memtrap *pa_memtrap_update(pa_memtrap *m, const void *start, size_t size);

void pa_memtrap_remove(pa_memtrap *m);

pa_bool_t pa_memtrap_is_good(pa_memtrap *m);

void pa_memtrap_install(void);

#endif
