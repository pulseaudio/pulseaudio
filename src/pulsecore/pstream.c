/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, write to the Free Software
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

#ifdef HAVE_SYS_SOCKET_H
#include <sys/socket.h>
#endif
#ifdef HAVE_SYS_UN_H
#include <sys/un.h>
#endif
#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#include "winsock.h"

#include <pulse/xmalloc.h>

#include <pulsecore/queue.h>
#include <pulsecore/log.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/creds.h>
#include <pulsecore/mutex.h>
#include <pulsecore/refcnt.h>

#include "pstream.h"

/* We piggyback information if audio data blocks are stored in SHM on the seek mode */
#define PA_FLAG_SHMDATA    0x80000000LU
#define PA_FLAG_SHMRELEASE 0x40000000LU
#define PA_FLAG_SHMREVOKE  0xC0000000LU
#define PA_FLAG_SHMMASK    0xFF000000LU
#define PA_FLAG_SEEKMASK   0x000000FFLU

/* The sequence descriptor header consists of 5 32bit integers: */
enum {
    PA_PSTREAM_DESCRIPTOR_LENGTH,
    PA_PSTREAM_DESCRIPTOR_CHANNEL,
    PA_PSTREAM_DESCRIPTOR_OFFSET_HI,
    PA_PSTREAM_DESCRIPTOR_OFFSET_LO,
    PA_PSTREAM_DESCRIPTOR_FLAGS,
    PA_PSTREAM_DESCRIPTOR_MAX
};

/* If we have an SHM block, this info follows the descriptor */
enum {
    PA_PSTREAM_SHM_BLOCKID,
    PA_PSTREAM_SHM_SHMID,
    PA_PSTREAM_SHM_INDEX,
    PA_PSTREAM_SHM_LENGTH,
    PA_PSTREAM_SHM_MAX
};

typedef uint32_t pa_pstream_descriptor[PA_PSTREAM_DESCRIPTOR_MAX];

#define PA_PSTREAM_DESCRIPTOR_SIZE (PA_PSTREAM_DESCRIPTOR_MAX*sizeof(uint32_t))
#define FRAME_SIZE_MAX_ALLOW PA_SCACHE_ENTRY_SIZE_MAX /* allow uploading a single sample in one frame at max */
#define FRAME_SIZE_MAX_USE (1024*64)

struct item_info {
    enum {
        PA_PSTREAM_ITEM_PACKET,
        PA_PSTREAM_ITEM_MEMBLOCK,
        PA_PSTREAM_ITEM_SHMRELEASE,
        PA_PSTREAM_ITEM_SHMREVOKE
    } type;


    /* packet info */
    pa_packet *packet;
#ifdef HAVE_CREDS
    int with_creds;
    pa_creds creds;
#endif

    /* memblock info */
    pa_memchunk chunk;
    uint32_t channel;
    int64_t offset;
    pa_seek_mode_t seek_mode;

    /* release/revoke info */
    uint32_t block_id;
};

struct pa_pstream {
    PA_REFCNT_DECLARE;
    
    pa_mainloop_api *mainloop;
    pa_defer_event *defer_event;
    pa_iochannel *io;
    pa_queue *send_queue;
    pa_mutex *mutex;

    int dead;

    struct {
        pa_pstream_descriptor descriptor;
        struct item_info* current;
        uint32_t shm_info[PA_PSTREAM_SHM_MAX];
        void *data;
        size_t index;
    } write;

    struct {
        pa_pstream_descriptor descriptor;
        pa_memblock *memblock;
        pa_packet *packet;
        uint32_t shm_info[PA_PSTREAM_SHM_MAX];
        void *data;
        size_t index;
    } read;

    int use_shm;
    pa_memimport *import;
    pa_memexport *export;

    pa_pstream_packet_cb_t recieve_packet_callback;
    void *recieve_packet_callback_userdata;

    pa_pstream_memblock_cb_t recieve_memblock_callback;
    void *recieve_memblock_callback_userdata;

    pa_pstream_notify_cb_t drain_callback;
    void *drain_callback_userdata;

    pa_pstream_notify_cb_t die_callback;
    void *die_callback_userdata;

    pa_mempool *mempool;

#ifdef HAVE_CREDS
    pa_creds read_creds, write_creds;
    int read_creds_valid, send_creds_now;
#endif
};

static int do_write(pa_pstream *p);
static int do_read(pa_pstream *p);

static void do_something(pa_pstream *p) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    pa_pstream_ref(p);

    pa_mutex_lock(p->mutex);
    
    p->mainloop->defer_enable(p->defer_event, 0);

    if (!p->dead && pa_iochannel_is_readable(p->io)) {
        if (do_read(p) < 0)
            goto fail;
    } else if (!p->dead && pa_iochannel_is_hungup(p->io))
        goto fail;

    if (!p->dead && pa_iochannel_is_writable(p->io)) {
        if (do_write(p) < 0)
            goto fail;
    }

    pa_mutex_unlock(p->mutex);

    pa_pstream_unref(p);
    return;

fail:

    p->dead = 1;
    
    if (p->die_callback)
        p->die_callback(p, p->die_callback_userdata);
    
    pa_mutex_unlock(p->mutex);
    
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

static void memimport_release_cb(pa_memimport *i, uint32_t block_id, void *userdata);

pa_pstream *pa_pstream_new(pa_mainloop_api *m, pa_iochannel *io, pa_mempool *pool) {
    pa_pstream *p;
    
    assert(m);
    assert(io);
    assert(pool);

    p = pa_xnew(pa_pstream, 1);
    PA_REFCNT_INIT(p);
    p->io = io;
    pa_iochannel_set_callback(io, io_callback, p);
    p->dead = 0;

    p->mutex = pa_mutex_new(1);

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

    p->mempool = pool;

    p->use_shm = 0;
    p->export = NULL;

    /* We do importing unconditionally */
    p->import = pa_memimport_new(p->mempool, memimport_release_cb, p);

    pa_iochannel_socket_set_rcvbuf(io, 1024*8); 
    pa_iochannel_socket_set_sndbuf(io, 1024*8);

#ifdef HAVE_CREDS
    p->send_creds_now = 0;
    p->read_creds_valid = 0;
#endif
    return p;
}

static void item_free(void *item, PA_GCC_UNUSED void *p) {
    struct item_info *i = item;
    assert(i);

    if (i->type == PA_PSTREAM_ITEM_MEMBLOCK) {
        assert(i->chunk.memblock);
        pa_memblock_unref(i->chunk.memblock);
    } else if (i->type == PA_PSTREAM_ITEM_PACKET) {
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

    if (p->mutex)
        pa_mutex_free(p->mutex);

    pa_xfree(p);
}

void pa_pstream_send_packet(pa_pstream*p, pa_packet *packet, const pa_creds *creds) {
    struct item_info *i;

    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);
    assert(packet);

    pa_mutex_lock(p->mutex);
    
    if (p->dead)
        goto finish;
    
    i = pa_xnew(struct item_info, 1);
    i->type = PA_PSTREAM_ITEM_PACKET;
    i->packet = pa_packet_ref(packet);
    
#ifdef HAVE_CREDS
    if ((i->with_creds = !!creds))
        i->creds = *creds;
#endif

    pa_queue_push(p->send_queue, i);
    p->mainloop->defer_enable(p->defer_event, 1);

finish:

    pa_mutex_unlock(p->mutex);
}

void pa_pstream_send_memblock(pa_pstream*p, uint32_t channel, int64_t offset, pa_seek_mode_t seek_mode, const pa_memchunk *chunk) {
    size_t length, idx;
    
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);
    assert(channel != (uint32_t) -1);
    assert(chunk);

    pa_mutex_lock(p->mutex);
    
    if (p->dead)
        goto finish;

    length = chunk->length;
    idx = 0;

    while (length > 0) {
        struct item_info *i;
        size_t n;
        
        i = pa_xnew(struct item_info, 1);
        i->type = PA_PSTREAM_ITEM_MEMBLOCK;

        n = length < FRAME_SIZE_MAX_USE ? length : FRAME_SIZE_MAX_USE;
        i->chunk.index = chunk->index + idx;
        i->chunk.length = n;
        i->chunk.memblock = pa_memblock_ref(chunk->memblock);
        
        i->channel = channel;
        i->offset = offset;
        i->seek_mode = seek_mode;
#ifdef HAVE_CREDS
        i->with_creds = 0;
#endif
        
        pa_queue_push(p->send_queue, i);

        idx += n;
        length -= n;
    }
        
    p->mainloop->defer_enable(p->defer_event, 1);

finish:
    
    pa_mutex_unlock(p->mutex);
}

static void memimport_release_cb(pa_memimport *i, uint32_t block_id, void *userdata) {
    struct item_info *item;
    pa_pstream *p = userdata;

    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    pa_mutex_lock(p->mutex);
    
    if (p->dead)
        goto finish;

/*     pa_log("Releasing block %u", block_id); */

    item = pa_xnew(struct item_info, 1);
    item->type = PA_PSTREAM_ITEM_SHMRELEASE;
    item->block_id = block_id;
#ifdef HAVE_CREDS
    item->with_creds = 0;
#endif

    pa_queue_push(p->send_queue, item);
    p->mainloop->defer_enable(p->defer_event, 1);

finish:

    pa_mutex_unlock(p->mutex);
}

static void memexport_revoke_cb(pa_memexport *e, uint32_t block_id, void *userdata) {
    struct item_info *item;
    pa_pstream *p = userdata;

    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    pa_mutex_lock(p->mutex);
    
    if (p->dead)
        goto finish;

/*     pa_log("Revoking block %u", block_id); */
    
    item = pa_xnew(struct item_info, 1);
    item->type = PA_PSTREAM_ITEM_SHMREVOKE;
    item->block_id = block_id;
#ifdef HAVE_CREDS
    item->with_creds = 0;
#endif

    pa_queue_push(p->send_queue, item);
    p->mainloop->defer_enable(p->defer_event, 1);

finish:

    pa_mutex_unlock(p->mutex);
}

static void prepare_next_write_item(pa_pstream *p) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    if (!(p->write.current = pa_queue_pop(p->send_queue)))
        return;
    
    p->write.index = 0;
    p->write.data = NULL;

    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = 0;
    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL] = htonl((uint32_t) -1);
    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = 0;
    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO] = 0;
    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS] = 0;
    
    if (p->write.current->type == PA_PSTREAM_ITEM_PACKET) {
        
        assert(p->write.current->packet);
        p->write.data = p->write.current->packet->data;
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl(p->write.current->packet->length);

    } else if (p->write.current->type == PA_PSTREAM_ITEM_SHMRELEASE) {

        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS] = htonl(PA_FLAG_SHMRELEASE);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = htonl(p->write.current->block_id);

    } else if (p->write.current->type == PA_PSTREAM_ITEM_SHMREVOKE) {

        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS] = htonl(PA_FLAG_SHMREVOKE);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = htonl(p->write.current->block_id);
        
    } else {
        uint32_t flags;
        int send_payload = 1;
        
        assert(p->write.current->type == PA_PSTREAM_ITEM_MEMBLOCK);
        assert(p->write.current->chunk.memblock);
        
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL] = htonl(p->write.current->channel);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = htonl((uint32_t) (((uint64_t) p->write.current->offset) >> 32));
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO] = htonl((uint32_t) ((uint64_t) p->write.current->offset));

        flags = p->write.current->seek_mode & PA_FLAG_SEEKMASK;

        if (p->use_shm) {
            uint32_t block_id, shm_id;
            size_t offset, length;

            assert(p->export);

            if (pa_memexport_put(p->export,
                                 p->write.current->chunk.memblock,
                                 &block_id,
                                 &shm_id,
                                 &offset,
                                 &length) >= 0) {
                
                flags |= PA_FLAG_SHMDATA;
                send_payload = 0;
                
                p->write.shm_info[PA_PSTREAM_SHM_BLOCKID] = htonl(block_id);
                p->write.shm_info[PA_PSTREAM_SHM_SHMID] = htonl(shm_id);
                p->write.shm_info[PA_PSTREAM_SHM_INDEX] = htonl((uint32_t) (offset + p->write.current->chunk.index));
                p->write.shm_info[PA_PSTREAM_SHM_LENGTH] = htonl((uint32_t) p->write.current->chunk.length);
                
                p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl(sizeof(p->write.shm_info));
                p->write.data = p->write.shm_info;
            }
/*             else */
/*                 pa_log_warn("Failed to export memory block."); */
        }

        if (send_payload) {
            p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl(p->write.current->chunk.length);
            p->write.data = (uint8_t*) p->write.current->chunk.memblock->data + p->write.current->chunk.index;
        }
        
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS] = htonl(flags);
    }

#ifdef HAVE_CREDS
    if ((p->send_creds_now = p->write.current->with_creds))
        p->write_creds = p->write.current->creds;
#endif
}

static int do_write(pa_pstream *p) {
    void *d;
    size_t l;
    ssize_t r;
    
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    if (!p->write.current)
        prepare_next_write_item(p);

    if (!p->write.current)
        return 0;

    if (p->write.index < PA_PSTREAM_DESCRIPTOR_SIZE) {
        d = (uint8_t*) p->write.descriptor + p->write.index;
        l = PA_PSTREAM_DESCRIPTOR_SIZE - p->write.index;
    } else {
        assert(p->write.data);
    
        d = (uint8_t*) p->write.data + p->write.index - PA_PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) - (p->write.index - PA_PSTREAM_DESCRIPTOR_SIZE);
    }

    assert(l > 0);
        
#ifdef HAVE_CREDS
    if (p->send_creds_now) {

        if ((r = pa_iochannel_write_with_creds(p->io, d, l, &p->write_creds)) < 0)
            return -1;

        p->send_creds_now = 0;
    } else
#endif

    if ((r = pa_iochannel_write(p->io, d, l)) < 0)
        return -1;

    p->write.index += r;

    if (p->write.index >= PA_PSTREAM_DESCRIPTOR_SIZE + ntohl(p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH])) {
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
    assert(PA_REFCNT_VALUE(p) > 0);
    
    if (p->read.index < PA_PSTREAM_DESCRIPTOR_SIZE) {
        d = (uint8_t*) p->read.descriptor + p->read.index;
        l = PA_PSTREAM_DESCRIPTOR_SIZE - p->read.index;
    } else {
        assert(p->read.data);
        d = (uint8_t*) p->read.data + p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) - (p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE);
    }

#ifdef HAVE_CREDS
    {
        int b = 0;
        
        if ((r = pa_iochannel_read_with_creds(p->io, d, l, &p->read_creds, &b)) <= 0)
            return -1;

        p->read_creds_valid = p->read_creds_valid || b;
    }
#else
    if ((r = pa_iochannel_read(p->io, d, l)) <= 0)
        return -1;
#endif
    
    p->read.index += r;

    if (p->read.index == PA_PSTREAM_DESCRIPTOR_SIZE) {
        uint32_t flags, length, channel;
        /* Reading of frame descriptor complete */

        flags = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS]);

        if (!p->import && (flags & PA_FLAG_SHMMASK) != 0) {
            pa_log_warn("Recieved SHM frame on a socket where SHM is disabled.");
            return -1;
        }
        
        if (flags == PA_FLAG_SHMRELEASE) {

            /* This is a SHM memblock release frame with no payload */

/*             pa_log("Got release frame for %u", ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI])); */
            
            assert(p->export);
            pa_memexport_process_release(p->export, ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI]));

            goto frame_done;
            
        } else if (flags == PA_FLAG_SHMREVOKE) {

            /* This is a SHM memblock revoke frame with no payload */

/*             pa_log("Got revoke frame for %u", ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI])); */

            assert(p->import);
            pa_memimport_process_revoke(p->import, ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI]));

            goto frame_done;
        }

        length = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]);
        
        if (length > FRAME_SIZE_MAX_ALLOW) {
            pa_log_warn("Recieved invalid frame size : %lu", (unsigned long) length);
            return -1;
        }
        
        assert(!p->read.packet && !p->read.memblock);

        channel = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL]);
        
        if (channel == (uint32_t) -1) {

            if (flags != 0) {
                pa_log_warn("Received packet frame with invalid flags value.");
                return -1;
            }
            
            /* Frame is a packet frame */
            p->read.packet = pa_packet_new(length);
            p->read.data = p->read.packet->data;
            
        } else {

            if ((flags & PA_FLAG_SEEKMASK) > PA_SEEK_RELATIVE_END) {
                pa_log_warn("Received memblock frame with invalid seek mode.");
                return -1;
            }
            
            if ((flags & PA_FLAG_SHMMASK) == PA_FLAG_SHMDATA) {

                if (length != sizeof(p->read.shm_info)) {
                    pa_log_warn("Recieved SHM memblock frame with Invalid frame length.");
                    return -1;
                }
            
                /* Frame is a memblock frame referencing an SHM memblock */
                p->read.data = p->read.shm_info;

            } else if ((flags & PA_FLAG_SHMMASK) == 0) {

                /* Frame is a memblock frame */
                
                p->read.memblock = pa_memblock_new(p->mempool, length);
                p->read.data = p->read.memblock->data;
            } else {
                
                pa_log_warn("Recieved memblock frame with invalid flags value.");
                return -1;
            }
        }
            
    } else if (p->read.index > PA_PSTREAM_DESCRIPTOR_SIZE) {
        /* Frame payload available */
        
        if (p->read.memblock && p->recieve_memblock_callback) {

            /* Is this memblock data? Than pass it to the user */
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
                        ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS]) & PA_FLAG_SEEKMASK,
                        &chunk,
                        p->recieve_memblock_callback_userdata);
                }

                /* Drop seek info for following callbacks */
                p->read.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS] =
                    p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] =
                    p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO] = 0;
            }
        }

        /* Frame complete */
        if (p->read.index >= ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) + PA_PSTREAM_DESCRIPTOR_SIZE) {
            
            if (p->read.memblock) {

                /* This was a memblock frame. We can unref the memblock now */
                pa_memblock_unref(p->read.memblock);

            } else if (p->read.packet) {
                
                if (p->recieve_packet_callback)
#ifdef HAVE_CREDS
                    p->recieve_packet_callback(p, p->read.packet, p->read_creds_valid ? &p->read_creds : NULL, p->recieve_packet_callback_userdata);
#else
                    p->recieve_packet_callback(p, p->read.packet, NULL, p->recieve_packet_callback_userdata);
#endif

                pa_packet_unref(p->read.packet);
            } else {
                pa_memblock *b;
                
                assert((ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS]) & PA_FLAG_SHMMASK) == PA_FLAG_SHMDATA);

                assert(p->import);

                if (!(b = pa_memimport_get(p->import,
                                          ntohl(p->read.shm_info[PA_PSTREAM_SHM_BLOCKID]),
                                          ntohl(p->read.shm_info[PA_PSTREAM_SHM_SHMID]),
                                          ntohl(p->read.shm_info[PA_PSTREAM_SHM_INDEX]),
                                          ntohl(p->read.shm_info[PA_PSTREAM_SHM_LENGTH])))) {

                    pa_log_warn("Failed to import memory block.");
                    return -1;
                }

                if (p->recieve_memblock_callback) {
                    int64_t offset;
                    pa_memchunk chunk;
                    
                    chunk.memblock = b;
                    chunk.index = 0;
                    chunk.length = b->length;

                    offset = (int64_t) (
                            (((uint64_t) ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI])) << 32) |
                            (((uint64_t) ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO]))));
                    
                    p->recieve_memblock_callback(
                            p,
                            ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL]),
                            offset,
                            ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS]) & PA_FLAG_SEEKMASK,
                            &chunk,
                            p->recieve_memblock_callback_userdata);
                }

                pa_memblock_unref(b);
            }

            goto frame_done;
        }
    }

    return 0;

frame_done:
    p->read.memblock = NULL;
    p->read.packet = NULL;
    p->read.index = 0;

#ifdef HAVE_CREDS
    p->read_creds_valid = 0;
#endif

    return 0;
}

void pa_pstream_set_die_callback(pa_pstream *p, pa_pstream_notify_cb_t cb, void *userdata) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);
    
    pa_mutex_lock(p->mutex);
    p->die_callback = cb;
    p->die_callback_userdata = userdata;
    pa_mutex_unlock(p->mutex);
}

void pa_pstream_set_drain_callback(pa_pstream *p, pa_pstream_notify_cb_t cb, void *userdata) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    pa_mutex_lock(p->mutex);
    p->drain_callback = cb;
    p->drain_callback_userdata = userdata;
    pa_mutex_unlock(p->mutex);
}

void pa_pstream_set_recieve_packet_callback(pa_pstream *p, pa_pstream_packet_cb_t cb, void *userdata) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    pa_mutex_lock(p->mutex);
    p->recieve_packet_callback = cb;
    p->recieve_packet_callback_userdata = userdata;
    pa_mutex_unlock(p->mutex);
}

void pa_pstream_set_recieve_memblock_callback(pa_pstream *p, pa_pstream_memblock_cb_t cb, void *userdata) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    pa_mutex_lock(p->mutex);
    p->recieve_memblock_callback = cb;
    p->recieve_memblock_callback_userdata = userdata;
    pa_mutex_unlock(p->mutex);
}

int pa_pstream_is_pending(pa_pstream *p) {
    int b;

    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    pa_mutex_lock(p->mutex);

    if (p->dead)
        b = 0;
    else
        b = p->write.current || !pa_queue_is_empty(p->send_queue);

    pa_mutex_unlock(p->mutex);

    return b;
}

void pa_pstream_unref(pa_pstream*p) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    if (PA_REFCNT_DEC(p) <= 0)
        pstream_free(p);
}

pa_pstream* pa_pstream_ref(pa_pstream*p) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);
    
    PA_REFCNT_INC(p);
    return p;
}

void pa_pstream_close(pa_pstream *p) {
    assert(p);

    pa_mutex_lock(p->mutex);
    
    p->dead = 1;

    if (p->import) {
        pa_memimport_free(p->import);
        p->import = NULL;
    }

    if (p->export) {
        pa_memexport_free(p->export);
        p->export = NULL;
    }

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

    pa_mutex_unlock(p->mutex);
}

void pa_pstream_use_shm(pa_pstream *p, int enable) {
    assert(p);
    assert(PA_REFCNT_VALUE(p) > 0);

    pa_mutex_lock(p->mutex);

    p->use_shm = enable;

    if (enable) {
    
        if (!p->export)
            p->export = pa_memexport_new(p->mempool, memexport_revoke_cb, p);

    } else {

        if (p->export) {
            pa_memexport_free(p->export);
            p->export = NULL;
        }
    }

    pa_mutex_unlock(p->mutex);
}
