#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <assert.h>
#include <netinet/in.h>

#include "pstream.h"
#include "queue.h"

enum pa_pstream_descriptor_index {
    PA_PSTREAM_DESCRIPTOR_LENGTH,
    PA_PSTREAM_DESCRIPTOR_CHANNEL,
    PA_PSTREAM_DESCRIPTOR_DELTA,
    PA_PSTREAM_DESCRIPTOR_MAX
};

typedef uint32_t pa_pstream_descriptor[PA_PSTREAM_DESCRIPTOR_MAX];

#define PA_PSTREAM_DESCRIPTOR_SIZE (PA_PSTREAM_DESCRIPTOR_MAX*sizeof(uint32_t))
#define FRAME_SIZE_MAX (1024*64)

struct item_info {
    enum { PA_PSTREAM_ITEM_PACKET, PA_PSTREAM_ITEM_MEMBLOCK } type;

    /* memblock info */
    struct pa_memchunk chunk;
    uint32_t channel;
    int32_t delta;

    /* packet info */
    struct pa_packet *packet;
};

struct pa_pstream {
    struct pa_mainloop_api *mainloop;
    struct mainloop_source *mainloop_source;
    struct pa_iochannel *io;
    struct pa_queue *send_queue;

    int in_use, shall_free;
    
    int dead;
    void (*die_callback) (struct pa_pstream *p, void *userdata);
    void *die_callback_userdata;

    struct {
        struct item_info* current;
        pa_pstream_descriptor descriptor;
        void *data;
        size_t index;
    } write;

    struct {
        struct pa_memblock *memblock;
        struct pa_packet *packet;
        pa_pstream_descriptor descriptor;
        void *data;
        size_t index;
    } read;

    void (*recieve_packet_callback) (struct pa_pstream *p, struct pa_packet *packet, void *userdata);
    void *recieve_packet_callback_userdata;

    void (*recieve_memblock_callback) (struct pa_pstream *p, uint32_t channel, int32_t delta, const struct pa_memchunk *chunk, void *userdata);
    void *recieve_memblock_callback_userdata;

    void (*drain_callback)(struct pa_pstream *p, void *userdata);
    void *drain_userdata;
};

static void do_write(struct pa_pstream *p);
static void do_read(struct pa_pstream *p);

static void do_something(struct pa_pstream *p) {
    assert(p && !p->shall_free);
    p->mainloop->enable_fixed(p->mainloop, p->mainloop_source, 0);

    if (p->dead)
        return;

    if (pa_iochannel_is_hungup(p->io)) {
        p->dead = 1;
        if (p->die_callback)
            p->die_callback(p, p->die_callback_userdata);

        return;
    }

    if (pa_iochannel_is_writable(p->io)) {
        p->in_use = 1;
        do_write(p);
        p->in_use = 0;

        if (p->shall_free) {
            pa_pstream_free(p);
            return;
        }
    }

    if (pa_iochannel_is_readable(p->io)) {
        p->in_use = 1;
        do_read(p);
        p->in_use = 0;
        if (p->shall_free) {
            pa_pstream_free(p);
            return;
        }
    }
}

static void io_callback(struct pa_iochannel*io, void *userdata) {
    struct pa_pstream *p = userdata;
    assert(p && p->io == io);
    do_something(p);
}

static void fixed_callback(struct pa_mainloop_api *m, void *id, void*userdata) {
    struct pa_pstream *p = userdata;
    assert(p && p->mainloop_source == id && p->mainloop == m);
    do_something(p);
}

struct pa_pstream *pa_pstream_new(struct pa_mainloop_api *m, struct pa_iochannel *io) {
    struct pa_pstream *p;
    assert(io);

    p = malloc(sizeof(struct pa_pstream));
    assert(p);

    p->io = io;
    pa_iochannel_set_callback(io, io_callback, p);

    p->dead = 0;
    p->die_callback = NULL;
    p->die_callback_userdata = NULL;

    p->mainloop = m;
    p->mainloop_source = m->source_fixed(m, fixed_callback, p);
    m->enable_fixed(m, p->mainloop_source, 0);
    
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
    p->drain_userdata = NULL;

    p->in_use = p->shall_free = 0;

    return p;
}

static void item_free(void *item, void *p) {
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

    free(i);
}

void pa_pstream_free(struct pa_pstream *p) {
    assert(p);

    if (p->in_use) {
        /* If this pstream object is used by someone else on the call stack, we have to postpone the freeing */
        p->dead = p->shall_free = 1;
        return;
    }

    pa_iochannel_free(p->io);
    pa_queue_free(p->send_queue, item_free, NULL);

    if (p->write.current)
        item_free(p->write.current, NULL);

    if (p->read.memblock)
        pa_memblock_unref(p->read.memblock);
    
    if (p->read.packet)
        pa_packet_unref(p->read.packet);

    p->mainloop->cancel_fixed(p->mainloop, p->mainloop_source);
    free(p);
}

void pa_pstream_send_packet(struct pa_pstream*p, struct pa_packet *packet) {
    struct item_info *i;
    assert(p && packet);

    i = malloc(sizeof(struct item_info));
    assert(i);
    i->type = PA_PSTREAM_ITEM_PACKET;
    i->packet = pa_packet_ref(packet);

    pa_queue_push(p->send_queue, i);
    p->mainloop->enable_fixed(p->mainloop, p->mainloop_source, 1);
}

void pa_pstream_send_memblock(struct pa_pstream*p, uint32_t channel, int32_t delta, const struct pa_memchunk *chunk) {
    struct item_info *i;
    assert(p && channel != (uint32_t) -1 && chunk);
    
    i = malloc(sizeof(struct item_info));
    assert(i);
    i->type = PA_PSTREAM_ITEM_MEMBLOCK;
    i->chunk = *chunk;
    i->channel = channel;
    i->delta = delta;

    pa_memblock_ref(i->chunk.memblock);

    pa_queue_push(p->send_queue, i);
    p->mainloop->enable_fixed(p->mainloop, p->mainloop_source, 1);
}

void pa_pstream_set_recieve_packet_callback(struct pa_pstream *p, void (*callback) (struct pa_pstream *p, struct pa_packet *packet, void *userdata), void *userdata) {
    assert(p && callback);

    p->recieve_packet_callback = callback;
    p->recieve_packet_callback_userdata = userdata;
}

void pa_pstream_set_recieve_memblock_callback(struct pa_pstream *p, void (*callback) (struct pa_pstream *p, uint32_t channel, int32_t delta, const struct pa_memchunk *chunk, void *userdata), void *userdata) {
    assert(p && callback);

    p->recieve_memblock_callback = callback;
    p->recieve_memblock_callback_userdata = userdata;
}

static void prepare_next_write_item(struct pa_pstream *p) {
    assert(p);

    if (!(p->write.current = pa_queue_pop(p->send_queue)))
        return;
    
    p->write.index = 0;
    
    if (p->write.current->type == PA_PSTREAM_ITEM_PACKET) {
        assert(p->write.current->packet);
        p->write.data = p->write.current->packet->data;
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl(p->write.current->packet->length);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL] = htonl((uint32_t) -1);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_DELTA] = 0;
    } else {
        assert(p->write.current->type == PA_PSTREAM_ITEM_MEMBLOCK && p->write.current->chunk.memblock);
        p->write.data = p->write.current->chunk.memblock->data + p->write.current->chunk.index;
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl(p->write.current->chunk.length);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL] = htonl(p->write.current->channel);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_DELTA] = htonl(p->write.current->delta);
    }
}

static void do_write(struct pa_pstream *p) {
    void *d;
    size_t l;
    ssize_t r;
    assert(p);

    if (!p->write.current)
        prepare_next_write_item(p);

    if (!p->write.current)
        return;

    assert(p->write.data);

    if (p->write.index < PA_PSTREAM_DESCRIPTOR_SIZE) {
        d = (void*) p->write.descriptor + p->write.index;
        l = PA_PSTREAM_DESCRIPTOR_SIZE - p->write.index;
    } else {
        d = (void*) p->write.data + p->write.index - PA_PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) - (p->write.index - PA_PSTREAM_DESCRIPTOR_SIZE);
    }

    if ((r = pa_iochannel_write(p->io, d, l)) < 0)
        goto die;

    p->write.index += r;

    if (p->write.index >= PA_PSTREAM_DESCRIPTOR_SIZE+ntohl(p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH])) {
        assert(p->write.current);
        item_free(p->write.current, (void *) 1);
        p->write.current = NULL;

        if (p->drain_callback && !pa_pstream_is_pending(p))
            p->drain_callback(p, p->drain_userdata);
    }

    return;
    
die:
    p->dead = 1;
    if (p->die_callback)
        p->die_callback(p, p->die_callback_userdata);
}

static void do_read(struct pa_pstream *p) {
    void *d;
    size_t l;
    ssize_t r;
    assert(p);

    if (p->read.index < PA_PSTREAM_DESCRIPTOR_SIZE) {
        d = (void*) p->read.descriptor + p->read.index;
        l = PA_PSTREAM_DESCRIPTOR_SIZE - p->read.index;
    } else {
        assert(p->read.data);
        d = (void*) p->read.data + p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) - (p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE);
    }

    if ((r = pa_iochannel_read(p->io, d, l)) <= 0)
        goto die;
    
    p->read.index += r;

    if (p->read.index == PA_PSTREAM_DESCRIPTOR_SIZE) {
        /* Reading of frame descriptor complete */

        /* Frame size too large */
        if (ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) > FRAME_SIZE_MAX)
            goto die;
        
        assert(!p->read.packet && !p->read.memblock);

        if (ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL]) == (uint32_t) -1) {
            /* Frame is a packet frame */
            p->read.packet = pa_packet_new(ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]));
            assert(p->read.packet);
            p->read.data = p->read.packet->data;
        } else {
            /* Frame is a memblock frame */
            p->read.memblock = pa_memblock_new(ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]));
            assert(p->read.memblock);
            p->read.data = p->read.memblock->data;
        }
            
    } else if (p->read.index > PA_PSTREAM_DESCRIPTOR_SIZE) {
        /* Frame payload available */
        
        if (p->read.memblock && p->recieve_memblock_callback) { /* Is this memblock data? Than pass it to the user */
            size_t l;

            l = (p->read.index - r) < PA_PSTREAM_DESCRIPTOR_SIZE ? p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE : (size_t) r;
                
            if (l > 0) {
                struct pa_memchunk chunk;
                
                chunk.memblock = p->read.memblock;
                chunk.index = p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE - l;
                chunk.length = l;

                if (p->recieve_memblock_callback)
                    p->recieve_memblock_callback(
                        p,
                        ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL]),
                        (int32_t) ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_DELTA]),
                        &chunk,
                        p->recieve_memblock_callback_userdata);
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
                    p->recieve_packet_callback(p, p->read.packet, p->recieve_packet_callback_userdata);

                pa_packet_unref(p->read.packet);
                p->read.packet = NULL;
            }

            p->read.index = 0;
        }
    }

    return;

die:
    p->dead = 1;
    if (p->die_callback)
        p->die_callback(p, p->die_callback_userdata);
   
}

void pa_pstream_set_die_callback(struct pa_pstream *p, void (*callback)(struct pa_pstream *p, void *userdata), void *userdata) {
    assert(p && callback);
    p->die_callback = callback;
    p->die_callback_userdata = userdata;
}

int pa_pstream_is_pending(struct pa_pstream *p) {
    assert(p);

    if (p->dead)
        return 0;

    return p->write.current || !pa_queue_is_empty(p->send_queue);
}

void pa_pstream_set_drain_callback(struct pa_pstream *p, void (*cb)(struct pa_pstream *p, void *userdata), void *userdata) {
    assert(p);

    p->drain_callback = cb;
    p->drain_userdata = userdata;
}

