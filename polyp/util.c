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

#include <stdarg.h>
#include <stdlib.h>
#include <signal.h>
#include <errno.h>
#include <assert.h>
#include <string.h>
#include <stdio.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>

#include "util.h"

void pa_make_nonblock_fd(int fd) {
    int v;

    if ((v = fcntl(fd, F_GETFL)) >= 0)
        if (!(v & O_NONBLOCK))
            fcntl(fd, F_SETFL, v|O_NONBLOCK);
}

int pa_make_secure_dir(const char* dir) {
    struct stat st;

    if (mkdir(dir, 0700) < 0) 
        if (errno != EEXIST)
            return -1;
    
    if (lstat(dir, &st) < 0) 
        goto fail;
    
    if (!S_ISDIR(st.st_mode) || (st.st_uid != getuid()) || ((st.st_mode & 0777) != 0700))
        goto fail;
    
    return 0;
    
fail:
    rmdir(dir);
    return -1;
}

ssize_t pa_loop_read(int fd, void*data, size_t size) {
    ssize_t ret = 0;
    assert(fd >= 0 && data && size);

    while (size > 0) {
        ssize_t r;

        if ((r = read(fd, data, size)) < 0)
            return r;

        if (r == 0)
            break;
        
        ret += r;
        data += r;
        size -= r;
    }

    return ret;
}

ssize_t pa_loop_write(int fd, const void*data, size_t size) {
    ssize_t ret = 0;
    assert(fd >= 0 && data && size);

    while (size > 0) {
        ssize_t r;

        if ((r = write(fd, data, size)) < 0)
            return r;

        if (r == 0)
            break;
        
        ret += r;
        data += r;
        size -= r;
    }

    return ret;
}

void pa_check_for_sigpipe(void) {
    struct sigaction sa;

    if (sigaction(SIGPIPE, NULL, &sa) < 0) {
        fprintf(stderr, __FILE__": sigaction() failed: %s\n", strerror(errno));
        return;
    }
        
    if (sa.sa_handler == SIG_DFL)
        fprintf(stderr, "polypaudio: WARNING: SIGPIPE is not trapped. This might cause malfunction!\n");
}

/* The following is based on an example from the GNU libc documentation */
char *pa_sprintf_malloc(const char *format, ...) {
    int  size = 100;
    char *c = NULL;
    
    assert(format);
    
    for(;;) {
        int r;
        va_list ap;

        c = realloc(c, size);
        assert(c);

        va_start(ap, format);
        r = vsnprintf(c, size, format, ap);
        va_end(ap);
        
        if (r > -1 && r < size)
            return c;

        if (r > -1)    /* glibc 2.1 */
            size = r+1; 
        else           /* glibc 2.0 */
            size *= 2;
    }
}
