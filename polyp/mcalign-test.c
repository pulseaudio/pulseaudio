#include <assert.h>
#include <unistd.h>
#include <sys/types.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "util.h"
#include "mcalign.h"

int main(int argc, char *argv[]) {
    struct pa_mcalign *a = pa_mcalign_new(11, NULL);
    struct pa_memchunk c;

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
        fprintf(stderr, "Read %u bytes\n", r);

        c.index += r;

        if (c.index >= c.memblock->length) {
            pa_memblock_unref(c.memblock);
            pa_memchunk_reset(&c);
        }

        for (;;) {
            struct pa_memchunk t;

            if (pa_mcalign_pop(a, &t) < 0)
                break;

            pa_loop_write(STDOUT_FILENO, (uint8_t*) t.memblock->data + t.index, t.length);
            fprintf(stderr, "Wrote %u bytes.\n", t.length);

            pa_memblock_unref(t.memblock);
        }
    }

    pa_mcalign_free(a);

    if (c.memblock)
        pa_memblock_unref(c.memblock);
}
