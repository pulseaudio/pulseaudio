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

#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <stdlib.h>
#include <assert.h>
#include <time.h>

#include "random.h"
#include "util.h"
#include "log.h"

#define RANDOM_DEVICE "/dev/urandom"

void pa_random(void *ret_data, size_t length) {
    int fd;
    ssize_t r = 0;
    assert(ret_data && length);
    
    if ((fd = open(RANDOM_DEVICE, O_RDONLY)) >= 0) {

        if ((r = pa_loop_read(fd, ret_data, length)) < 0 || (size_t) r != length)
            pa_log_error(__FILE__": failed to read entropy from '%s'\n", RANDOM_DEVICE);

        close(fd);
    }

    if ((size_t) r != length) {
        uint8_t *p;
        size_t l;
        
        pa_log_warn(__FILE__": WARNING: Failed to open entropy device '"RANDOM_DEVICE"': %s"
                    ", falling back to unsecure pseudo RNG.\n", strerror(errno));

        srandom(time(NULL));
        
        for (p = ret_data, l = length; l > 0; p++, l--)
            *p = (uint8_t) random();
    }
}
