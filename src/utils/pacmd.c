/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
  or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public License
  along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <assert.h>
#include <signal.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <sys/un.h>

#include <polypcore/util.h>
#include <polypcore/log.h>
#include <polypcore/pid.h>

int main(PA_GCC_UNUSED int main, PA_GCC_UNUSED char*argv[]) {
    pid_t pid ;
    int fd = -1;
    int ret = 1, i;
    struct sockaddr_un sa;
    char ibuf[256], obuf[256];
    size_t ibuf_index, ibuf_length, obuf_index, obuf_length;
    fd_set ifds, ofds;

    if (pa_pid_file_check_running(&pid) < 0) {
        pa_log(__FILE__": no Polypaudio daemon running\n");
        goto fail;
    }

    if ((fd = socket(PF_UNIX, SOCK_STREAM, 0)) < 0) {
        pa_log(__FILE__": socket(PF_UNIX, SOCK_STREAM, 0): %s\n", strerror(errno));
        goto fail;
    }

    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    pa_runtime_path("cli", sa.sun_path, sizeof(sa.sun_path));

    for (i = 0; i < 5; i++) {
        int r;
        
        if ((r = connect(fd, (struct sockaddr*) &sa, sizeof(sa))) < 0 && (errno != ECONNREFUSED && errno != ENOENT)) {
            pa_log(__FILE__": connect() failed: %s\n", strerror(errno));
            goto fail;
        }
            
        if (r >= 0)
            break;

        if (pa_pid_file_kill(SIGUSR2, NULL) < 0) {
            pa_log(__FILE__": failed to kill Polypaudio daemon.\n");
            goto fail;
        }

        pa_msleep(50);
    }

    if (i >= 5) {
        pa_log(__FILE__": daemon not responding.\n");
        goto fail;
    }

    ibuf_index = ibuf_length = obuf_index = obuf_length = 0;


    FD_ZERO(&ifds);
    FD_SET(0, &ifds);
    FD_SET(fd, &ifds);

    FD_ZERO(&ofds);
    
    for (;;) {
        if (select(FD_SETSIZE, &ifds, &ofds, NULL, NULL) < 0) {
            pa_log(__FILE__": select() failed: %s\n", strerror(errno));
            goto fail;
        }

        if (FD_ISSET(0, &ifds)) {
            ssize_t r;
            assert(!ibuf_length);
            
            if ((r = read(0, ibuf, sizeof(ibuf))) <= 0) {
                if (r == 0)
                    break;
                
                pa_log(__FILE__": read() failed: %s\n", strerror(errno));
                goto fail;
            }
            
            ibuf_length = (size_t) r;
            ibuf_index = 0;
        }
        
        if (FD_ISSET(fd, &ifds)) {
            ssize_t r;
            assert(!obuf_length);

            if ((r = read(fd, obuf, sizeof(obuf))) <= 0) {
                if (r == 0)
                    break;
                
                pa_log(__FILE__": read() failed: %s\n", strerror(errno));
                goto fail;
            }

            obuf_length = (size_t) r;
            obuf_index = 0;
        }

        if (FD_ISSET(1, &ofds)) {
            ssize_t r;
            assert(obuf_length);
            
            if ((r = write(1, obuf + obuf_index, obuf_length)) < 0) {
                pa_log(__FILE__": write() failed: %s\n", strerror(errno));
                goto fail;
            }
            
            obuf_length -= (size_t) r;
            obuf_index += obuf_index;

        }

        if (FD_ISSET(fd, &ofds)) {
            ssize_t r;
            assert(ibuf_length);
            
            if ((r = write(fd, ibuf + ibuf_index, ibuf_length)) < 0) {
                pa_log(__FILE__": write() failed: %s\n", strerror(errno));
                goto fail;
            }
            
            ibuf_length -= (size_t) r;
            ibuf_index += obuf_index;

        }

        FD_ZERO(&ifds);
        FD_ZERO(&ofds);
        
        if (obuf_length <= 0)
            FD_SET(fd, &ifds);
        else
            FD_SET(1, &ofds);
        
        if (ibuf_length <= 0)
            FD_SET(0, &ifds);
        else
            FD_SET(fd, &ofds);
    }
    

    ret = 0;
    
fail:
    if (fd >= 0)
        close(fd);
    
    return ret;
}
