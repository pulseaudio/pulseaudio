/***
    This file is part of PulseAudio.

    Copyright 2006 Lennart Poettering
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

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <dirent.h>
#include <signal.h>

#ifdef HAVE_SYS_MMAN_H
#include <sys/mman.h>
#endif

#include <pulse/xmalloc.h>
#include <pulse/gccmacro.h>

#include <pulsecore/core-error.h>
#include <pulsecore/log.h>
#include <pulsecore/random.h>
#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>
#include <pulsecore/atomic.h>

#include "shm.h"

#if defined(__linux__) && !defined(MADV_REMOVE)
#define MADV_REMOVE 9
#endif

#define MAX_SHM_SIZE (PA_ALIGN(1024*1024*64))

#ifdef __linux__
/* On Linux we know that the shared memory blocks are files in
 * /dev/shm. We can use that information to list all blocks and
 * cleanup unused ones */
#define SHM_PATH "/dev/shm/"
#else
#undef SHM_PATH
#endif

#define SHM_MARKER ((int) 0xbeefcafe)

/* We now put this SHM marker at the end of each segment. It's
 * optional, to not require a reboot when upgrading, though */
struct shm_marker {
    pa_atomic_t marker; /* 0xbeefcafe */
    pa_atomic_t pid;
    uint64_t _reserved1;
    uint64_t _reserved2;
    uint64_t _reserved3;
    uint64_t _reserved4;
} PA_GCC_PACKED;

static char *segment_name(char *fn, size_t l, unsigned id) {
    pa_snprintf(fn, l, "/pulse-shm-%u", id);
    return fn;
}

int pa_shm_create_rw(pa_shm *m, size_t size, pa_bool_t shared, mode_t mode) {
    char fn[32];
    int fd = -1;

    pa_assert(m);
    pa_assert(size > 0);
    pa_assert(size <= MAX_SHM_SIZE);
    pa_assert(mode >= 0600);

    /* Each time we create a new SHM area, let's first drop all stale
     * ones */
    pa_shm_cleanup();

    /* Round up to make it aligned */
    size = PA_ALIGN(size);

    if (!shared) {
        m->id = 0;
        m->size = size;

#ifdef MAP_ANONYMOUS
        if ((m->ptr = mmap(NULL, m->size, PROT_READ|PROT_WRITE, MAP_ANONYMOUS|MAP_PRIVATE, -1, (off_t) 0)) == MAP_FAILED) {
            pa_log("mmap() failed: %s", pa_cstrerror(errno));
            goto fail;
        }
#elif defined(HAVE_POSIX_MEMALIGN)
        {
            int r;

            if ((r = posix_memalign(&m->ptr, PA_PAGE_SIZE, size)) < 0) {
                pa_log("posix_memalign() failed: %s", pa_cstrerror(r));
                goto fail;
            }
        }
#else
        m->ptr = pa_xmalloc(m->size);
#endif

        m->do_unlink = FALSE;

    } else {
#ifdef HAVE_SHM_OPEN
        struct shm_marker *marker;

        pa_random(&m->id, sizeof(m->id));
        segment_name(fn, sizeof(fn), m->id);

        if ((fd = shm_open(fn, O_RDWR|O_CREAT|O_EXCL, mode & 0444)) < 0) {
            pa_log("shm_open() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        m->size = size + PA_ALIGN(sizeof(struct shm_marker));

        if (ftruncate(fd, (off_t) m->size) < 0) {
            pa_log("ftruncate() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        if ((m->ptr = mmap(NULL, m->size, PROT_READ|PROT_WRITE, MAP_SHARED, fd, (off_t) 0)) == MAP_FAILED) {
            pa_log("mmap() failed: %s", pa_cstrerror(errno));
            goto fail;
        }

        /* We store our PID at the end of the shm block, so that we
         * can check for dead shm segments later */
        marker = (struct shm_marker*) ((uint8_t*) m->ptr + m->size - PA_ALIGN(sizeof(struct shm_marker)));
        pa_atomic_store(&marker->pid, (int) getpid());
        pa_atomic_store(&marker->marker, SHM_MARKER);

        pa_assert_se(close(fd) == 0);
        m->do_unlink = TRUE;
#else
        return -1;
#endif
    }

    m->shared = shared;

    return 0;

fail:

#ifdef HAVE_SHM_OPEN
    if (fd >= 0) {
        shm_unlink(fn);
        pa_close(fd);
    }
#endif

    return -1;
}

void pa_shm_free(pa_shm *m) {
    pa_assert(m);
    pa_assert(m->ptr);
    pa_assert(m->size > 0);

#ifdef MAP_FAILED
    pa_assert(m->ptr != MAP_FAILED);
#endif

    if (!m->shared) {
#ifdef MAP_ANONYMOUS
        if (munmap(m->ptr, m->size) < 0)
            pa_log("munmap() failed: %s", pa_cstrerror(errno));
#elif defined(HAVE_POSIX_MEMALIGN)
        free(m->ptr);
#else
        pa_xfree(m->ptr);
#endif
    } else {
#ifdef HAVE_SHM_OPEN
        if (munmap(m->ptr, m->size) < 0)
            pa_log("munmap() failed: %s", pa_cstrerror(errno));

        if (m->do_unlink) {
            char fn[32];

            segment_name(fn, sizeof(fn), m->id);

            if (shm_unlink(fn) < 0)
                pa_log(" shm_unlink(%s) failed: %s", fn, pa_cstrerror(errno));
        }
#else
        /* We shouldn't be here without shm support */
        pa_assert_not_reached();
#endif
    }

    memset(m, 0, sizeof(*m));
}

void pa_shm_punch(pa_shm *m, size_t offset, size_t size) {
    void *ptr;
    size_t o, ps;

    pa_assert(m);
    pa_assert(m->ptr);
    pa_assert(m->size > 0);
    pa_assert(offset+size <= m->size);

#ifdef MAP_FAILED
    pa_assert(m->ptr != MAP_FAILED);
#endif

    /* You're welcome to implement this as NOOP on systems that don't
     * support it */

    /* Align this to multiples of the page size */
    ptr = (uint8_t*) m->ptr + offset;
    o = (size_t) ((uint8_t*) ptr - (uint8_t*) PA_PAGE_ALIGN_PTR(ptr));

    if (o > 0) {
        ps = PA_PAGE_SIZE;
        ptr = (uint8_t*) ptr + (ps - o);
        size -= ps - o;
    }

#ifdef MADV_REMOVE
    if (madvise(ptr, size, MADV_REMOVE) >= 0)
        return;
#endif

#ifdef MADV_FREE
    if (madvise(ptr, size, MADV_FREE) >= 0)
        return;
#endif

#ifdef MADV_DONTNEED
    pa_assert_se(madvise(ptr, size, MADV_DONTNEED) == 0);
#elif defined(POSIX_MADV_DONTNEED)
    pa_assert_se(posix_madvise(ptr, size, POSIX_MADV_DONTNEED) == 0);
#endif
}

#ifdef HAVE_SHM_OPEN

int pa_shm_attach_ro(pa_shm *m, unsigned id) {
    char fn[32];
    int fd = -1;
    struct stat st;

    pa_assert(m);

    segment_name(fn, sizeof(fn), m->id = id);

    if ((fd = shm_open(fn, O_RDONLY, 0)) < 0) {
        if (errno != EACCES)
            pa_log("shm_open() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (fstat(fd, &st) < 0) {
        pa_log("fstat() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    if (st.st_size <= 0 ||
        st.st_size > (off_t) (MAX_SHM_SIZE+PA_ALIGN(sizeof(struct shm_marker))) ||
        PA_ALIGN((size_t) st.st_size) != (size_t) st.st_size) {
        pa_log("Invalid shared memory segment size");
        goto fail;
    }

    m->size = (size_t) st.st_size;

    if ((m->ptr = mmap(NULL, m->size, PROT_READ, MAP_SHARED, fd, (off_t) 0)) == MAP_FAILED) {
        pa_log("mmap() failed: %s", pa_cstrerror(errno));
        goto fail;
    }

    m->do_unlink = 0;
    m->shared = 1;

    pa_assert_se(pa_close(fd) == 0);

    return 0;

fail:
    if (fd >= 0)
        pa_close(fd);

    return -1;
}

#else /* HAVE_SHM_OPEN */

int pa_shm_attach_ro(pa_shm *m, unsigned id) {
    return -1;
}

#endif /* HAVE_SHM_OPEN */

int pa_shm_cleanup(void) {

#ifdef HAVE_SHM_OPEN
#ifdef SHM_PATH
    DIR *d;
    struct dirent *de;

    if (!(d = opendir(SHM_PATH))) {
        pa_log_warn("Failed to read "SHM_PATH": %s", pa_cstrerror(errno));
        return -1;
    }

    while ((de = readdir(d))) {
        pa_shm seg;
        unsigned id;
        pid_t pid;
        char fn[128];
        struct shm_marker *m;

        if (strncmp(de->d_name, "pulse-shm-", 10))
            continue;

        if (pa_atou(de->d_name + 10, &id) < 0)
            continue;

        if (pa_shm_attach_ro(&seg, id) < 0)
            continue;

        if (seg.size < PA_ALIGN(sizeof(struct shm_marker))) {
            pa_shm_free(&seg);
            continue;
        }

        m = (struct shm_marker*) ((uint8_t*) seg.ptr + seg.size - PA_ALIGN(sizeof(struct shm_marker)));

        if (pa_atomic_load(&m->marker) != SHM_MARKER) {
            pa_shm_free(&seg);
            continue;
        }

        if (!(pid = (pid_t) pa_atomic_load(&m->pid))) {
            pa_shm_free(&seg);
            continue;
        }

        if (kill(pid, 0) == 0 || errno != ESRCH) {
            pa_shm_free(&seg);
            continue;
        }

        pa_shm_free(&seg);

        /* Ok, the owner of this shms segment is dead, so, let's remove the segment */
        segment_name(fn, sizeof(fn), id);

        if (shm_unlink(fn) < 0 && errno != EACCES && errno != ENOENT)
            pa_log_warn("Failed to remove SHM segment %s: %s\n", fn, pa_cstrerror(errno));
    }

    closedir(d);
#endif /* SHM_PATH */
#endif /* HAVE_SHM_OPEN */

    return 0;
}
