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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <assert.h>
#include <unistd.h>

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include "winsock.h"

#include <polypcore/queue.h>
#include <polypcore/xmalloc.h>
#include <polypcore/log.h>

#include "pstream.h"

enum {
    PA_PSTREAM_DESCRIPTOR_LENGTH,
    PA_PSTREAM_DESCRIPTOR_CHANNEL,
    PA_PSTREAM_DESCRIPTOR_OFFSET_HI,
    PA_PSTREAM_DESCRIPTOR_OFFSET_LO,
    PA_PSTREAM_DESCRIPTOR_SEEK,
    PA_PSTREAM_DESCRIPTOR_MAX
};

typedef uint32_t pa_pstream_descriptor[PA_PSTREAM_DESCRIPTOR_MAX];

#define PA_PSTREAM_DESCRIPTOR_SIZE (PA_PSTREAM_DESCRIPTOR_MAX*sizeof(uint32_t))
#define FRAME_SIZE_MAX (1024*500) /* half a megabyte */

struct item_info {
    enum { PA_PSTREAM_ITEM_PACKET, PA_PSTREAM_ITEM_MEMBLOCK } type;

    /* memblock info */
    pa_memchunk chunk;
    uint32_t channel;
    int64_t offset;
    pa_seek_mode_t seek_mode;

    /* packet info */
    pa_packet *packet;
#ifdef SCM_CREDENTIALS
    int with_creds;
#endif
};

struct pa_pstream {
    int ref;
    
    pa_mainloop_api *mainloop;
    pa_defer_event *defer_event;
    pa_iochannel *io;
    pa_queue *send_queue;

    int dead;

    struct {
        struct item_info* current;
        pa_pstream_descriptor descriptor;
        void *data;
        size_t index;
    } write;

    struct {
        pa_memblock *memblock;
        pa_packet *packet;
        pa_pstream_descriptor descriptor;
        void *data;
        size_t index;
    } read;

    pa_pstream_packet_cb_t recieve_packet_callback;
    void *recieve_packet_callback_userdata;

    pa_pstream_memblock_cb_t recieve_memblock_callback;
    void *recieve_memblock_callback_userdata;

    pa_pstream_notify_cb_t drain_callback;
    void *drain_callback_userdata;

    pa_pstream_notify_cb_t die_callback;
    void *die_callback_userdata;

    pa_memblock_stat *memblock_stat;

#ifdef SCM_CREDENTIALS
    int send_creds_now;
    struct ucred ucred;
    int creds_valid;
#endif
};

static int do_write(pa_pstream *p);
static int do_read(pa_pstream *p);

static void do_something(pa_pstream *p) {
    assert(p);

    p->mainloop->defer_enable(p->defer_event, 0);

    pa_pstream_ref(p);

    if (!p->dead && pa_iochannel_is_readable(p->io)) {
        if (do_read(p) < 0)
            goto fail;
    } else if (!p->dead && pa_iochannel_is_hungup(p->io))
        goto fail;

    if (!p->dead && pa_iochannel_is_writable(p->io)) {
        if (do_write(p) < 0)
            goto fail;
    }

    pa_pstream_unref(p);
    return;

fail:

    p->dead = 1;
    
    if (p->die_callback)
        p->die_callback(p, p->die_callback_userdata);
    
    pa_pstream_unref(p);
}

static void io_callback(pa_iochannel*io, void *userdata) {
    pa_pstream *p = userdata;
    
    assert(p);
    assert(p->io == io);
    
    do_something(p);
}

static void defer_callback(pa_mainloop_api *m, pa_defer_event *e, void*userdata) {
    pa_pstream *p = userdata;

    assert(p);
    assert(p->defer_event == e);
    assert(p->mainloop == m);
    
    do_something(p);
}

pa_pstream *pa_pstream_new(pa_mainloop_api *m, pa_iochannel *io, pa_memblock_stat *s) {
    pa_pstream *p;
    assert(io);

    p = pa_xnew(pa_pstream, 1);
    
    p->ref = 1;
    p->io = io;
    pa_iochannel_set_callback(io, io_callback, p);

    p->dead = 0;

    p->mainloop = m;
    p->defer_event = m->defer_new(m, defer_callback, p);
    m->defer_enable(p->defer_event, 0);
    
    p->send_queue = pa_queue_new();
    assert(p->send_queue);

    p->write.current = NULL;
    p->write.index = 0;

    p->read.memblock = NULL;
    p->read.packet = NULL;
    p->read.index = 0;

    p->recieve_packet_callback = NULL;
    p->recieve_packet_callback_userdata = NULL;
    
    p->recieve_memblock_callback = NULL;
    p->recieve_memblock_callback_userdata = NULL;

    p->drain_callback = NULL;
    p->drain_callback_userdata = NULL;

    p->die_callback = NULL;
    p->die_callback_userdata = NULL;

    p->memblock_stat = s;

    pa_iochannel_socket_set_rcvbuf(io, 1024*8); 
    pa_iochannel_socket_set_sndbuf(io, 1024*8);

#ifdef SCM_CREDENTIALS
    p->send_creds_now = 0;
    p->creds_valid = 0;
#endif
    return p;
}

static void item_free(void *item, PA_GCC_UNUSED void *p) {
    struct item_info *i = item;
    assert(i);

    if (i->type == PA_PSTREAM_ITEM_MEMBLOCK) {
        assert(i->chunk.memblock);
        pa_memblock_unref(i->chunk.memblock);
    } else {
        assert(i->type == PA_PSTREAM_ITEM_PACKET);
        assert(i->packet);
        pa_packet_unref(i->packet);
    }

    pa_xfree(i);
}

static void pstream_free(pa_pstream *p) {
    assert(p);

    pa_pstream_close(p);
    
    pa_queue_free(p->send_queue, item_free, NULL);

    if (p->write.current)
        item_free(p->write.current, NULL);

    if (p->read.memblock)
        pa_memblock_unref(p->read.memblock);
    
    if (p->read.packet)
        pa_packet_unref(p->read.packet);

    pa_xfree(p);
}

void pa_pstream_send_packet(pa_pstream*p, pa_packet *packet, int with_creds) {
    struct item_info *i;
    assert(p && packet && p->ref >= 1);

    if (p->dead)
        return;
    
/*     pa_log(__FILE__": push-packet %p", packet); */
    
    i = pa_xnew(struct item_info, 1);
    i->type = PA_PSTREAM_ITEM_PACKET;
    i->packet = pa_packet_ref(packet);
#ifdef SCM_CREDENTIALS
    i->with_creds = with_creds;
#endif

    pa_queue_push(p->send_queue, i);
    p->mainloop->defer_enable(p->defer_event, 1);
}

void pa_pstream_send_memblock(pa_pstream*p, uint32_t channel, int64_t offset, pa_seek_mode_t seek_mode, const pa_memchunk *chunk) {
    struct item_info *i;
    assert(p && channel != (uint32_t) -1 && chunk && p->ref >= 1);

    if (p->dead)
        return;
    
/*     pa_log(__FILE__": push-memblock %p", chunk); */
    
    i = pa_xnew(struct item_info, 1);
    i->type = PA_PSTREAM_ITEM_MEMBLOCK;
    i->chunk = *chunk;
    i->channel = channel;
    i->offset = offset;
    i->seek_mode = seek_mode;

    pa_memblock_ref(i->chunk.memblock);

    pa_queue_push(p->send_queue, i);
    p->mainloop->defer_enable(p->defer_event, 1);
}

static void prepare_next_write_item(pa_pstream *p) {
    assert(p);

    if (!(p->write.current = pa_queue_pop(p->send_queue)))
        return;
    
    p->write.index = 0;
    
    if (p->write.current->type == PA_PSTREAM_ITEM_PACKET) {
        /*pa_log(__FILE__": pop-packet %p", p->write.current->packet);*/
        
        assert(p->write.current->packet);
        p->write.data = p->write.current->packet->data;
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl(p->write.current->packet->length);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL] = htonl((uint32_t) -1);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = 0;
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO] = 0;
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_SEEK] = 0;

#ifdef SCM_CREDENTIALS
        p->send_creds_now = 1;
#endif
        
    } else {
        assert(p->write.current->type == PA_PSTREAM_ITEM_MEMBLOCK && p->write.current->chunk.memblock);
        p->write.data = (uint8_t*) p->write.current->chunk.memblock->data + p->write.current->chunk.index;
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl(p->write.current->chunk.length);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL] = htonl(p->write.current->channel);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = htonl((uint32_t) (((uint64_t) p->write.current->offset) >> 32));
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO] = htonl((uint32_t) ((uint64_t) p->write.current->offset));
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_SEEK] = htonl(p->write.current->seek_mode);

#ifdef SCM_CREDENTIALS
        p->send_creds_now = 1;
#endif
    }
}

static int do_write(pa_pstream *p) {
    void *d;
    size_t l;
    ssize_t r;
    assert(p);

    if (!p->write.current)
        prepare_next_write_item(p);

    if (!p->write.current)
        return 0;

    assert(p->write.data);

    if (p->write.index < PA_PSTREAM_DESCRIPTOR_SIZE) {
        d = (uint8_t*) p->write.descriptor + p->write.index;
        l = PA_PSTREAM_DESCRIPTOR_SIZE - p->write.index;
    } else {
        d = (uint8_t*) p->write.data + p->write.index - PA_PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) - (p->write.index - PA_PSTREAM_DESCRIPTOR_SIZE);
    }

#ifdef SCM_CREDENTIALS
    if (p->send_creds_now) {

        if ((r = pa_iochannel_write_with_creds(p->io, d, l)) < 0)
            return -1;

        p->send_creds_now = 0;
    } else
#endif

    if ((r = pa_iochannel_write(p->io, d, l)) < 0)
        return -1;

    p->write.index += r;

    if (p->write.index >= PA_PSTREAM_DESCRIPTOR_SIZE+ntohl(p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH])) {
        assert(p->write.current);
        item_free(p->write.current, (void *) 1);
        p->write.current = NULL;

        if (p->drain_callback && !pa_pstream_is_pending(p))
            p->drain_callback(p, p->drain_callback_userdata);
    }

    return 0;
}

static int do_read(pa_pstream *p) {
    void *d;
    size_t l; 
    ssize_t r;
    assert(p);

    if (p->read.index < PA_PSTREAM_DESCRIPTOR_SIZE) {
        d = (uint8_t*) p->read.descriptor + p->read.index;
        l = PA_PSTREAM_DESCRIPTOR_SIZE - p->read.index;
    } else {
        assert(p->read.data);
        d = (uint8_t*) p->read.data + p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) - (p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE);
    }

#ifdef SCM_CREDENTIALS
    {
        int b;
        
        if ((r = pa_iochannel_read_with_creds(p->io, d, l, &p->ucred, &b)) <= 0)
            return -1;

        p->creds_valid = p->creds_valid || b;
    }
#else
    if ((r = pa_iochannel_read(p->io, d, l)) <= 0)
        return -1;
#endif
    
    p->read.index += r;

    if (p->read.index == PA_PSTREAM_DESCRIPTOR_SIZE) {
        /* Reading of frame descriptor complete */

        /* Frame size too large */
        if (ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) > FRAME_SIZE_MAX) {
            pa_log_warn(__FILE__": Frame size too large");
            return -1;
        }
        
        assert(!p->read.packet && !p->read.memblock);

        if (ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL]) == (uint32_t) -1) {
            /* Frame is a packet frame */
            p->read.packet = pa_packet_new(ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]));
            p->read.data = p->read.packet->data;
        } else {
            /* Frame is a memblock frame */
            p->read.memblock = pa_memblock_new(ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]), p->memblock_stat);
            p->read.data = p->read.memblock->data;

            if (ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_SEEK]) > PA_SEEK_RELATIVE_END) {
                pa_log_warn(__FILE__": Invalid seek mode");
                return -1;
            }
        }
            
    } else if (p->read.index > PA_PSTREAM_DESCRIPTOR_SIZE) {
        /* Frame payload available */
        
        if (p->read.memblock && p->recieve_memblock_callback) { /* Is this memblock data? Than pass it to the user */
            l = (p->read.index - r) < PA_PSTREAM_DESCRIPTOR_SIZE ? p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE : (size_t) r;
                
            if (l > 0) {
                pa_memchunk chunk;
                
                chunk.memblock = p->read.memblock;
                chunk.index = p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE - l;
                chunk.length = l;

                if (p->recieve_memblock_callback) {
                    int64_t offset;

                    offset = (int64_t) (
                            (((uint64_t) ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI])) << 32) |
                            (((uint64_t) ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO]))));
                    
                    p->recieve_memblock_callback(
                        p,
                        ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL]),
                        offset,
                        ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_SEEK]),
                        &chunk,
                        p->recieve_memblock_callback_userdata);
                }

                /* Drop seek info for following callbacks */
                p->read.descriptor[PA_PSTREAM_DESCRIPTOR_SEEK] =
                    p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] =
                    p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO] = 0;
            }
        }

        /* Frame complete */
        if (p->read.index >= ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) + PA_PSTREAM_DESCRIPTOR_SIZE) {
            if (p->read.memblock) {
                assert(!p->read.packet);
                
                pa_memblock_unref(p->read.memblock);
                p->read.memblock = NULL;
            } else {
                assert(p->read.packet);
                
                if (p->recieve_packet_callback)
#ifdef SCM_CREDENTIALS                    
                    p->recieve_packet_callback(p, p->read.packet, p->creds_valid ? &p->ucred : NULL, p->recieve_packet_callback_userdata);
#else
                    p->recieve_packet_callback(p, p->read.packet, NULL, p->recieve_packet_callback_userdata);
#endif

                pa_packet_unref(p->read.packet);
                p->read.packet = NULL;
            }

            p->read.index = 0;
#ifdef SCM_CREDENTIALS
            p->creds_valid = 0;
#endif
        }
    }

    return 0;   
}

void pa_pstream_set_die_callback(pa_pstream *p, pa_pstream_notify_cb_t cb, void *userdata) {
    assert(p);
    assert(p->ref >= 1);

    p->die_callback = cb;
    p->die_callback_userdata = userdata;
}


void pa_pstream_set_drain_callback(pa_pstream *p, pa_pstream_notify_cb_t cb, void *userdata) {
    assert(p);
    assert(p->ref >= 1);

    p->drain_callback = cb;
    p->drain_callback_userdata = userdata;
}

void pa_pstream_set_recieve_packet_callback(pa_pstream *p, pa_pstream_packet_cb_t cb, void *userdata) {
    assert(p);
    assert(p->ref >= 1);

    p->recieve_packet_callback = cb;
    p->recieve_packet_callback_userdata = userdata;
}

void pa_pstream_set_recieve_memblock_callback(pa_pstream *p, pa_pstream_memblock_cb_t cb, void *userdata) {
    assert(p);
    assert(p->ref >= 1);

    p->recieve_memblock_callback = cb;
    p->recieve_memblock_callback_userdata = userdata;
}

int pa_pstream_is_pending(pa_pstream *p) {
    assert(p);

    if (p->dead)
        return 0;

    return p->write.current || !pa_queue_is_empty(p->send_queue);
}

void pa_pstream_unref(pa_pstream*p) {
    assert(p);
    assert(p->ref >= 1);

    if (--p->ref == 0)
        pstream_free(p);
}

pa_pstream* pa_pstream_ref(pa_pstream*p) {
    assert(p);
    assert(p->ref >= 1);
    
    p->ref++;
    return p;
}

void pa_pstream_close(pa_pstream *p) {
    assert(p);

    p->dead = 1;

    if (p->io) {
        pa_iochannel_free(p->io);
        p->io = NULL;
    }

    if (p->defer_event) {
        p->mainloop->defer_free(p->defer_event);
        p->defer_event = NULL;
    }

    p->die_callback = NULL;
    p->drain_callback = NULL;
    p->recieve_packet_callback = NULL;
    p->recieve_memblock_callback = NULL;
}
