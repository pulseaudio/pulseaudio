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

#include <stdio.h>
#include <assert.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdlib.h>
#include <sys/types.h>
#include <sys/stat.h>

#include <polypcore/module.h>
#include <polypcore/modargs.h>
#include <polypcore/xmalloc.h>
#include <polypcore/log.h>

#include "module-detect-symdef.h"

PA_MODULE_AUTHOR("Lennart Poettering")
PA_MODULE_DESCRIPTION("Detect available audio hardware and load matching drivers")
PA_MODULE_VERSION(PACKAGE_VERSION)
PA_MODULE_USAGE("just-one=<boolean>")

static const char *endswith(const char *haystack, const char *needle) {
    size_t l, m;
    const char *p;
    
    if ((l = strlen(haystack)) < (m = strlen(needle)))
        return NULL;

    if (strcmp(p = haystack + l - m, needle))
        return NULL;

    return p;
}

#ifdef HAVE_ALSA
static int detect_alsa(pa_core *c, int just_one) {
    FILE *f;
    int n = 0, n_sink = 0, n_source = 0;

    if (!(f = fopen("/proc/asound/devices", "r"))) {

        if (errno != ENOENT)
            pa_log_error(__FILE__": open(\"/proc/asound/devices\") failed: %s", strerror(errno));
        
        return -1;
    }

    while (!feof(f)) {
        char line[64], args[64];
        unsigned device, subdevice;
        int is_sink;
    
        if (!fgets(line, sizeof(line), f))
            break;

        line[strcspn(line, "\r\n")] = 0;

        if (endswith(line, "digital audio playback"))
            is_sink = 1;
        else if (endswith(line, "digital audio capture"))
            is_sink = 0;
        else
            continue;

        if (just_one && is_sink && n_sink >= 1)
            continue;
        
        if (just_one && !is_sink && n_source >= 1)
            continue;

        if (sscanf(line, " %*i: [%u- %u]: ", &device, &subdevice) != 2)
            continue;

        /* Only one sink per device */
        if (subdevice != 0)
            continue;

        snprintf(args, sizeof(args), "device=hw:%u,0", device);
        if (!pa_module_load(c, is_sink ? "module-alsa-sink" : "module-alsa-source", args))
            continue;

        n++;

        if (is_sink)
            n_sink++;
        else
            n_source++;
    }

    fclose(f);
    
    return n;
}
#endif

#ifdef HAVE_OSS
static int detect_oss(pa_core *c, int just_one) {
    FILE *f;
    int n = 0, b = 0;
    
    if (!(f = fopen("/dev/sndstat", "r")) &&
        !(f = fopen("/proc/sndstat", "r")) &&
        !(f = fopen("/proc/asound/oss/sndstat", "r"))) {

        if (errno != ENOENT)
            pa_log_error(__FILE__": failed to open OSS sndstat device: %s", strerror(errno));

        return -1;
    }

    while (!feof(f)) {
        char line[64], args[64];
        unsigned device;
    
        if (!fgets(line, sizeof(line), f))
            break;

        line[strcspn(line, "\r\n")] = 0;

        if (!b) {
            b = strcmp(line, "Audio devices:") == 0;
            continue;
        }

        if (line[0] == 0)
            break;
        
        if (sscanf(line, "%u: ", &device) != 1)
            continue;

        if (device == 0)
            snprintf(args, sizeof(args), "device=/dev/dsp");
        else
            snprintf(args, sizeof(args), "device=/dev/dsp%u", device);
        
        if (!pa_module_load(c, "module-oss", args))
            continue;

        n++;

        if (just_one)
            break;
    }

    fclose(f);
    return n;
}
#endif

#ifdef HAVE_SOLARIS
static int detect_solaris(pa_core *c, int just_one) {
    struct stat s;
    const char *dev;
    char args[64];

    dev = getenv("AUDIODEV");
    if (!dev)
        dev = "/dev/audio";

    if (stat(dev, &s) < 0) {
        if (errno != ENOENT)
            pa_log_error(__FILE__": failed to open device %s: %s", dev, strerror(errno));
        return -1;
    }

    if (!S_ISCHR(s.st_mode))
        return 0;

    snprintf(args, sizeof(args), "device=%s", dev);

    if (!pa_module_load(c, "module-solaris", args))
        return 0;

    return 1;
}
#endif

#ifdef OS_IS_WIN32
static int detect_waveout(pa_core *c, int just_one) {
    /*
     * FIXME: No point in enumerating devices until the plugin supports
     * selecting anything but the first.
     */
    if (!pa_module_load(c, "module-waveout", ""))
        return 0;

    return 1;
}
#endif

int pa__init(pa_core *c, pa_module*m) {
    int just_one = 0, n = 0;
    pa_modargs *ma;

    static const char* const valid_modargs[] = {
        "just-one",
        NULL
    };
    
    assert(c);
    assert(m);

    if (!(ma = pa_modargs_new(m->argument, valid_modargs))) {
        pa_log(__FILE__": Failed to parse module arguments");
        goto fail;
    }
    
    if (pa_modargs_get_value_boolean(ma, "just-one", &just_one) < 0) {
        pa_log(__FILE__": just_one= expects a boolean argument.");
        goto fail;
    }

#if HAVE_ALSA
    if ((n = detect_alsa(c, just_one)) <= 0) 
#endif
#if HAVE_OSS
    if ((n = detect_oss(c, just_one)) <= 0)
#endif
#if HAVE_SOLARIS
    if ((n = detect_solaris(c, just_one)) <= 0)
#endif
#if OS_IS_WIN32
    if ((n = detect_waveout(c, just_one)) <= 0)
#endif
    {
        pa_log_warn(__FILE__": failed to detect any sound hardware.");
        goto fail;
    }

    pa_log_info(__FILE__": loaded %i modules.", n);
    
    /* We were successful and can unload ourselves now. */
    pa_module_unload_request(m);

    pa_modargs_free(ma);

    return 0;

fail:
    if (ma)
        pa_modargs_free(ma);
    
    return -1;
}


void pa__done(pa_core *c, pa_module*m) {
    /* NOP */
}

