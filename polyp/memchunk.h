#ifndef foomemchunkhfoo
#define foomemchunkhfoo

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

#include "memblock.h"

struct pa_memchunk {
    struct pa_memblock *memblock;
    size_t index, length;
};

void pa_memchunk_make_writable(struct pa_memchunk *c, struct pa_memblock_stat *s);

struct pa_mcalign;

struct pa_mcalign *pa_mcalign_new(size_t base, struct pa_memblock_stat *s);
void pa_mcalign_free(struct pa_mcalign *m);
void pa_mcalign_push(struct pa_mcalign *m, const struct pa_memchunk *c);
int pa_mcalign_pop(struct pa_mcalign *m, struct pa_memchunk *c);

#endif
