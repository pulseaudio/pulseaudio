#ifndef foopulseflisthfoo
#define foopulseflisthfoo

/***
  This file is part of PulseAudio.

  Copyright 2006-2008 Lennart Poettering

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

#include <pulse/def.h>
#include <pulse/gccmacro.h>

#include <pulsecore/once.h>
#include <pulsecore/core-util.h>

/* A multiple-reader multipler-write lock-free free list implementation */

typedef struct pa_flist pa_flist;

/* Size is required to be a power of two, or 0 for the default size */
pa_flist * pa_flist_new(unsigned size);
void pa_flist_free(pa_flist *l, pa_free_cb_t free_cb);

/* Please note that this routine might fail! */
int pa_flist_push(pa_flist*l, void *p);
void* pa_flist_pop(pa_flist*l);

/* Please not that the destructor stuff is not really necesary, we do
 * this just to make valgrind output more useful. */

#define PA_STATIC_FLIST_DECLARE(name, size, free_cb)                    \
    static struct {                                                     \
        pa_flist *flist;                                                \
        pa_once once;                                                   \
    } name##_flist = { NULL, PA_ONCE_INIT };                            \
    static void name##_flist_init(void) {                               \
        name##_flist.flist = pa_flist_new(size);                        \
    }                                                                   \
    static inline pa_flist* name##_flist_get(void) {                    \
        pa_run_once(&name##_flist.once, name##_flist_init);             \
        return name##_flist.flist;                                      \
    }                                                                   \
    static void name##_flist_destructor(void) PA_GCC_DESTRUCTOR;        \
    static void name##_flist_destructor(void) {                         \
        if (!pa_in_valgrind())                                          \
            return;                                                     \
        if (name##_flist.flist)                                         \
            pa_flist_free(name##_flist.flist, (free_cb));               \
    }                                                                   \
    struct __stupid_useless_struct_to_allow_trailing_semicolon

#define PA_STATIC_FLIST_GET(name) (name##_flist_get())

#endif
