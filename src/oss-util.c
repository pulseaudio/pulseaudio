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

int pa_oss_open(const char *device, int *mode, int* pcaps) {
    int fd = -1;
    assert(device && mode && (*mode == O_RDWR || *mode == O_RDONLY || *mode == O_WRONLY));

    if (*mode == O_RDWR) {
        if ((fd = open(device, O_RDWR|O_NDELAY)) >= 0) {
            int dcaps, *tcaps;
            ioctl(fd, SNDCTL_DSP_SETDUPLEX, 0);

            tcaps = pcaps ? pcaps : &dcaps;
            
            if (ioctl(fd, SNDCTL_DSP_GETCAPS, tcaps) < 0) {
                fprintf(stderr, __FILE__": SNDCTL_DSP_GETCAPS: %s\n", strerror(errno));
                goto fail;
            }

            if (*tcaps & DSP_CAP_DUPLEX)
                return fd;

            close(fd);
        }
        
        if ((fd = open(device, (*mode = O_WRONLY)|O_NDELAY)) < 0) {
            if ((fd = open(device, (*mode = O_RDONLY)|O_NDELAY)) < 0) {
                fprintf(stderr, __FILE__": open('%s'): %s\n", device, strerror(errno));
                goto fail;
            }
        }
    } else {
        if ((fd = open(device, *mode|O_NDELAY)) < 0) {
            fprintf(stderr, __FILE__": open('%s'): %s\n", device, strerror(errno));
            goto fail;
        }
    } 

    if (pcaps) {
        if (ioctl(fd, SNDCTL_DSP_GETCAPS, pcaps) < 0) {
            fprintf(stderr, "SNDCTL_DSP_GETCAPS: %s\n", strerror(errno));
            goto fail;
        }
    }
    
    return fd;

fail:
    if (fd >= 0)
        close(fd);
    return fd;
}

int pa_oss_auto_format(int fd, struct pa_sample_spec *ss) {
    int format, channels, speed;

    assert(fd >= 0 && ss);
    
    format = AFMT_S16_NE;
    if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != AFMT_S16_NE) {
        int f = AFMT_S16_NE == AFMT_S16_LE ? AFMT_S16_BE : AFMT_S16_LE;
        format = f;
        if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != f) {
            format = AFMT_U8;
            if (ioctl(fd, SNDCTL_DSP_SETFMT, &format) < 0 || format != AFMT_U8) {
                fprintf(stderr, "SNDCTL_DSP_SETFMT: %s\n", format != AFMT_U8 ? "No supported sample format" : strerror(errno));
                return -1;
            } else
                ss->format = PA_SAMPLE_U8;
        } else
            ss->format = f == AFMT_S16_LE ? PA_SAMPLE_S16LE : PA_SAMPLE_S16BE;
    } else
        ss->format = PA_SAMPLE_S16NE;
        
    channels = 2;
    if (ioctl(fd, SNDCTL_DSP_CHANNELS, &channels) < 0) {
        fprintf(stderr, "SNDCTL_DSP_CHANNELS: %s\n", strerror(errno));
        return -1;
    }
    assert(channels);
    ss->channels = channels;

    speed = 44100;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &speed) < 0) {
        fprintf(stderr, "SNDCTL_DSP_SPEED: %s\n", strerror(errno));
        return -1;
    }
    assert(speed);
    ss->rate = speed;

    return 0;
}
