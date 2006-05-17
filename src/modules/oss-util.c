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

#include <polypcore/core-util.h>
#include <polypcore/log.h>

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
                pa_log(__FILE__": SNDCTL_DSP_GETCAPS: %s", strerror(errno));
                goto fail;
            }

            if (*tcaps & DSP_CAP_DUPLEX)
                goto success;

            pa_log_warn(__FILE__": '%s' doesn't support full duplex", device);

            close(fd);
        }
        
        if ((fd = open(device, (*mode = O_WRONLY)|O_NDELAY)) < 0) {
            if ((fd = open(device, (*mode = O_RDONLY)|O_NDELAY)) < 0) {
                pa_log(__FILE__": open('%s'): %s", device, strerror(errno));
                goto fail;
            }
        }
    } else {
        if ((fd = open(device, *mode|O_NDELAY)) < 0) {
            pa_log(__FILE__": open('%s'): %s", device, strerror(errno));
            goto fail;
        }
    } 

success:

    if (pcaps) {
        if (ioctl(fd, SNDCTL_DSP_GETCAPS, pcaps) < 0) {
            pa_log(__FILE__": SNDCTL_DSP_GETCAPS: %s", strerror(errno));
            goto fail;
        }
    }

    pa_fd_set_cloexec(fd, 1);
    
    return fd;

fail:
    if (fd >= 0)
        close(fd);
    return -1;
}

int pa_oss_auto_format(int fd, pa_sample_spec *ss) {
    int format, channels, speed, reqformat;
    static const int format_trans[PA_SAMPLE_MAX] = {
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
                    pa_log(__FILE__": SNDCTL_DSP_SETFMT: %s", format != AFMT_U8 ? "No supported sample format" : strerror(errno));
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
        pa_log(__FILE__": SNDCTL_DSP_CHANNELS: %s", strerror(errno));
        return -1;
    }
    assert(channels);
    ss->channels = channels;

    speed = ss->rate;
    if (ioctl(fd, SNDCTL_DSP_SPEED, &speed) < 0) {
        pa_log(__FILE__": SNDCTL_DSP_SPEED: %s", strerror(errno));
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
        pa_log(__FILE__": SNDCTL_DSP_SETFRAGMENT: %s", strerror(errno));
        return -1;
    }

    return 0;
}

static int pa_oss_get_volume(int fd, int mixer, const pa_sample_spec *ss, pa_cvolume *volume) {
    char cv[PA_CVOLUME_SNPRINT_MAX];
    unsigned vol;

    assert(fd >= 0);
    assert(ss);
    assert(volume);
    
    if (ioctl(fd, mixer, &vol) < 0)
        return -1;

    volume->values[0] = ((vol & 0xFF) * PA_VOLUME_NORM) / 100;

    if ((volume->channels = ss->channels) >= 2)
        volume->values[1] = (((vol >> 8) & 0xFF) * PA_VOLUME_NORM) / 100;

    pa_log_debug(__FILE__": Read mixer settings: %s", pa_cvolume_snprint(cv, sizeof(cv), volume));
    return 0;
}

static int pa_oss_set_volume(int fd, int mixer, const pa_sample_spec *ss, const pa_cvolume *volume) {
    char cv[PA_CVOLUME_SNPRINT_MAX];
    unsigned vol;
    pa_volume_t l, r;

    l = volume->values[0] > PA_VOLUME_NORM ? PA_VOLUME_NORM : volume->values[0];

    vol = (l*100)/PA_VOLUME_NORM;

    if (ss->channels >= 2) {
        r = volume->values[1] > PA_VOLUME_NORM ? PA_VOLUME_NORM : volume->values[1];
        vol |= ((r*100)/PA_VOLUME_NORM) << 8;
    }
    
    if (ioctl(fd, mixer, &vol) < 0)
        return -1;

    pa_log_debug(__FILE__": Wrote mixer settings: %s", pa_cvolume_snprint(cv, sizeof(cv), volume));
    return 0;
}

int pa_oss_get_pcm_volume(int fd, const pa_sample_spec *ss, pa_cvolume *volume) {
    return pa_oss_get_volume(fd, SOUND_MIXER_READ_PCM, ss, volume);
}

int pa_oss_set_pcm_volume(int fd, const pa_sample_spec *ss, const pa_cvolume *volume) {
    return pa_oss_set_volume(fd, SOUND_MIXER_WRITE_PCM, ss, volume);
}

int pa_oss_get_input_volume(int fd, const pa_sample_spec *ss, pa_cvolume *volume) {
    return pa_oss_get_volume(fd, SOUND_MIXER_READ_IGAIN, ss, volume);
}

int pa_oss_set_input_volume(int fd, const pa_sample_spec *ss, const pa_cvolume *volume) {
    return pa_oss_set_volume(fd, SOUND_MIXER_WRITE_IGAIN, ss, volume);
}

int pa_oss_get_hw_description(const char *dev, char *name, size_t l) {
    FILE *f;
    const char *e = NULL;
    int n, r = -1;
    int b = 0;

    if (strncmp(dev, "/dev/dsp", 8) == 0)
        e = dev+8;
    else if (strncmp(dev, "/dev/adsp", 9) == 0)
        e = dev+9;
    else
        return -1;

    if (*e == 0)
        n = 0;
    else if (*e >= '0' && *e <= '9' && *(e+1) == 0)
        n = *e - '0';
    else
        return -1;
    
    if (!(f = fopen("/dev/sndstat", "r")) &&
        !(f = fopen("/proc/sndstat", "r")) &&
        !(f = fopen("/proc/asound/oss/sndstat", "r"))) {

        if (errno != ENOENT)
            pa_log_warn(__FILE__": failed to open OSS sndstat device: %s", strerror(errno));

        return -1;
    }

    while (!feof(f)) {
        char line[64];
        int device;
    
        if (!fgets(line, sizeof(line), f))
            break;

        line[strcspn(line, "\r\n")] = 0;

        if (!b) {
            b = strcmp(line, "Audio devices:") == 0;
            continue;
        }

        if (line[0] == 0)
            break;
        
        if (sscanf(line, "%i: ", &device) != 1)
            continue;

        if (device == n) {
            char *k = strchr(line, ':');
            assert(k);
            k++;
            k += strspn(k, " ");

            if (pa_endswith(k, " (DUPLEX)"))
                k[strlen(k)-9] = 0;
            
            pa_strlcpy(name, k, l);
            r = 0;
            break;
        }
    }

    fclose(f);
    return r;
}
