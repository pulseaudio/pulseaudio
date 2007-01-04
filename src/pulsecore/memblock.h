#ifndef foopulsememblockhfoo
#define foopulsememblockhfoo

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

#include <sys/types.h>
#include <inttypes.h>

#include <pulsecore/llist.h>
#include <pulsecore/refcnt.h>

/* A pa_memblock is a reference counted memory block. PulseAudio
 * passed references to pa_memblocks around instead of copying
 * data. See pa_memchunk for a structure that describes parts of
 * memory blocks. */

/* The type of memory this block points to */
typedef enum pa_memblock_type {
    PA_MEMBLOCK_POOL,             /* Memory is part of the memory pool */
    PA_MEMBLOCK_POOL_EXTERNAL,    /* Data memory is part of the memory pool but the pa_memblock structure itself not */
    PA_MEMBLOCK_APPENDED,         /* the data is appended to the memory block */
    PA_MEMBLOCK_USER,             /* User supplied memory, to be freed with free_cb */
    PA_MEMBLOCK_FIXED,            /* data is a pointer to fixed memory that needs not to be freed */
    PA_MEMBLOCK_IMPORTED,         /* Memory is imported from another process via shm */
    PA_MEMBLOCK_TYPE_MAX
} pa_memblock_type_t;

typedef struct pa_memblock pa_memblock;
typedef struct pa_mempool pa_mempool;
typedef struct pa_mempool_stat pa_mempool_stat;
typedef struct pa_memimport_segment pa_memimport_segment;
typedef struct pa_memimport pa_memimport;
typedef struct pa_memexport pa_memexport;

typedef void (*pa_memimport_release_cb_t)(pa_memimport *i, uint32_t block_id, void *userdata);
typedef void (*pa_memexport_revoke_cb_t)(pa_memexport *e, uint32_t block_id, void *userdata);

struct pa_memblock {
    pa_memblock_type_t type;
    int read_only; /* boolean */
    PA_REFCNT_DECLARE; /* the reference counter */
    size_t length;
    void *data;
    pa_mempool *pool;

    union {
        struct {
            void (*free_cb)(void *p);  /* If type == PA_MEMBLOCK_USER this points to a function for freeing this memory block */
        } user;

        struct  {
            uint32_t id;
            pa_memimport_segment *segment;
        } imported;
    } per_type;
};

/* Please note that updates to this structure are not locked,
 * i.e. n_allocated might be updated at a point in time where
 * n_accumulated is not yet. Take these values with a grain of salt,
 * threy are here for purely statistical reasons.*/
struct pa_mempool_stat {
    AO_t n_allocated;
    AO_t n_accumulated;
    AO_t n_imported;
    AO_t n_exported;
    AO_t allocated_size;
    AO_t accumulated_size;
    AO_t imported_size;
    AO_t exported_size;

    AO_t n_too_large_for_pool;
    AO_t n_pool_full;

    AO_t n_allocated_by_type[PA_MEMBLOCK_TYPE_MAX];
    AO_t n_accumulated_by_type[PA_MEMBLOCK_TYPE_MAX];
};

/* Allocate a new memory block of type PA_MEMBLOCK_MEMPOOL or PA_MEMBLOCK_APPENDED, depending on the size */
pa_memblock *pa_memblock_new(pa_mempool *, size_t length);

/* Allocate a new memory block of type PA_MEMBLOCK_MEMPOOL. If the requested size is too large, return NULL */
pa_memblock *pa_memblock_new_pool(pa_mempool *, size_t length);

/* Allocate a new memory block of type PA_MEMBLOCK_USER */
pa_memblock *pa_memblock_new_user(pa_mempool *, void *data, size_t length, void (*free_cb)(void *p), int read_only);

/* A special case of pa_memblock_new_user: take a memory buffer previously allocated with pa_xmalloc()  */
#define pa_memblock_new_malloced(p,data,length) pa_memblock_new_user(p, data, length, pa_xfree, 0)

/* Allocate a new memory block of type PA_MEMBLOCK_FIXED */
pa_memblock *pa_memblock_new_fixed(pa_mempool *, void *data, size_t length, int read_only);

void pa_memblock_unref(pa_memblock*b);
pa_memblock* pa_memblock_ref(pa_memblock*b);

/* This special unref function has to be called by the owner of the
memory of a static memory block when he wants to release all
references to the memory. This causes the memory to be copied and
converted into a PA_MEMBLOCK_DYNAMIC type memory block */
void pa_memblock_unref_fixed(pa_memblock*b);

/* The memory block manager */
pa_mempool* pa_mempool_new(int shared);
void pa_mempool_free(pa_mempool *p);
const pa_mempool_stat* pa_mempool_get_stat(pa_mempool *p);
void pa_mempool_vacuum(pa_mempool *p);
int pa_mempool_get_shm_id(pa_mempool *p, uint32_t *id);
int pa_mempool_is_shared(pa_mempool *p);

/* For recieving blocks from other nodes */
pa_memimport* pa_memimport_new(pa_mempool *p, pa_memimport_release_cb_t cb, void *userdata);
void pa_memimport_free(pa_memimport *i);
pa_memblock* pa_memimport_get(pa_memimport *i, uint32_t block_id, uint32_t shm_id, size_t offset, size_t size);
int pa_memimport_process_revoke(pa_memimport *i, uint32_t block_id);

/* For sending blocks to other nodes */
pa_memexport* pa_memexport_new(pa_mempool *p, pa_memexport_revoke_cb_t cb, void *userdata);
void pa_memexport_free(pa_memexport *e);
int pa_memexport_put(pa_memexport *e, pa_memblock *b, uint32_t *block_id, uint32_t *shm_id, size_t *offset, size_t *size);
int pa_memexport_process_release(pa_memexport *e, uint32_t id);

#endif
