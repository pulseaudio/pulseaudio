/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
  Copyright 2006 Pierre Ossman <ossman@cendio.se> for Cendio AB

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


#include <pulse/xmalloc.h>

#include <pulsecore/winsock.h>
#include <pulsecore/queue.h>
#include <pulsecore/log.h>
#include <pulsecore/core-scache.h>
#include <pulsecore/creds.h>
#include <pulsecore/refcnt.h>
#include <pulsecore/flist.h>
#include <pulsecore/macro.h>

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

PA_STATIC_FLIST_DECLARE(items, 0, pa_xfree);

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
    pa_bool_t with_creds;
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

    pa_bool_t dead;

    struct {
        pa_pstream_descriptor descriptor;
        struct item_info* current;
        uint32_t shm_info[PA_PSTREAM_SHM_MAX];
        void *data;
        size_t index;
        pa_memchunk memchunk;
    } write;

    struct {
        pa_pstream_descriptor descriptor;
        pa_memblock *memblock;
        pa_packet *packet;
        uint32_t shm_info[PA_PSTREAM_SHM_MAX];
        void *data;
        size_t index;
    } read;

    pa_bool_t use_shm;
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

    pa_pstream_block_id_cb_t revoke_callback;
    void *revoke_callback_userdata;

    pa_pstream_block_id_cb_t release_callback;
    void *release_callback_userdata;

    pa_mempool *mempool;

#ifdef HAVE_CREDS
    pa_creds read_creds, write_creds;
    pa_bool_t read_creds_valid, send_creds_now;
#endif
};

static int do_write(pa_pstream *p);
static int do_read(pa_pstream *p);

static void do_something(pa_pstream *p) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    pa_pstream_ref(p);

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

    pa_pstream_unref(p);
    return;

fail:

    if (p->die_callback)
        p->die_callback(p, p->die_callback_userdata);

    pa_pstream_unlink(p);
    pa_pstream_unref(p);
}

static void io_callback(pa_iochannel*io, void *userdata) {
    pa_pstream *p = userdata;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);
    pa_assert(p->io == io);

    do_something(p);
}

static void defer_callback(pa_mainloop_api *m, pa_defer_event *e, void*userdata) {
    pa_pstream *p = userdata;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);
    pa_assert(p->defer_event == e);
    pa_assert(p->mainloop == m);

    do_something(p);
}

static void memimport_release_cb(pa_memimport *i, uint32_t block_id, void *userdata);

pa_pstream *pa_pstream_new(pa_mainloop_api *m, pa_iochannel *io, pa_mempool *pool) {
    pa_pstream *p;

    pa_assert(m);
    pa_assert(io);
    pa_assert(pool);

    p = pa_xnew(pa_pstream, 1);
    PA_REFCNT_INIT(p);
    p->io = io;
    pa_iochannel_set_callback(io, io_callback, p);
    p->dead = FALSE;

    p->mainloop = m;
    p->defer_event = m->defer_new(m, defer_callback, p);
    m->defer_enable(p->defer_event, 0);

    p->send_queue = pa_queue_new();

    p->write.current = NULL;
    p->write.index = 0;
    pa_memchunk_reset(&p->write.memchunk);
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
    p->revoke_callback = NULL;
    p->revoke_callback_userdata = NULL;
    p->release_callback = NULL;
    p->release_callback_userdata = NULL;

    p->mempool = pool;

    p->use_shm = FALSE;
    p->export = NULL;

    /* We do importing unconditionally */
    p->import = pa_memimport_new(p->mempool, memimport_release_cb, p);

    pa_iochannel_socket_set_rcvbuf(io, pa_mempool_block_size_max(p->mempool));
    pa_iochannel_socket_set_sndbuf(io, pa_mempool_block_size_max(p->mempool));

#ifdef HAVE_CREDS
    p->send_creds_now = FALSE;
    p->read_creds_valid = FALSE;
#endif
    return p;
}

static void item_free(void *item, void *q) {
    struct item_info *i = item;
    pa_assert(i);

    if (i->type == PA_PSTREAM_ITEM_MEMBLOCK) {
        pa_assert(i->chunk.memblock);
        pa_memblock_unref(i->chunk.memblock);
    } else if (i->type == PA_PSTREAM_ITEM_PACKET) {
        pa_assert(i->packet);
        pa_packet_unref(i->packet);
    }

    if (pa_flist_push(PA_STATIC_FLIST_GET(items), i) < 0)
        pa_xfree(i);
}

static void pstream_free(pa_pstream *p) {
    pa_assert(p);

    pa_pstream_unlink(p);

    pa_queue_free(p->send_queue, item_free, NULL);

    if (p->write.current)
        item_free(p->write.current, NULL);

    if (p->write.memchunk.memblock)
        pa_memblock_unref(p->write.memchunk.memblock);

    if (p->read.memblock)
        pa_memblock_unref(p->read.memblock);

    if (p->read.packet)
        pa_packet_unref(p->read.packet);

    pa_xfree(p);
}

void pa_pstream_send_packet(pa_pstream*p, pa_packet *packet, const pa_creds *creds) {
    struct item_info *i;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);
    pa_assert(packet);

    if (p->dead)
        return;

    if (!(i = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
        i = pa_xnew(struct item_info, 1);

    i->type = PA_PSTREAM_ITEM_PACKET;
    i->packet = pa_packet_ref(packet);

#ifdef HAVE_CREDS
    if ((i->with_creds = !!creds))
        i->creds = *creds;
#endif

    pa_queue_push(p->send_queue, i);

    p->mainloop->defer_enable(p->defer_event, 1);
}

void pa_pstream_send_memblock(pa_pstream*p, uint32_t channel, int64_t offset, pa_seek_mode_t seek_mode, const pa_memchunk *chunk) {
    size_t length, idx;
    size_t bsm;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);
    pa_assert(channel != (uint32_t) -1);
    pa_assert(chunk);

    if (p->dead)
        return;

    idx = 0;
    length = chunk->length;

    bsm = pa_mempool_block_size_max(p->mempool);

    while (length > 0) {
        struct item_info *i;
        size_t n;

        if (!(i = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
            i = pa_xnew(struct item_info, 1);
        i->type = PA_PSTREAM_ITEM_MEMBLOCK;

        n = PA_MIN(length, bsm);
        i->chunk.index = chunk->index + idx;
        i->chunk.length = n;
        i->chunk.memblock = pa_memblock_ref(chunk->memblock);

        i->channel = channel;
        i->offset = offset;
        i->seek_mode = seek_mode;
#ifdef HAVE_CREDS
        i->with_creds = FALSE;
#endif

        pa_queue_push(p->send_queue, i);

        idx += n;
        length -= n;
    }

    p->mainloop->defer_enable(p->defer_event, 1);
}

void pa_pstream_send_release(pa_pstream *p, uint32_t block_id) {
    struct item_info *item;
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    if (p->dead)
        return;

/*     pa_log("Releasing block %u", block_id); */

    if (!(item = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
        item = pa_xnew(struct item_info, 1);
    item->type = PA_PSTREAM_ITEM_SHMRELEASE;
    item->block_id = block_id;
#ifdef HAVE_CREDS
    item->with_creds = FALSE;
#endif

    pa_queue_push(p->send_queue, item);
    p->mainloop->defer_enable(p->defer_event, 1);
}

/* might be called from thread context */
static void memimport_release_cb(pa_memimport *i, uint32_t block_id, void *userdata) {
    pa_pstream *p = userdata;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    if (p->dead)
        return;

    if (p->release_callback)
        p->release_callback(p, block_id, p->release_callback_userdata);
    else
        pa_pstream_send_release(p, block_id);
}

void pa_pstream_send_revoke(pa_pstream *p, uint32_t block_id) {
    struct item_info *item;
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    if (p->dead)
        return;
/*     pa_log("Revoking block %u", block_id); */

    if (!(item = pa_flist_pop(PA_STATIC_FLIST_GET(items))))
        item = pa_xnew(struct item_info, 1);
    item->type = PA_PSTREAM_ITEM_SHMREVOKE;
    item->block_id = block_id;
#ifdef HAVE_CREDS
    item->with_creds = FALSE;
#endif

    pa_queue_push(p->send_queue, item);
    p->mainloop->defer_enable(p->defer_event, 1);
}

/* might be called from thread context */
static void memexport_revoke_cb(pa_memexport *e, uint32_t block_id, void *userdata) {
    pa_pstream *p = userdata;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    if (p->revoke_callback)
        p->revoke_callback(p, block_id, p->revoke_callback_userdata);
    else
        pa_pstream_send_revoke(p, block_id);
}

static void prepare_next_write_item(pa_pstream *p) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    p->write.current = pa_queue_pop(p->send_queue);

    if (!p->write.current)
        return;

    p->write.index = 0;
    p->write.data = NULL;
    pa_memchunk_reset(&p->write.memchunk);

    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = 0;
    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL] = htonl((uint32_t) -1);
    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = 0;
    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO] = 0;
    p->write.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS] = 0;

    if (p->write.current->type == PA_PSTREAM_ITEM_PACKET) {

        pa_assert(p->write.current->packet);
        p->write.data = p->write.current->packet->data;
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl((uint32_t) p->write.current->packet->length);

    } else if (p->write.current->type == PA_PSTREAM_ITEM_SHMRELEASE) {

        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS] = htonl(PA_FLAG_SHMRELEASE);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = htonl(p->write.current->block_id);

    } else if (p->write.current->type == PA_PSTREAM_ITEM_SHMREVOKE) {

        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS] = htonl(PA_FLAG_SHMREVOKE);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = htonl(p->write.current->block_id);

    } else {
        uint32_t flags;
        pa_bool_t send_payload = TRUE;

        pa_assert(p->write.current->type == PA_PSTREAM_ITEM_MEMBLOCK);
        pa_assert(p->write.current->chunk.memblock);

        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_CHANNEL] = htonl(p->write.current->channel);
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI] = htonl((uint32_t) (((uint64_t) p->write.current->offset) >> 32));
        p->write.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_LO] = htonl((uint32_t) ((uint64_t) p->write.current->offset));

        flags = (uint32_t) (p->write.current->seek_mode & PA_FLAG_SEEKMASK);

        if (p->use_shm) {
            uint32_t block_id, shm_id;
            size_t offset, length;

            pa_assert(p->export);

            if (pa_memexport_put(p->export,
                                 p->write.current->chunk.memblock,
                                 &block_id,
                                 &shm_id,
                                 &offset,
                                 &length) >= 0) {

                flags |= PA_FLAG_SHMDATA;
                send_payload = FALSE;

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
            p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH] = htonl((uint32_t) p->write.current->chunk.length);
            p->write.memchunk = p->write.current->chunk;
            pa_memblock_ref(p->write.memchunk.memblock);
            p->write.data = NULL;
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
    pa_memblock *release_memblock = NULL;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    if (!p->write.current)
        prepare_next_write_item(p);

    if (!p->write.current)
        return 0;

    if (p->write.index < PA_PSTREAM_DESCRIPTOR_SIZE) {
        d = (uint8_t*) p->write.descriptor + p->write.index;
        l = PA_PSTREAM_DESCRIPTOR_SIZE - p->write.index;
    } else {
        pa_assert(p->write.data || p->write.memchunk.memblock);

        if (p->write.data)
            d = p->write.data;
        else {
            d = (uint8_t*) pa_memblock_acquire(p->write.memchunk.memblock) + p->write.memchunk.index;
            release_memblock = p->write.memchunk.memblock;
        }

        d = (uint8_t*) d + p->write.index - PA_PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) - (p->write.index - PA_PSTREAM_DESCRIPTOR_SIZE);
    }

    pa_assert(l > 0);

#ifdef HAVE_CREDS
    if (p->send_creds_now) {

        if ((r = pa_iochannel_write_with_creds(p->io, d, l, &p->write_creds)) < 0)
            goto fail;

        p->send_creds_now = FALSE;
    } else
#endif

    if ((r = pa_iochannel_write(p->io, d, l)) < 0)
        goto fail;

    if (release_memblock)
        pa_memblock_release(release_memblock);

    p->write.index += (size_t) r;

    if (p->write.index >= PA_PSTREAM_DESCRIPTOR_SIZE + ntohl(p->write.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH])) {
        pa_assert(p->write.current);
        item_free(p->write.current, NULL);
        p->write.current = NULL;

        if (p->write.memchunk.memblock)
            pa_memblock_unref(p->write.memchunk.memblock);

        pa_memchunk_reset(&p->write.memchunk);

        if (p->drain_callback && !pa_pstream_is_pending(p))
            p->drain_callback(p, p->drain_callback_userdata);
    }

    return 0;

fail:

    if (release_memblock)
        pa_memblock_release(release_memblock);

    return -1;
}

static int do_read(pa_pstream *p) {
    void *d;
    size_t l;
    ssize_t r;
    pa_memblock *release_memblock = NULL;
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    if (p->read.index < PA_PSTREAM_DESCRIPTOR_SIZE) {
        d = (uint8_t*) p->read.descriptor + p->read.index;
        l = PA_PSTREAM_DESCRIPTOR_SIZE - p->read.index;
    } else {
        pa_assert(p->read.data || p->read.memblock);

        if (p->read.data)
            d = p->read.data;
        else {
            d = pa_memblock_acquire(p->read.memblock);
            release_memblock = p->read.memblock;
        }

        d = (uint8_t*) d + p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE;
        l = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]) - (p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE);
    }

#ifdef HAVE_CREDS
    {
        pa_bool_t b = 0;

        if ((r = pa_iochannel_read_with_creds(p->io, d, l, &p->read_creds, &b)) <= 0)
            goto fail;

        p->read_creds_valid = p->read_creds_valid || b;
    }
#else
    if ((r = pa_iochannel_read(p->io, d, l)) <= 0)
        goto fail;
#endif

    if (release_memblock)
        pa_memblock_release(release_memblock);

    p->read.index += (size_t) r;

    if (p->read.index == PA_PSTREAM_DESCRIPTOR_SIZE) {
        uint32_t flags, length, channel;
        /* Reading of frame descriptor complete */

        flags = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS]);

        if (!p->use_shm && (flags & PA_FLAG_SHMMASK) != 0) {
            pa_log_warn("Received SHM frame on a socket where SHM is disabled.");
            return -1;
        }

        if (flags == PA_FLAG_SHMRELEASE) {

            /* This is a SHM memblock release frame with no payload */

/*             pa_log("Got release frame for %u", ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI])); */

            pa_assert(p->export);
            pa_memexport_process_release(p->export, ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI]));

            goto frame_done;

        } else if (flags == PA_FLAG_SHMREVOKE) {

            /* This is a SHM memblock revoke frame with no payload */

/*             pa_log("Got revoke frame for %u", ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI])); */

            pa_assert(p->import);
            pa_memimport_process_revoke(p->import, ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_OFFSET_HI]));

            goto frame_done;
        }

        length = ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_LENGTH]);

        if (length > FRAME_SIZE_MAX_ALLOW || length <= 0) {
            pa_log_warn("Received invalid frame size: %lu", (unsigned long) length);
            return -1;
        }

        pa_assert(!p->read.packet && !p->read.memblock);

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
                    pa_log_warn("Received SHM memblock frame with Invalid frame length.");
                    return -1;
                }

                /* Frame is a memblock frame referencing an SHM memblock */
                p->read.data = p->read.shm_info;

            } else if ((flags & PA_FLAG_SHMMASK) == 0) {

                /* Frame is a memblock frame */

                p->read.memblock = pa_memblock_new(p->mempool, length);
                p->read.data = NULL;
            } else {

                pa_log_warn("Received memblock frame with invalid flags value.");
                return -1;
            }
        }

    } else if (p->read.index > PA_PSTREAM_DESCRIPTOR_SIZE) {
        /* Frame payload available */

        if (p->read.memblock && p->recieve_memblock_callback) {

            /* Is this memblock data? Than pass it to the user */
            l = (p->read.index - (size_t) r) < PA_PSTREAM_DESCRIPTOR_SIZE ? (size_t) (p->read.index - PA_PSTREAM_DESCRIPTOR_SIZE) : (size_t) r;

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

                pa_assert((ntohl(p->read.descriptor[PA_PSTREAM_DESCRIPTOR_FLAGS]) & PA_FLAG_SHMMASK) == PA_FLAG_SHMDATA);

                pa_assert(p->import);

                if (!(b = pa_memimport_get(p->import,
                                          ntohl(p->read.shm_info[PA_PSTREAM_SHM_BLOCKID]),
                                          ntohl(p->read.shm_info[PA_PSTREAM_SHM_SHMID]),
                                          ntohl(p->read.shm_info[PA_PSTREAM_SHM_INDEX]),
                                          ntohl(p->read.shm_info[PA_PSTREAM_SHM_LENGTH])))) {

                    if (pa_log_ratelimit())
                        pa_log_debug("Failed to import memory block.");
                }

                if (p->recieve_memblock_callback) {
                    int64_t offset;
                    pa_memchunk chunk;

                    chunk.memblock = b;
                    chunk.index = 0;
                    chunk.length = b ? pa_memblock_get_length(b) : ntohl(p->read.shm_info[PA_PSTREAM_SHM_LENGTH]);

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

                if (b)
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
    p->read.data = NULL;

#ifdef HAVE_CREDS
    p->read_creds_valid = FALSE;
#endif

    return 0;

fail:
    if (release_memblock)
        pa_memblock_release(release_memblock);

    return -1;
}

void pa_pstream_set_die_callback(pa_pstream *p, pa_pstream_notify_cb_t cb, void *userdata) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    p->die_callback = cb;
    p->die_callback_userdata = userdata;
}

void pa_pstream_set_drain_callback(pa_pstream *p, pa_pstream_notify_cb_t cb, void *userdata) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    p->drain_callback = cb;
    p->drain_callback_userdata = userdata;
}

void pa_pstream_set_recieve_packet_callback(pa_pstream *p, pa_pstream_packet_cb_t cb, void *userdata) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    p->recieve_packet_callback = cb;
    p->recieve_packet_callback_userdata = userdata;
}

void pa_pstream_set_recieve_memblock_callback(pa_pstream *p, pa_pstream_memblock_cb_t cb, void *userdata) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    p->recieve_memblock_callback = cb;
    p->recieve_memblock_callback_userdata = userdata;
}

void pa_pstream_set_release_callback(pa_pstream *p, pa_pstream_block_id_cb_t cb, void *userdata) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    p->release_callback = cb;
    p->release_callback_userdata = userdata;
}

void pa_pstream_set_revoke_callback(pa_pstream *p, pa_pstream_block_id_cb_t cb, void *userdata) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    p->release_callback = cb;
    p->release_callback_userdata = userdata;
}

pa_bool_t pa_pstream_is_pending(pa_pstream *p) {
    pa_bool_t b;

    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    if (p->dead)
        b = FALSE;
    else
        b = p->write.current || !pa_queue_isempty(p->send_queue);

    return b;
}

void pa_pstream_unref(pa_pstream*p) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    if (PA_REFCNT_DEC(p) <= 0)
        pstream_free(p);
}

pa_pstream* pa_pstream_ref(pa_pstream*p) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    PA_REFCNT_INC(p);
    return p;
}

void pa_pstream_unlink(pa_pstream *p) {
    pa_assert(p);

    if (p->dead)
        return;

    p->dead = TRUE;

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
}

void pa_pstream_enable_shm(pa_pstream *p, pa_bool_t enable) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

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
}

pa_bool_t pa_pstream_get_shm(pa_pstream *p) {
    pa_assert(p);
    pa_assert(PA_REFCNT_VALUE(p) > 0);

    return p->use_shm;
}
