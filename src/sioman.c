#include <assert.h>
#include "sioman.h"

static int stdio_inuse = 0;

int pa_stdio_acquire(void) {
    if (stdio_inuse)
        return -1;

    stdio_inuse = 1;
    return 0;
}

void pa_stdio_release(void) {
    assert(stdio_inuse);
    stdio_inuse = 0;
} 
