#ifndef foomemblockhfoo
#define foomemblockhfoo

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
#include <inttypes.h>

enum pa_memblock_type { PA_MEMBLOCK_FIXED, PA_MEMBLOCK_APPENDED, PA_MEMBLOCK_DYNAMIC };

struct pa_memblock {
    enum pa_memblock_type type;
    unsigned ref;
    size_t length;
    void *data;
};

struct pa_memblock *pa_memblock_new(size_t length);
struct pa_memblock *pa_memblock_new_fixed(void *data, size_t length);
struct pa_memblock *pa_memblock_new_dynamic(void *data, size_t length);

void pa_memblock_unref(struct pa_memblock*b);
struct pa_memblock* pa_memblock_ref(struct pa_memblock*b);

void pa_memblock_unref_fixed(struct pa_memblock*b);

unsigned pa_memblock_get_count(void);
unsigned pa_memblock_get_total(void);

#endif
