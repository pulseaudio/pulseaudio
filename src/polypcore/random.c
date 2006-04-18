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

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#include <polypcore/util.h>
#include <polypcore/log.h>

#include "random.h"

static int has_whined = 0;

static const char *devices[] = { "/dev/urandom", "/dev/random", NULL };

static int random_proper(void *ret_data, size_t length) {
    assert(ret_data && length);

#ifdef OS_IS_WIN32

    return -1;

#else /* OS_IS_WIN32 */

    int fd, ret;
    ssize_t r = 0;
    const char **device;

    device = devices;

    while (*device) {
        ret = 0;

        if ((fd = open(*device, O_RDONLY)) >= 0) {

            if ((r = pa_loop_read(fd, ret_data, length)) < 0 || (size_t) r != length)
                ret = -1;

            close(fd);
        } else
            ret = -1;

        if (ret == 0)
            break;
    }

    return ret;
#endif /* OS_IS_WIN32 */
}

void pa_random_seed(void) {
    unsigned int seed;

    if (random_proper(&seed, sizeof(unsigned int)) < 0) {
        if (!has_whined)
            pa_log_warn(__FILE__": failed to get proper entropy. Falling back to seeding with current time.");
        has_whined = 1;

        seed = (unsigned int) time(NULL);
    }

    srand(seed);
}

void pa_random(void *ret_data, size_t length) {
    uint8_t *p;
    size_t l;

    assert(ret_data && length);

    if (random_proper(ret_data, length) >= 0)
        return;

    if (!has_whined)
        pa_log_warn(__FILE__": failed to get proper entropy. Falling back to unsecure pseudo RNG.");
    has_whined = 1;

    for (p = ret_data, l = length; l > 0; p++, l--)
        *p = (uint8_t) rand();
}
