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

#include <assert.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>

#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/random.h>
#include <pulse/xmalloc.h>

#include "shm.h"

#if defined(__linux__) && !defined(MADV_REMOVE)
#define MADV_REMOVE 9
#endif    

#define MAX_SHM_SIZE (1024*1024*20)

static char *segment_name(char *fn, size_t l, unsigned id) {
    snprintf(fn, l, "/pulse-shm-%u", id);
    return fn;
}

int pa_shm_create_rw(pa_shm *m, size_t size, int shared, mode_t mode) {
    char fn[32];
    int fd = -1;
    
    assert(m);
    assert(size > 0);
    assert(size < MAX_SHM_SIZE);
    assert(mode >= 0600);

    if (!shared) {
        m->id = 0;
        m->size = size;

#ifdef MAP_ANONYMOUS
        if ((m->ptr = mmap(NULL, m->size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, 0)) == MAP_FAILED) {
            pa_log("mmap() failed: %s", pa_cstrerror(errno));
            goto fail;
        }
#elif defined(HAVE_POSIX_MEMALIGN)
        {
            int r;
            
            if ((r = posix_memalign(&m->ptr, sysconf(_SC_PAGESIZE), size)) < 0) {
                pa_log("posix_memalign() failed: %s", pa_cstrerror(r));
                goto fail;
            }
        }
#else
        m->ptr = pa_xmalloc(m->size);
#endif
        
        m->do_unlink = 0;
        
    } else {
        pa_random(&m->id, sizeof(m->id));
        segment_name(fn, sizeof(fn), m->id);
    
        if ((fd = shm_open(fn, O_RDWR|O_CREAT|O_EXCL, mode & 0444)) < 0) {
            pa_log("shm_open() failed: %s", pa_cstrerror(errno));
            goto fail;
        }
        
        if (ftruncate(fd, m->size = size) < 0) {
            pa_log("ftruncate() failed: %s", pa_cstrerror(errno));
            goto fail;
        }
        
        if ((m->ptr = mmap(NULL, m->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, 0)) == MAP_FAILED) {
            pa_log("mmap() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        close(fd);
        m->do_unlink = 1;
    }

    m->shared = shared;
    
    return 0;
    
fail:

    if (fd >= 0) {
        shm_unlink(fn);
        close(fd);
    }

    return -1;
}

void pa_shm_free(pa_shm *m) {
    char fn[32];
    
    assert(m);
    assert(m->ptr && m->ptr != MAP_FAILED);
    assert(m->size > 0);

#if !defined(MAP_ANONYMOUS) && defined(HAVE_POSIX_MEMALIGN)
    if (!m->shared)
        free(m->ptr);
    else
#elif !defined(MAP_ANONYMOUS)
    if (!m->shared)
        pa_xfree(m->ptr);
    else
#endif        
    
    if (munmap(m->ptr, m->size) < 0)
        pa_log("munmap() failed: %s", pa_cstrerror(errno));

    if (m->do_unlink) {
        segment_name(fn, sizeof(fn), m->id);
        
        if (shm_unlink(fn) < 0)
            pa_log(__FILE__":shm_unlink(%s) failed: %s", fn, pa_cstrerror(errno));
    }

    memset(m, 0, sizeof(*m));
}

void pa_shm_punch(pa_shm *m, size_t offset, size_t size) {
    void *ptr;
    
    assert(m);
    assert(m->ptr && m->ptr != MAP_FAILED);
    assert(m->size > 0);
    assert(offset < m->size);
    assert(offset+size < m->size);

    /* You're welcome to implement this as NOOP on systems that don't
     * support it */

    ptr = (uint8_t*) m->ptr + offset;
    
#ifdef __linux__
{
    /* On Linux ptr must be page aligned */
    long psz = sysconf(_SC_PAGESIZE);
    unsigned o;

    o = ((unsigned long) ptr) - ((((unsigned long) ptr)/psz) * psz);
    
    if (o > 0) {
        ptr = (uint8_t*) ptr + (psz - o);
        size -= psz - o;
    }
}
#endif
    
#ifdef MADV_REMOVE
    if (madvise(ptr, size, MADV_REMOVE) >= 0)
        return;
#endif

#ifdef MADV_FREE
    if (madvise(ptr, size, MADV_FREE) >= 0)
        return;
#endif    
    
#ifdef MADV_DONTNEED
    madvise(ptr, size, MADV_DONTNEED);
#endif
}

int pa_shm_attach_ro(pa_shm *m, unsigned id) {
    char fn[32];
    int fd = -1;
    struct stat st;

    assert(m);

    segment_name(fn, sizeof(fn), m->id = id);

    if ((fd = shm_open(fn, O_RDONLY, 0)) < 0) {
        pa_log("shm_open() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (fstat(fd, &st) < 0) {
        pa_log("fstat() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (st.st_size <= 0 || st.st_size > MAX_SHM_SIZE) {
        pa_log("Invalid shared memory segment size");
        goto fail;
    }

    m->size = st.st_size;
        
    if ((m->ptr = mmap(NULL, m->size, PROT_READ, MAP_SHARED, fd, 0)) == MAP_FAILED) {
        pa_log("mmap() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    m->do_unlink = 0;
    m->shared = 1;
    
    close(fd);
    
    return 0;
    
fail:
    if (fd >= 0)
        close(fd);

    return -1;
}
