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
#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

#include "oss-util.h"
#include "util.h"
#include "log.h"

int pa_oss_open(const char *device, int *mode, int* pcaps) {
    int fd = -1;
    assert(device && mode && (*mode == O_RDWR || *mode == O_RDONLY || *mode == O_WRONLY));

    if (*mode == O_RDWR) {
        if ((fd = open(device, O_RDWR|O_NDELAY)) >= 0) {
            int dcaps, *tcaps;
            ioctl(fd, SNDCTL_DSP_SETDUPLEX, 0);

            tcaps = pcaps ? pcaps : &dcaps;
            
            if (ioctl(fd, SNDCTL_DSP_GETCAPS, tcaps) < 0) {
                pa_log(__FILE__": SNDCTL_DSP_GETCAPS: %s\n", strerror(errno));
                goto fail;
            }

            if (*tcaps & DSP_CAP_DUPLEX)
                return fd;

            close(fd);
        }
        
        if ((fd = open(device, (*mode = O_WRONLY)|O_NDELAY)) < 0) {
            if ((fd = open(device, (*mode = O_RDONLY)|O_NDELAY)) < 0) {
                pa_log(__FILE__": open('%s'): %s\n", device, strerror(errno));
                goto fail;
            }
        }
    } else {
        if ((fd = open(device, *mode|O_NDELAY)) < 0) {
            pa_log(__FILE__": open('%s'): %s\n", device, strerror(errno));
            goto fail;
        }
    } 

    if (pcaps) {
        if (ioctl(fd, SNDCTL_DSP_GETCAPS, pcaps) < 0) {
            pa_log(__FILE__": SNDCTL_DSP_GETCAPS: %s\n", strerror(errno));
            goto fail;
        }
    }

    pa_fd_set_cloexec(fd, 1);
    
    return fd;

fail:
    if (fd >= 0)
        close(fd);
    return fd;
}

int pa_oss_auto_format(int fd, struct pa_sample_spec *ss) {
    int format, channels, speed, reqformat;
    static const int format_trans[] = {
        [PA_SAMPLE_U8] = AFMT_U8,
        [PA_SAMPLE_ALAW] = AFMT_A_LAW,
        [PA_SAMPLE_ULAW] = AFMT_MU_LAW,
        [PA_SAMPLE_S16LE] = AFMT_S16_LE,
        [PA_SAMPLE_S16BE] = AFMT_S16_BE,
        [PA_SAMPLE_FLOAT32LE] = AFMT_QUERY, /* not supported */
        [PA_SAMPLE_FLOAT32BE] = AFMT_QUERY, /* not supported */
    };

    assert(fd >= 0 && ss);

    reqformat = format = format_trans[ss->format];
    if (reqformat == AFMT_QUERY || ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != reqformat) {
        format = AFMT_S16_NE;
        if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != AFMT_S16_NE) {
            int f = AFMT_S16_NE == AFMT_S16_LE ? AFMT_S16_BE : AFMT_S16_LE;
            format = f;
            if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != f) {
                format = AFMT_U8;
                if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != AFMT_U8) {
                    pa_log(__FILE__": SNDCTL_DSP_SETFMT: %s\n", format != AFMT_U8 ? "No supported sample format" : strerror(errno));
                    return -1;
                } else
                    ss->format = PA_SAMPLE_U8;
            } else
                ss->format = f == AFMT_S16_LE ? PA_SAMPLE_S16LE : PA_SAMPLE_S16BE;
        } else
            ss->format = PA_SAMPLE_S16NE;
    }
        
    channels = ss->channels;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0) {
        pa_log(__FILE__": SNDCTL_DSP_CHANNELS: %s\n", strerror(errno));
        return -1;
    }
    assert(channels);
    ss->channels = channels;

    speed = ss->rate;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &speed) < 0) {
        pa_log(__FILE__": SNDCTL_DSP_SPEED: %s\n", strerror(errno));
        return -1;
    }
    assert(speed);
    ss->rate = speed;

    return 0;
}

static int simple_log2(int v) {
    int k = 0;

    for (;;) {
        v >>= 1;
        if (!v) break;
        k++;
    }
    
    return k;
}

int pa_oss_set_fragments(int fd, int nfrags, int frag_size) {
    int arg;
    arg = ((int) nfrags << 16) | simple_log2(frag_size);
    
    if (ioctl(fd, SNDCTL_DSP_SETFRAGMENT, &arg) < 0) {
        pa_log(__FILE__": SNDCTL_DSP_SETFRAGMENT: %s\n", strerror(errno));
        return -1;
    }

    return 0;
}
