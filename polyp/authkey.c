/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>

#include "authkey.h"
#include "util.h"
#include "log.h"

#define RANDOM_DEVICE "/dev/urandom"

static int generate(int fd, void *data, size_t length) {
    int random_fd, ret = -1;
    ssize_t r;
    assert(fd >= 0 && data && length);

    if ((random_fd = open(RANDOM_DEVICE, O_RDONLY)) >= 0) {

        if ((r = pa_loop_read(random_fd, data, length)) < 0 || (size_t) r != length) {
            pa_log(__FILE__": failed to read entropy from '%s'\n", RANDOM_DEVICE);
            goto finish;
        }
        
    } else {
        uint8_t *p;
        size_t l;
        pa_log(__FILE__": WARNING: Failed to open entropy device '"RANDOM_DEVICE"': %s"
               ", falling back to unsecure pseudo RNG.\n", strerror(errno));

        srandom(time(NULL));
        
        for (p = data, l = length; l > 0; p++, l--)
            *p = (uint8_t) random();
    }

    lseek(fd, 0, SEEK_SET);

    if ((r = pa_loop_write(fd, data, length)) < 0 || (size_t) r != length) {
        pa_log(__FILE__": failed to write cookie file\n");
        goto finish;
    }

    ret = 0;

finish:

    if (random_fd >= 0)
        close(random_fd);

    return ret;
}

static int load(const char *fn, void *data, size_t length) {
    int fd = -1;
    int writable = 1;
    assert(fn && data && length);
    int unlock = 0, ret;
    ssize_t r;

    if ((fd = open(fn, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR)) < 0) {
        if (errno != EACCES || (fd = open(fn, O_RDONLY)) < 0) {
            pa_log(__FILE__": failed to open cookie file '%s'\n", fn);
            goto finish;
        } else
            writable = 0;
    }

    unlock = pa_lock_file(fd, 1) >= 0;

    if ((r = pa_loop_read(fd, data, length)) < 0) {
        pa_log(__FILE__": failed to read cookie file '%s'\n", fn);
        goto finish;
    }

    if ((size_t) r != length) {
        
        if (!writable) {
            pa_log(__FILE__": unable to write cookie to read only file\n");
            goto finish;
        }
        
        if (generate(fd, data, length) < 0)
            goto finish;
    }

    ret = 0;
    
finish:

    if (fd >= 0) {
        
        if (unlock)
            pa_lock_file(fd, 0);
        
        close(fd);
    }

    return ret;
}

int pa_authkey_load(const char *path, void *data, size_t length) {
    int ret;

    assert(path && data && length);

    ret = load(path, data, length);

    if (ret < 0)
        pa_log(__FILE__": Failed to load authorization key '%s': %s\n", path,
               (ret == -1) ? strerror(errno) : "file corrupt");

    return ret;
}

int pa_authkey_load_from_home(const char *fn, void *data, size_t length) {
    char *home;
    char path[PATH_MAX];

    assert(fn && data && length);
    
    if (!(home = getenv("HOME")))
        return -2;
    
    snprintf(path, sizeof(path), "%s/%s", home, fn);

    return pa_authkey_load(path, data, length);
}

int pa_authkey_load_auto(const char *fn, void *data, size_t length) {
    assert(fn && data && length);

    if (*fn == '/')
        return pa_authkey_load(fn, data, length);
    else
        return pa_authkey_load_from_home(fn, data, length);
}
