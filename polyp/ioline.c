/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "ioline.h"
#include "xmalloc.h"

#define BUFFER_LIMIT (64*1024)
#define READ_SIZE (1024)

struct pa_ioline {
    struct pa_iochannel *io;
    int dead;

    char *wbuf;
    size_t wbuf_length, wbuf_index, wbuf_valid_length;

    char *rbuf;
    size_t rbuf_length, rbuf_index, rbuf_valid_length;

    void (*callback)(struct pa_ioline*io, const char *s, void *userdata);
    void *userdata;
};

static void io_callback(struct pa_iochannel*io, void *userdata);
static int do_write(struct pa_ioline *l);

struct pa_ioline* pa_ioline_new(struct pa_iochannel *io) {
    struct pa_ioline *l;
    assert(io);
    
    l = pa_xmalloc(sizeof(struct pa_ioline));
    l->io = io;
    l->dead = 0;

    l->wbuf = NULL;
    l->wbuf_length = l->wbuf_index = l->wbuf_valid_length = 0;

    l->rbuf = NULL;
    l->rbuf_length = l->rbuf_index = l->rbuf_valid_length = 0;

    l->callback = NULL;
    l->userdata = NULL;

    pa_iochannel_set_callback(io, io_callback, l);
    
    return l;
}

void pa_ioline_free(struct pa_ioline *l) {
    assert(l);
    pa_iochannel_free(l->io);
    pa_xfree(l->wbuf);
    pa_xfree(l->rbuf);
    pa_xfree(l);
}

void pa_ioline_puts(struct pa_ioline *l, const char *c) {
    size_t len;
    assert(l && c);
    
    len = strlen(c);
    if (len > BUFFER_LIMIT - l->wbuf_valid_length)
        len = BUFFER_LIMIT - l->wbuf_valid_length;

    if (!len)
        return;
    
    if (len > l->wbuf_length - l->wbuf_valid_length) {
        size_t n = l->wbuf_valid_length+len;
        char *new = pa_xmalloc(n);
        if (l->wbuf) {
            memcpy(new, l->wbuf+l->wbuf_index, l->wbuf_valid_length);
            pa_xfree(l->wbuf);
        }
        l->wbuf = new;
        l->wbuf_length = n;
        l->wbuf_index = 0;
    } else if (len > l->wbuf_length - l->wbuf_valid_length - l->wbuf_index) {
        memmove(l->wbuf, l->wbuf+l->wbuf_index, l->wbuf_valid_length);
        l->wbuf_index = 0;
    }

    memcpy(l->wbuf+l->wbuf_index+l->wbuf_valid_length, c, len);
    l->wbuf_valid_length += len;

    do_write(l);
}

void pa_ioline_set_callback(struct pa_ioline*l, void (*callback)(struct pa_ioline*io, const char *s, void *userdata), void *userdata) {
    assert(l && callback);
    l->callback = callback;
    l->userdata = userdata;
}

static int do_read(struct pa_ioline *l) {
    ssize_t r;
    size_t m, len;
    char *e;
    assert(l);

    if (!pa_iochannel_is_readable(l->io))
        return 0;

    len = l->rbuf_length - l->rbuf_index - l->rbuf_valid_length;

    if (len < READ_SIZE) {
        size_t n = l->rbuf_valid_length+READ_SIZE;

        if (n >= BUFFER_LIMIT)
            n = BUFFER_LIMIT;
        
        if (l->rbuf_length >= n) {
            if (l->rbuf_valid_length)
                memmove(l->rbuf, l->rbuf+l->rbuf_index, l->rbuf_valid_length);
        } else {
            char *new = pa_xmalloc(n);
            if (l->rbuf_valid_length)
                memcpy(new, l->rbuf+l->rbuf_index, l->rbuf_valid_length);
            pa_xfree(l->rbuf);
            l->rbuf = new;
            l->rbuf_length = n;
        }
        
        l->rbuf_index = 0;
    }

    len = l->rbuf_length - l->rbuf_index - l->rbuf_valid_length;
    
    if ((r = pa_iochannel_read(l->io, l->rbuf+l->rbuf_index+l->rbuf_valid_length, len)) <= 0)
        return -1;

    e = memchr(l->rbuf+l->rbuf_index+l->rbuf_valid_length, '\n', r);
    l->rbuf_valid_length += r;

    if (!e &&l->rbuf_valid_length >= BUFFER_LIMIT)
        e = l->rbuf+BUFFER_LIMIT-1;

    if (e) {
        char *p;
        
        *e = 0;
    
        p = l->rbuf+l->rbuf_index;
        m = strlen(p);

        l->rbuf_index += m+1;
        l->rbuf_valid_length -= m+1;

        if (l->rbuf_valid_length == 0)
            l->rbuf_index = 0;

        if (l->callback)
            l->callback(l, p, l->userdata);
    }

    return 0;
}

static int do_write(struct pa_ioline *l) {
    ssize_t r;
    assert(l);

    if (!l->wbuf_valid_length || !pa_iochannel_is_writable(l->io))
        return 0;
    
    if ((r = pa_iochannel_write(l->io, l->wbuf+l->wbuf_index, l->wbuf_valid_length)) < 0)
        return -1;

    l->wbuf_valid_length -= r;
    if (l->wbuf_valid_length == 0)
        l->wbuf_index = 0;

    return 0;
}

static void io_callback(struct pa_iochannel*io, void *userdata) {
    struct pa_ioline *l = userdata;
    assert(io && l);
    
    if (!l->dead && do_write(l) < 0)
        goto fail;
    
    if (!l->dead && do_read(l) < 0)
        goto fail;
    
    return;

fail:
    l->dead = 1;
    if (l->callback)
        l->callback(l, NULL, l->userdata);
}
