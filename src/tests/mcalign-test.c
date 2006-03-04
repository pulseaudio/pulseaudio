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

#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include <polypcore/util.h>
#include <polypcore/mcalign.h>
#include <polypcore/gccmacro.h>

/* A simple program for testing pa_mcalign */

int main(PA_GCC_UNUSED int argc, PA_GCC_UNUSED char *argv[]) {
    pa_mcalign *a = pa_mcalign_new(11, NULL);
    pa_memchunk c;

    pa_memchunk_reset(&c);

    srand(time(NULL));

    for (;;) {
        ssize_t r;
        size_t l;

        if (!c.memblock) {
            c.memblock = pa_memblock_new(2048, NULL);
            c.index = c.length = 0;
        }

        assert(c.index < c.memblock->length);

        l = c.memblock->length - c.index;

        l = l <= 1 ? l : rand() % (l-1) +1 ;
        
        if ((r = read(STDIN_FILENO, (uint8_t*) c.memblock->data + c.index, l)) <= 0) {
            fprintf(stderr, "read() failed: %s\n", r < 0 ? strerror(errno) : "EOF");
            break;
        }

        c.length = r;
        pa_mcalign_push(a, &c);
        fprintf(stderr, "Read %d bytes\n", r);

        c.index += r;

        if (c.index >= c.memblock->length) {
            pa_memblock_unref(c.memblock);
            pa_memchunk_reset(&c);
        }

        for (;;) {
            pa_memchunk t;

            if (pa_mcalign_pop(a, &t) < 0)
                break;

            pa_loop_write(STDOUT_FILENO, (uint8_t*) t.memblock->data + t.index, t.length);
            fprintf(stderr, "Wrote %lu bytes.\n", (unsigned long) t.length);

            pa_memblock_unref(t.memblock);
        }
    }

    pa_mcalign_free(a);

    if (c.memblock)
        pa_memblock_unref(c.memblock);

    return 0;
}
