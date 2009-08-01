/***
  This file is part of PulseAudio.

  Copyright 2004-2006 Lennart Poettering
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

#include <pulse/util.h>
#include <pulse/xmalloc.h>
#include <pulsecore/core-error.h>
#include <pulsecore/core-util.h>
#include <pulsecore/log.h>
#include <pulsecore/random.h>
#include <pulsecore/macro.h>

#include "authkey.h"

/* Generate a new authorization key, store it in file fd and return it in *data  */
static int generate(int fd, void *ret_data, size_t length) {
    ssize_t r;

    pa_assert(fd >= 0);
    pa_assert(ret_data);
    pa_assert(length > 0);

    pa_random(ret_data, length);

    lseek(fd, (off_t) 0, SEEK_SET);
    (void) ftruncate(fd, (off_t) 0);

    if ((r = pa_loop_write(fd, ret_data, length, NULL)) < 0 || (size_t) r != length) {
        pa_log("Failed to write cookie file: %s", pa_cstrerror(errno));
        return -1;
    }

    return 0;
}

#ifndef O_BINARY
#define O_BINARY 0
#endif

#ifndef O_NOCTTY
#define O_NOCTTY 0
#endif

/* Load an euthorization cookie from file fn and store it in data. If
 * the cookie file doesn't exist, create it */
static int load(const char *fn, void *data, size_t length) {
    int fd = -1;
    int writable = 1;
    int unlock = 0, ret = -1;
    ssize_t r;

    pa_assert(fn);
    pa_assert(data);
    pa_assert(length > 0);

    if ((fd = open(fn, O_RDWR|O_CREAT|O_BINARY|O_NOCTTY, S_IRUSR|S_IWUSR)) < 0) {

        if (errno != EACCES || (fd = open(fn, O_RDONLY|O_BINARY|O_NOCTTY)) < 0) {
            pa_log_warn("Failed to open cookie file '%s': %s", fn, pa_cstrerror(errno));
            goto finish;
        } else
            writable = 0;
    }

    unlock = pa_lock_fd(fd, 1) >= 0;

    if ((r = pa_loop_read(fd, data, length, NULL)) < 0) {
        pa_log("Failed to read cookie file '%s': %s", fn, pa_cstrerror(errno));
        goto finish;
    }

    if ((size_t) r != length) {
        pa_log_debug("Got %d bytes from cookie file '%s', expected %d", (int) r, fn, (int) length);

        if (!writable) {
            pa_log_warn("Unable to write cookie to read-only file");
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

        if (pa_close(fd) < 0) {
            pa_log_warn("Failed to close cookie file: %s", pa_cstrerror(errno));
            ret = -1;
        }
    }

    return ret;
}

/* Load a cookie from a cookie file. If the file doesn't exist, create it. */
int pa_authkey_load(const char *path, void *data, size_t length) {
    int ret;

    pa_assert(path);
    pa_assert(data);
    pa_assert(length > 0);

    if ((ret = load(path, data, length)) < 0)
        pa_log_warn("Failed to load authorization key '%s': %s", path, (ret < 0) ? pa_cstrerror(errno) : "File corrupt");

    return ret;
}

/* If the specified file path starts with / return it, otherwise
 * return path prepended with home directory */
static char *normalize_path(const char *fn) {

    pa_assert(fn);

#ifndef OS_IS_WIN32
    if (fn[0] != '/') {
#else
    if (strlen(fn) < 3 || !isalpha(fn[0]) || fn[1] != ':' || fn[2] != '\\') {
#endif
        char *homedir, *s;

        if (!(homedir = pa_get_home_dir_malloc()))
            return NULL;

        s = pa_sprintf_malloc("%s" PA_PATH_SEP "%s", homedir, fn);
        pa_xfree(homedir);

        return s;
    }

    return pa_xstrdup(fn);
}

/* Load a cookie from a file in the home directory. If the specified
 * path starts with /, use it as absolute path instead. */
int pa_authkey_load_auto(const char *fn, void *data, size_t length) {
    char *p;
    int ret;

    pa_assert(fn);
    pa_assert(data);
    pa_assert(length > 0);

    if (!(p = normalize_path(fn)))
        return -2;

    ret = pa_authkey_load(p, data, length);
    pa_xfree(p);

    return ret;
}

/* Store the specified cookie in the specified cookie file */
int pa_authkey_save(const char *fn, const void *data, size_t length) {
    int fd = -1;
    int unlock = 0, ret = -1;
    ssize_t r;
    char *p;

    pa_assert(fn);
    pa_assert(data);
    pa_assert(length > 0);

    if (!(p = normalize_path(fn)))
        return -2;

    if ((fd = open(p, O_RDWR|O_CREAT|O_NOCTTY, S_IRUSR|S_IWUSR)) < 0) {
        pa_log_warn("Failed to open cookie file '%s': %s", fn, pa_cstrerror(errno));
        goto finish;
    }

    unlock = pa_lock_fd(fd, 1) >= 0;

    if ((r = pa_loop_write(fd, data, length, NULL)) < 0 || (size_t) r != length) {
        pa_log("Failed to read cookie file '%s': %s", fn, pa_cstrerror(errno));
        goto finish;
    }

    ret = 0;

finish:

    if (fd >= 0) {

        if (unlock)
            pa_lock_fd(fd, 0);

        if (pa_close(fd) < 0) {
            pa_log_warn("Failed to close cookie file: %s", pa_cstrerror(errno));
            ret = -1;
        }
    }

    pa_xfree(p);

    return ret;
}
