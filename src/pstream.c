#include <stdlib.h>
#include <assert.h>
#include <netinet/in.h>

#include "pstream.h"
#include "queue.h"

enum pstream_descriptor_index {
    PSTREAM_DESCRIPTOR_LENGTH,
    PSTREAM_DESCRIPTOR_CHANNEL,
    PSTREAM_DESCRIPTOR_DELTA,
    PSTREAM_DESCRIPTOR_MAX
};

typedef uint32_t pstream_descriptor[PSTREAM_DESCRIPTOR_MAX];

#define PSTREAM_DESCRIPTOR_SIZE (PSTREAM_DESCRIPTOR_MAX*sizeof(uint32_t))
#define FRAME_SIZE_MAX (1024*64)

struct item_info {
    enum { PSTREAM_ITEM_PACKET, PSTREAM_ITEM_MEMBLOCK } type;

    /* memblock info */
    struct memchunk chunk;
    uint32_t channel;
    int32_t delta;

    /* packet info */
    struct packet *packet;
};

struct pstream {
    struct pa_mainloop_api *mainloop;
    struct mainloop_source *mainloop_source;
    struct iochannel *io;
    struct queue *send_queue;

    int dead;
    void (*die_callback) (struct pstream *p, void *userdad);
    void *die_callback_userdata;

    struct {
        struct item_info* current;
        pstream_descriptor descriptor;
        void *data;
        size_t index;
    } write;

    void (*send_callback) (struct pstream *p, void *userdata);
    void *send_callback_userdata;

    struct {
        struct memblock *memblock;
        struct packet *packet;
        pstream_descriptor descriptor;
        void *data;
        size_t index;
    } read;

    int (*recieve_packet_callback) (struct pstream *p, struct packet *packet, void *userdata);
    void *recieve_packet_callback_userdata;

    int (*recieve_memblock_callback) (struct pstream *p, uint32_t channel, int32_t delta, struct memchunk *chunk, void *userdata);
    void *recieve_memblock_callback_userdata;
};

static void do_write(struct pstream *p);
static void do_read(struct pstream *p);

static void io_callback(struct iochannel*io, void *userdata) {
    struct pstream *p = userdata;
    assert(p && p->io == io);

    p->mainloop->enable_fixed(p->mainloop, p->mainloop_source, 0);
    
    do_write(p);
    do_read(p);
}

static void fixed_callback(struct pa_mainloop_api *m, void *id, void*userdata) {
    struct pstream *p = userdata;
    assert(p && p->mainloop_source == id && p->mainloop == m);

    p->mainloop->enable_fixed(p->mainloop, p->mainloop_source, 0);
    
    do_write(p);
    do_read(p);
}

struct pstream *pstream_new(struct pa_mainloop_api *m, struct iochannel *io) {
    struct pstream *p;
    assert(io);

    p = malloc(sizeof(struct pstream));
    assert(p);

    p->io = io;
    iochannel_set_callback(io, io_callback, p);

    p->dead = 0;
    p->die_callback = NULL;
    p->die_callback_userdata = NULL;

    p->mainloop = m;
    p->mainloop_source = m->source_fixed(m, fixed_callback, p);
    m->enable_fixed(m, p->mainloop_source, 0);
    
    p->send_queue = queue_new();
    assert(p->send_queue);

    p->write.current = NULL;
    p->write.index = 0;

    p->read.memblock = NULL;
    p->read.packet = NULL;
    p->read.index = 0;

    p->send_callback = NULL;
    p->send_callback_userdata = NULL;

    p->recieve_packet_callback = NULL;
    p->recieve_packet_callback_userdata = NULL;
    
    p->recieve_memblock_callback = NULL;
    p->recieve_memblock_callback_userdata = NULL;

    return p;
}

static void item_free(void *item, void *p) {
    struct item_info *i = item;
    assert(i);

    if (i->type == PSTREAM_ITEM_MEMBLOCK) {
        assert(i->chunk.memblock);
        memblock_unref(i->chunk.memblock);
    } else {
        assert(i->type == PSTREAM_ITEM_PACKET);
        assert(i->packet);
        packet_unref(i->packet);
    }

    free(i);
}

void pstream_free(struct pstream *p) {
    assert(p);

    iochannel_free(p->io);
    queue_free(p->send_queue, item_free, NULL);

    if (p->write.current)
        item_free(p->write.current, NULL);

    if (p->read.memblock)
        memblock_unref(p->read.memblock);
    
    if (p->read.packet)
        packet_unref(p->read.packet);

    p->mainloop->cancel_fixed(p->mainloop, p->mainloop_source);
    free(p);
}

void pstream_set_send_callback(struct pstream*p, void (*callback) (struct pstream *p, void *userdata), void *userdata) {
    assert(p && callback);

    p->send_callback = callback;
    p->send_callback_userdata = userdata;
}

void pstream_send_packet(struct pstream*p, struct packet *packet) {
    struct item_info *i;
    assert(p && packet);

    i = malloc(sizeof(struct item_info));
    assert(i);
    i->type = PSTREAM_ITEM_PACKET;
    i->packet = packet_ref(packet);

    queue_push(p->send_queue, i);
    p->mainloop->enable_fixed(p->mainloop, p->mainloop_source, 1);
}

void pstream_send_memblock(struct pstream*p, uint32_t channel, int32_t delta, struct memchunk *chunk) {
    struct item_info *i;
    assert(p && channel != (uint32_t) -1 && chunk);
    
    i = malloc(sizeof(struct item_info));
    assert(i);
    i->type = PSTREAM_ITEM_MEMBLOCK;
    i->chunk = *chunk;
    i->channel = channel;
    i->delta = delta;

    memblock_ref(i->chunk.memblock);

    queue_push(p->send_queue, i);
    p->mainloop->enable_fixed(p->mainloop, p->mainloop_source, 1);
}

void pstream_set_recieve_packet_callback(struct pstream *p, int (*callback) (struct pstream *p, struct packet *packet, void *userdata), void *userdata) {
    assert(p && callback);

    p->recieve_packet_callback = callback;
    p->recieve_packet_callback_userdata = userdata;
}

void pstream_set_recieve_memblock_callback(struct pstream *p, int (*callback) (struct pstream *p, uint32_t channel, int32_t delta, struct memchunk *chunk, void *userdata), void *userdata) {
    assert(p && callback);

    p->recieve_memblock_callback = callback;
    p->recieve_memblock_callback_userdata = userdata;
}

static void prepare_next_write_item(struct pstream *p) {
    assert(p);

    if (!(p->write.current = queue_pop(p->send_queue)))
        return;
    
    p->write.index = 0;
    
    if (p->write.current->type == PSTREAM_ITEM_PACKET) {
        assert(p->write.current->packet);
        p->write.data = p->write.current->packet->data;
        p->write.descriptor[PSTREAM_DESCRIPTOR_LENGTH] = htonl(p->write.current->packet->length);
        p->write.descriptor[PSTREAM_DESCRIPTOR_CHANNEL] = htonl((uint32_t) -1);
        p->write.descriptor[PSTREAM_DESCRIPTOR_DELTA] = 0;
    } else {
        assert(p->write.current->type == PSTREAM_ITEM_MEMBLOCK && p->write.current->chunk.memblock);
        p->write.data = p->write.current->chunk.memblock->data + p->write.current->chunk.index;
        p->write.descriptor[PSTREAM_DESCRIPTOR_LENGTH] = htonl(p->write.current->chunk.length);
        p->write.descriptor[PSTREAM_DESCRIPTOR_CHANNEL] = htonl(p->write.current->channel);
        p->write.descriptor[PSTREAM_DESCRIPTOR_DELTA] = htonl(p->write.current->delta);
    }
}

static void do_write(struct pstream *p) {
    void *d;
    size_t l;
    ssize_t r;
    assert(p);

    if (p->dead || !iochannel_is_writable(p->io))
        return;
    
    if (!p->write.current)
        prepare_next_write_item(p);

    if (!p->write.current)
        return;

    assert(p->write.data);

    if (p->write.index < PSTREAM_DESCRIPTOR_SIZE) {
        d = (void*) p->write.descriptor + p->write.index;
        l = PSTREAM_DESCRIPTOR_SIZE - p->write.index;
    } else {
        d = (void*) p->write.data + p->write.index - PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->write.descriptor[PSTREAM_DESCRIPTOR_LENGTH]) - (p->write.index - PSTREAM_DESCRIPTOR_SIZE);
    }

    if ((r = iochannel_write(p->io, d, l)) < 0) 
        goto die;

    p->write.index += r;

    if (p->write.index >= PSTREAM_DESCRIPTOR_SIZE+ntohl(p->write.descriptor[PSTREAM_DESCRIPTOR_LENGTH])) {
        assert(p->write.current);
        item_free(p->write.current, (void *) 1);
        p->write.current = NULL;

        if (p->send_callback && queue_is_empty(p->send_queue))
            p->send_callback(p, p->send_callback_userdata);
    }

    return;
    
die:
    p->dead = 1;
    if (p->die_callback)
        p->die_callback(p, p->die_callback_userdata);
}

static void do_read(struct pstream *p) {
    void *d;
    size_t l;
    ssize_t r;
    assert(p);

    if (p->dead || !iochannel_is_readable(p->io))
        return;

    if (p->read.index < PSTREAM_DESCRIPTOR_SIZE) {
        d = (void*) p->read.descriptor + p->read.index;
        l = PSTREAM_DESCRIPTOR_SIZE - p->read.index;
    } else {
        assert(p->read.data);
        d = (void*) p->read.data + p->read.index - PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->read.descriptor[PSTREAM_DESCRIPTOR_LENGTH]) - (p->read.index - PSTREAM_DESCRIPTOR_SIZE);
    }

    if ((r = iochannel_read(p->io, d, l)) <= 0)
        goto die;
    
    p->read.index += r;

    if (p->read.index == PSTREAM_DESCRIPTOR_SIZE) {
        /* Reading of frame descriptor complete */

        /* Frame size too large */
        if (ntohl(p->read.descriptor[PSTREAM_DESCRIPTOR_LENGTH]) > FRAME_SIZE_MAX)
            goto die;
        
        assert(!p->read.packet && !p->read.memblock);

        if (ntohl(p->read.descriptor[PSTREAM_DESCRIPTOR_CHANNEL]) == (uint32_t) -1) {
            /* Frame is a packet frame */
            p->read.packet = packet_new(ntohl(p->read.descriptor[PSTREAM_DESCRIPTOR_LENGTH]));
            assert(p->read.packet);
            p->read.data = p->read.packet->data;
        } else {
            /* Frame is a memblock frame */
            p->read.memblock = memblock_new(ntohl(p->read.descriptor[PSTREAM_DESCRIPTOR_LENGTH]));
            assert(p->read.memblock);
            p->read.data = p->read.memblock->data;
        }
            
    } else if (p->read.index > PSTREAM_DESCRIPTOR_SIZE) {
        /* Frame payload available */
        
        if (p->read.memblock && p->recieve_memblock_callback) { /* Is this memblock data? Than pass it to the user */
            size_t l;

            l = (p->read.index - r) < PSTREAM_DESCRIPTOR_SIZE ? p->read.index - PSTREAM_DESCRIPTOR_SIZE : (size_t) r;
                
            if (l > 0) {
                struct memchunk chunk;
                
                chunk.memblock = p->read.memblock;
                chunk.index = p->read.index - PSTREAM_DESCRIPTOR_SIZE - l;
                chunk.length = l;
                
                if (p->recieve_memblock_callback(p,
                                                 ntohl(p->read.descriptor[PSTREAM_DESCRIPTOR_CHANNEL]),
                                                 (int32_t) ntohl(p->read.descriptor[PSTREAM_DESCRIPTOR_DELTA]),
                                                 &chunk,
                                                 p->recieve_memblock_callback_userdata) < 0)
                    goto die;
            }
        }

        /* Frame complete */
        if (p->read.index >= ntohl(p->read.descriptor[PSTREAM_DESCRIPTOR_LENGTH]) + PSTREAM_DESCRIPTOR_SIZE) {
            if (p->read.memblock) {
                assert(!p->read.packet);
                
                memblock_unref(p->read.memblock);
                p->read.memblock = NULL;
            } else {
                int r = 0;
                assert(p->read.packet);

                if (p->recieve_packet_callback)
                    r = p->recieve_packet_callback(p, p->read.packet, p->recieve_packet_callback_userdata);

                packet_unref(p->read.packet);
                p->read.packet = NULL;

                if (r < 0)
                    goto die;
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

void pstream_set_die_callback(struct pstream *p, void (*callback)(struct pstream *p, void *userdata), void *userdata) {
    assert(p && callback);
    p->die_callback = callback;
    p->die_callback_userdata = userdata;
}
