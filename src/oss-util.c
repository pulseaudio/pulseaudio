#include <assert.h>
#include <sys/soundcard.h>
#include <sys/ioctl.h>
#include <stdio.h>
#include <errno.h>
#include <string.h>

#include "oss-util.h"

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
