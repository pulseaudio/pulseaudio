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
#include <pulse/i18n.h>

#include <pulsecore/core-util.h>
#include <pulsecore/macro.h>

#include "format.h"

const char *pa_encoding_to_string(pa_encoding_t e) {
    static const char* const table[]= {
        [PA_ENCODING_PCM] = "pcm",
        [PA_ENCODING_AC3_IEC61937] = "ac3-iec61937",
        [PA_ENCODING_EAC3_IEC61937] = "eac3-iec61937",
        [PA_ENCODING_MPEG_IEC61937] = "mpeg-iec61937",
        [PA_ENCODING_ANY] = "any",
    };

    if (e < 0 || e >= PA_ENCODING_MAX)
        return NULL;

    return table[e];
}

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

void pa_format_info_free2(pa_format_info *f, void *userdata) {
    pa_format_info_free(f);
}

int pa_format_info_valid(const pa_format_info *f) {
    return (f->encoding >= 0 && f->encoding < PA_ENCODING_MAX && f->plist != NULL);
}

int pa_format_info_is_pcm(const pa_format_info *f) {
    return f->encoding == PA_ENCODING_PCM;
}

char *pa_format_info_snprint(char *s, size_t l, const pa_format_info *f) {
    char *tmp;

    pa_assert(s);
    pa_assert(l > 0);
    pa_assert(f);

    pa_init_i18n();

    if (!pa_format_info_valid(f))
        pa_snprintf(s, l, _("(invalid)"));
    else {
        tmp = pa_proplist_to_string_sep(f->plist, ", ");
        pa_snprintf(s, l, _("%s, %s"), pa_encoding_to_string(f->encoding), tmp[0] ? tmp : _("(no properties)"));
        pa_xfree(tmp);
    }

    return s;
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
pa_bool_t pa_format_info_to_sample_spec(pa_format_info *f, pa_sample_spec *ss, pa_channel_map *map) {
    const char *sf, *r, *ch;
    uint32_t channels;

    pa_assert(f);
    pa_assert(ss);
    pa_return_val_if_fail(f->encoding == PA_ENCODING_PCM, FALSE);

    pa_return_val_if_fail(sf = pa_proplist_gets(f->plist, PA_PROP_FORMAT_SAMPLE_FORMAT), FALSE);
    pa_return_val_if_fail(r = pa_proplist_gets(f->plist, PA_PROP_FORMAT_RATE), FALSE);
    pa_return_val_if_fail(ch = pa_proplist_gets(f->plist, PA_PROP_FORMAT_CHANNELS), FALSE);

    pa_return_val_if_fail((ss->format = pa_parse_sample_format(sf)) != PA_SAMPLE_INVALID, FALSE);
    pa_return_val_if_fail(pa_atou(r, &ss->rate) == 0, FALSE);
    pa_return_val_if_fail(pa_atou(ch, &channels) == 0, FALSE);
    ss->channels = (uint8_t) channels;

    if (map) {
        const char *m = pa_proplist_gets(f->plist, PA_PROP_FORMAT_CHANNEL_MAP);
        pa_channel_map_init(map);

        if (m)
            pa_return_val_if_fail(pa_channel_map_parse(map, m) != NULL, FALSE);
    }

    return TRUE;
}

/* For compressed streams */
pa_bool_t pa_format_info_to_sample_spec_fake(pa_format_info *f, pa_sample_spec *ss) {
    const char *r;

    pa_assert(f);
    pa_assert(ss);
    pa_return_val_if_fail(f->encoding != PA_ENCODING_PCM, FALSE);

    ss->format = PA_SAMPLE_S16LE;
    ss->channels = 2;

    pa_return_val_if_fail(r = pa_proplist_gets(f->plist, PA_PROP_FORMAT_RATE), FALSE);
    pa_return_val_if_fail(pa_atou(r, &ss->rate) == 0, FALSE);

    return TRUE;
}
