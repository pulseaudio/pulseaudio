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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <signal.h>
#include <assert.h>
#include <unistd.h>

#include "memory.h"
#include "util.h"

/* Make sure not to allocate more than this much memory. */
#define MAX_ALLOC_SIZE (1024*1024*20) /* 20MB */

/* #undef malloc */
/* #undef free */
/* #undef realloc */
/* #undef strndup */
/* #undef strdup */

/** called in case of an OOM situation. Prints an error message and
 * exits */
static void oom(void) {
    static const char e[] = "Not enough memory\n";
    pa_loop_write(STDERR_FILENO, e, sizeof(e)-1);
    raise(SIGQUIT);
    _exit(1);
}

void* pa_xmalloc(size_t size) {
    void *p;
    assert(size > 0);
    assert(size < MAX_ALLOC_SIZE);
    
    if (!(p = malloc(size)))
        oom();
        
    return p;
}

void* pa_xmalloc0(size_t size) {
    void *p;
    assert(size > 0);
    assert(size < MAX_ALLOC_SIZE);
    
    if (!(p = calloc(1, size)))
        oom();
        
    return p;
}
    
void *pa_xrealloc(void *ptr, size_t size) {
    void *p;
    assert(size > 0);
    assert(size < MAX_ALLOC_SIZE);
    
    if (!(p = realloc(ptr, size)))
        oom();
    return p;
}

void* pa_xmemdup(const void *p, size_t l) {
    if (!p)
        return NULL;
    else {
        char *r = pa_xmalloc(l);
        memcpy(r, p, l);
        return r;
    }
}

char *pa_xstrdup(const char *s) {
    if (!s)
        return NULL;

    return pa_xmemdup(s, strlen(s)+1);
}

char *pa_xstrndup(const char *s, size_t l) {
    if (!s)
        return NULL;
    else {
        char *r;
        size_t t = strlen(s);

        if (t > l)
            t = l;
        
        r = pa_xmemdup(s, t+1);
        r[t] = 0;
        return r;
    }
}

