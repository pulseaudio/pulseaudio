/* $Id$ */

/***
  This file is part of PulseAudio.
 
  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <stdio.h>
#include <unistd.h>

#include <pulsecore/memblock.h>
#include <pulse/xmalloc.h>

static void release_cb(pa_memimport *i, uint32_t block_id, void *userdata) {
    printf("%s: Imported block %u is released.\n", (char*) userdata, block_id);
}

static void revoke_cb(pa_memexport *e, uint32_t block_id, void *userdata) {
    printf("%s: Exported block %u is revoked.\n", (char*) userdata, block_id);
}

static void print_stats(pa_mempool *p, const char *text) {
    const pa_mempool_stat*s = pa_mempool_get_stat(p);

    printf("%s = {\n"
           "n_allocated = %u\n"
           "n_accumulated = %u\n"
           "n_imported = %u\n"
           "n_exported = %u\n"
           "allocated_size = %lu\n"
           "accumulated_size = %lu\n"
           "imported_size = %lu\n"
           "exported_size = %lu\n"
           "n_too_large_for_pool = %u\n"
           "n_pool_full = %u\n"
           "}\n",
           text,
           s->n_allocated,
           s->n_accumulated,
           s->n_imported,
           s->n_exported,
           (unsigned long) s->allocated_size,
           (unsigned long) s->accumulated_size,
           (unsigned long) s->imported_size,
           (unsigned long) s->exported_size,
           s->n_too_large_for_pool,
           s->n_pool_full);
}

int main(int argc, char *argv[]) {
    pa_mempool *pool_a, *pool_b, *pool_c;
    unsigned id_a, id_b, id_c;
    pa_memexport *export_a, *export_b;
    pa_memimport *import_b, *import_c;
    pa_memblock *mb_a, *mb_b, *mb_c;
    int r, i;
    pa_memblock* blocks[5];
    uint32_t id, shm_id;
    size_t offset, size;

    const char txt[] = "This is a test!";
    
    pool_a = pa_mempool_new(1);
    pool_b = pa_mempool_new(1);
    pool_c = pa_mempool_new(1);

    pa_mempool_get_shm_id(pool_a, &id_a);
    pa_mempool_get_shm_id(pool_b, &id_b);
    pa_mempool_get_shm_id(pool_c, &id_c);
    
    assert(pool_a && pool_b && pool_c);
    
    blocks[0] = pa_memblock_new_fixed(pool_a, (void*) txt, sizeof(txt), 1);
    blocks[1] = pa_memblock_new(pool_a, sizeof(txt));
    snprintf(blocks[1]->data, blocks[1]->length, "%s", txt);
    blocks[2] = pa_memblock_new_pool(pool_a, sizeof(txt));
    snprintf(blocks[2]->data, blocks[2]->length, "%s", txt);
    blocks[3] = pa_memblock_new_malloced(pool_a, pa_xstrdup(txt), sizeof(txt));
    blocks[4] = NULL;

    for (i = 0; blocks[i]; i++) {
        printf("Memory block %u\n", i);

        mb_a = blocks[i];
        assert(mb_a);
        
        export_a = pa_memexport_new(pool_a, revoke_cb, (void*) "A");
        export_b = pa_memexport_new(pool_b, revoke_cb, (void*) "B");
        
        assert(export_a && export_b);
        
        import_b = pa_memimport_new(pool_b, release_cb, (void*) "B");
        import_c = pa_memimport_new(pool_c, release_cb, (void*) "C");
        
        assert(import_b && import_c);
        
        r = pa_memexport_put(export_a, mb_a, &id, &shm_id, &offset, &size);
        assert(r >= 0);
        assert(shm_id == id_a);

        printf("A: Memory block exported as %u\n", id);
        
        mb_b = pa_memimport_get(import_b, id, shm_id, offset, size);
        assert(mb_b);
        r = pa_memexport_put(export_b, mb_b, &id, &shm_id, &offset, &size);
        assert(r >= 0);
        assert(shm_id == id_a || shm_id == id_b);
        pa_memblock_unref(mb_b);

        printf("B: Memory block exported as %u\n", id);
        
        mb_c = pa_memimport_get(import_c, id, shm_id, offset, size);
        assert(mb_c);
        printf("1 data=%s\n", (char*) mb_c->data);

        print_stats(pool_a, "A");
        print_stats(pool_b, "B");
        print_stats(pool_c, "C");
        
        pa_memexport_free(export_b);
        printf("2 data=%s\n", (char*) mb_c->data);
        pa_memblock_unref(mb_c);
        
        pa_memimport_free(import_b);
        
        pa_memblock_unref(mb_a);
        
        pa_memimport_free(import_c);
        pa_memexport_free(export_a);
    }

    printf("vaccuuming...\n");
    
    pa_mempool_vacuum(pool_a);
    pa_mempool_vacuum(pool_b);
    pa_mempool_vacuum(pool_c);

    printf("vaccuuming done...\n");

    pa_mempool_free(pool_a);
    pa_mempool_free(pool_b);
    pa_mempool_free(pool_c);

    return 0;
}
