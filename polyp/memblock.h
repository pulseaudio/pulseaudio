#ifndef foomemblockhfoo
#define foomemblockhfoo

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

#include <sys/types.h>
#include <inttypes.h>

enum pa_memblock_type { PA_MEMBLOCK_FIXED, PA_MEMBLOCK_APPENDED, PA_MEMBLOCK_DYNAMIC, PA_MEMBLOCK_USER };

struct pa_memblock_stat;

struct pa_memblock {
    enum pa_memblock_type type;
    unsigned ref;
    size_t length;
    void *data;
    void (*free_cb)(void *p);
    struct pa_memblock_stat *stat;
};

struct pa_memblock *pa_memblock_new(size_t length, struct pa_memblock_stat*s);
struct pa_memblock *pa_memblock_new_fixed(void *data, size_t length, struct pa_memblock_stat*s);
struct pa_memblock *pa_memblock_new_dynamic(void *data, size_t length, struct pa_memblock_stat*s);
struct pa_memblock *pa_memblock_new_user(void *data, size_t length, void (*free_cb)(void *p), struct pa_memblock_stat*s);

void pa_memblock_unref(struct pa_memblock*b);
struct pa_memblock* pa_memblock_ref(struct pa_memblock*b);

void pa_memblock_unref_fixed(struct pa_memblock*b);

struct pa_memblock_stat {
    int ref;
    unsigned total;
    unsigned total_size;
    unsigned allocated;
    unsigned allocated_size;
};

struct pa_memblock_stat* pa_memblock_stat_new(void);
void pa_memblock_stat_unref(struct pa_memblock_stat *s);
struct pa_memblock_stat * pa_memblock_stat_ref(struct pa_memblock_stat *s);

#endif
