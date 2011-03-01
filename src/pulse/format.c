/***
  This file is part of PulseAudio.

  Copyright 2011 Intel Corporation
  Copyright 2011 Collabora Multimedia
  Copyright 2011 Arun Raghavan <arun.raghavan@collabora.co.uk>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulse/internal.h>
#include <pulse/xmalloc.h>

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "format.h"

pa_format_info* pa_format_info_new(void) {
    pa_format_info *f = pa_xnew(pa_format_info, 1);

    f->encoding = PA_ENCODING_INVALID;
    f->plist = pa_proplist_new();

    return f;
}

pa_format_info* pa_format_info_copy(const pa_format_info *src) {
    pa_format_info *dest;

    pa_assert(src);

    dest = pa_xnew(pa_format_info, 1);

    dest->encoding = src->encoding;

    if (src->plist)
        dest->plist = pa_proplist_copy(src->plist);
    else
        dest->plist = NULL;

    return dest;
}

void pa_format_info_free(pa_format_info *f) {
    pa_assert(f);

    pa_proplist_free(f->plist);
    pa_xfree(f);
}

int pa_format_info_valid(pa_format_info *f) {
    return (f->encoding >= 0 && f->encoding < PA_ENCODING_MAX && f->plist != NULL);
}

int pa_format_info_is_pcm(pa_format_info *f) {
    return f->encoding == PA_ENCODING_PCM;
}

pa_bool_t pa_format_info_is_compatible(pa_format_info *first, pa_format_info *second) {
    const char *key;
    void *state = NULL;

    pa_assert(first);
    pa_assert(second);

    if (first->encoding != second->encoding)
        return FALSE;

    while ((key = pa_proplist_iterate(first->plist, &state))) {
        const char *value_one, *value_two;

        value_one = pa_proplist_gets(first->plist, key);
        value_two = pa_proplist_gets(second->plist, key);

        if (!value_two || !pa_streq(value_one, value_two))
            return FALSE;
    }

    return TRUE;
}

pa_format_info* pa_format_info_from_sample_spec(pa_sample_spec *ss, pa_channel_map *map) {
    char cm[PA_CHANNEL_MAP_SNPRINT_MAX];
    pa_format_info *f;

    pa_assert(ss && pa_sample_spec_valid(ss));
    pa_assert(!map || pa_channel_map_valid(map));

    f = pa_format_info_new();
    f->encoding = PA_ENCODING_PCM;

    pa_proplist_sets(f->plist, PA_PROP_FORMAT_SAMPLE_FORMAT, pa_sample_format_to_string(ss->format));
    pa_proplist_setf(f->plist, PA_PROP_FORMAT_RATE, "%u", (unsigned int) ss->rate);
    pa_proplist_setf(f->plist, PA_PROP_FORMAT_CHANNELS, "%u", (unsigned int) ss->channels);

    if (map) {
        pa_channel_map_snprint(cm, sizeof(cm), map);
        pa_proplist_setf(f->plist, PA_PROP_FORMAT_CHANNEL_MAP, "%s", cm);
    }

    return f;
}

/* For PCM streams */
void pa_format_info_to_sample_spec(pa_format_info *f, pa_sample_spec *ss, pa_channel_map *map) {
    const char *sf, *r, *ch;
    uint32_t channels;

    pa_assert(f);
    pa_assert(ss);
    pa_assert(f->encoding == PA_ENCODING_PCM);

    pa_assert(sf = pa_proplist_gets(f->plist, PA_PROP_FORMAT_SAMPLE_FORMAT));
    pa_assert(r = pa_proplist_gets(f->plist, PA_PROP_FORMAT_RATE));
    pa_assert(ch = pa_proplist_gets(f->plist, PA_PROP_FORMAT_CHANNELS));

    pa_assert((ss->format = pa_parse_sample_format(sf)) != PA_SAMPLE_INVALID);
    pa_assert(pa_atou(r, &ss->rate) == 0);
    pa_assert(pa_atou(ch, &channels) == 0);
    ss->channels = (uint8_t) channels;

    if (map) {
        const char *m = pa_proplist_gets(f->plist, PA_PROP_FORMAT_CHANNEL_MAP);
        pa_channel_map_init(map);

        if (m)
            pa_assert(pa_channel_map_parse(map, m) != NULL);
    }
}

/* For compressed streams */
void pa_format_info_to_sample_spec_fake(pa_format_info *f, pa_sample_spec *ss) {
    const char *r;

    pa_assert(f);
    pa_assert(ss);
    pa_assert(f->encoding != PA_ENCODING_PCM);

    ss->format = PA_SAMPLE_S16LE;
    ss->channels = 2;

    pa_assert(r = pa_proplist_gets(f->plist, PA_PROP_FORMAT_RATE));
    pa_assert(pa_atou(r, &ss->rate) == 0);
}
