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
#include <fcntl.h>
#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <inttypes.h>
#include <stdlib.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>

#include <polypcore/util.h>
#include <polypcore/log.h>
#include <polypcore/random.h>

#include "authkey.h"

/* Generate a new authorization key, store it in file fd and return it in *data  */
static int generate(int fd, void *ret_data, size_t length) {
    ssize_t r;
    assert(fd >= 0 && ret_data && length);

    pa_random(ret_data, length);

    lseek(fd, 0, SEEK_SET);
    ftruncate(fd, 0);

    if ((r = pa_loop_write(fd, ret_data, length)) < 0 || (size_t) r != length) {
        pa_log(__FILE__": failed to write cookie file: %s", strerror(errno));
        return -1;
    }

    return 0;
}

#ifndef O_BINARY
#define O_BINARY 0
#endif

/* Load an euthorization cookie from file fn and store it in data. If
 * the cookie file doesn't exist, create it */
static int load(const char *fn, void *data, size_t length) {
    int fd = -1;
    int writable = 1;
    int unlock = 0, ret = -1;
    ssize_t r;
    assert(fn && data && length);

    if ((fd = open(fn, O_RDWR|O_CREAT|O_BINARY, S_IRUSR|S_IWUSR)) < 0) {
        if (errno != EACCES || (fd = open(fn, O_RDONLY|O_BINARY)) < 0) {
            pa_log(__FILE__": failed to open cookie file '%s': %s", fn, strerror(errno));
            goto finish;
        } else
            writable = 0;
    }

    unlock = pa_lock_fd(fd, 1) >= 0;

    if ((r = pa_loop_read(fd, data, length)) < 0) {
        pa_log(__FILE__": failed to read cookie file '%s': %s", fn, strerror(errno));
        goto finish;
    }

    if ((size_t) r != length) {
        pa_log_debug(__FILE__": got %d bytes from cookie file '%s', expected %d", (int)r, fn, (int)length);
        
        if (!writable) {
            pa_log(__FILE__": unable to write cookie to read only file");
            goto finish;
        }
        
        if (generate(fd, data, length) < 0)
            goto finish;
    }

    ret = 0;
    
finish:

    if (fd >= 0) {
        
        if (unlock)
            pa_lock_fd(fd, 0);
        
        close(fd);
    }

    return ret;
}

/* Load a cookie from a cookie file. If the file doesn't exist, create it. */
int pa_authkey_load(const char *path, void *data, size_t length) {
    int ret;

    assert(path && data && length);

    ret = load(path, data, length);

    if (ret < 0)
        pa_log(__FILE__": Failed to load authorization key '%s': %s", path,
               (ret == -1) ? strerror(errno) : "file corrupt");

    return ret;
}

/* If the specified file path starts with / return it, otherwise
 * return path prepended with home directory */
static const char *normalize_path(const char *fn, char *s, size_t l) {
    assert(fn && s && l > 0);

#ifndef OS_IS_WIN32
    if (fn[0] != '/') {
#else
    if (strlen(fn) < 3 || !isalpha(fn[0]) || fn[1] != ':' || fn[2] != '\\') {
#endif
        char homedir[PATH_MAX];
        if (!pa_get_home_dir(homedir, sizeof(homedir)))
            return NULL;
        
#ifndef OS_IS_WIN32
        snprintf(s, l, "%s/%s", homedir, fn);
#else
        snprintf(s, l, "%s\\%s", homedir, fn);
#endif
        return s;
    }

    return fn;
}

/* Load a cookie from a file in the home directory. If the specified
 * path starts with /, use it as absolute path instead. */
int pa_authkey_load_auto(const char *fn, void *data, size_t length) {
    char path[PATH_MAX];
    const char *p;
    assert(fn && data && length);

    if (!(p = normalize_path(fn, path, sizeof(path))))
        return -2;
        
    return pa_authkey_load(p, data, length);
}

/* Store the specified cookie in the speicified cookie file */
int pa_authkey_save(const char *fn, const void *data, size_t length) {
    int fd = -1;
    int unlock = 0, ret = -1;
    ssize_t r;
    char path[PATH_MAX];
    const char *p;
    assert(fn && data && length);

    if (!(p = normalize_path(fn, path, sizeof(path))))
        return -2;

    if ((fd = open(p, O_RDWR|O_CREAT, S_IRUSR|S_IWUSR)) < 0) {
        pa_log(__FILE__": failed to open cookie file '%s': %s", fn, strerror(errno));
        goto finish;
    }

    unlock = pa_lock_fd(fd, 1) >= 0;

    if ((r = pa_loop_write(fd, data, length)) < 0 || (size_t) r != length) {
        pa_log(__FILE__": failed to read cookie file '%s': %s", fn, strerror(errno));
        goto finish;
    }

    ret = 0;
    
finish:

    if (fd >= 0) {
        
        if (unlock)
            pa_lock_fd(fd, 0);
        
        close(fd);
    }

    return ret;
}
