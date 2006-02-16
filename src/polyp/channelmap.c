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

#include <stdlib.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "channelmap.h"

pa_channel_map* pa_channel_map_init(pa_channel_map *m) {
    unsigned c;
    assert(m);

    m->channels = 0;

    for (c = 0; c < PA_CHANNELS_MAX; c++)
        m->map[c] = PA_CHANNEL_POSITION_INVALID;

    return m;
}

pa_channel_map* pa_channel_map_init_mono(pa_channel_map *m) {
    assert(m);

    pa_channel_map_init(m);

    m->channels = 1;
    m->map[0] = PA_CHANNEL_POSITION_MONO;
    return m;
}

pa_channel_map* pa_channel_map_init_stereo(pa_channel_map *m) {
    assert(m);

    pa_channel_map_init(m);

    m->channels = 2;
    m->map[0] = PA_CHANNEL_POSITION_LEFT;
    m->map[1] = PA_CHANNEL_POSITION_RIGHT;
    return m;
}

pa_channel_map* pa_channel_map_init_auto(pa_channel_map *m, unsigned channels) {
    assert(m);
    assert(channels > 0);
    assert(channels <= PA_CHANNELS_MAX);

    pa_channel_map_init(m);

    m->channels = channels;

    /* This is somewhat compatible with RFC3551 */
    
    switch (channels) {
        case 1:
            m->map[0] = PA_CHANNEL_POSITION_MONO;
            return m;

        case 6:
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[3] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            m->map[4] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            m->map[5] = PA_CHANNEL_POSITION_LFE;
            return m;
            
        case 5:
            m->map[2] = PA_CHANNEL_POSITION_FRONT_CENTER;
            m->map[3] = PA_CHANNEL_POSITION_REAR_LEFT;
            m->map[4] = PA_CHANNEL_POSITION_REAR_RIGHT;
            /* Fall through */
            
        case 2:
            m->map[0] = PA_CHANNEL_POSITION_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_FRONT_RIGHT;
            return m;

        case 3:
            m->map[0] = PA_CHANNEL_POSITION_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_RIGHT;
            m->map[2] = PA_CHANNEL_POSITION_CENTER;
            return m;

        case 4:
            m->map[0] = PA_CHANNEL_POSITION_LEFT;
            m->map[1] = PA_CHANNEL_POSITION_CENTER;
            m->map[2] = PA_CHANNEL_POSITION_RIGHT;
            m->map[3] = PA_CHANNEL_POSITION_LFE;
            return m;
            
        default:
            return NULL;
    }
}

const char* pa_channel_position_to_string(pa_channel_position_t pos) {

    const char *const table[] = {
        [PA_CHANNEL_POSITION_MONO] = "mono",

        [PA_CHANNEL_POSITION_FRONT_CENTER] = "front-center",
        [PA_CHANNEL_POSITION_FRONT_LEFT] = "front-left",
        [PA_CHANNEL_POSITION_FRONT_RIGHT] = "front-right",
        
        [PA_CHANNEL_POSITION_REAR_CENTER] = "rear-center",
        [PA_CHANNEL_POSITION_REAR_LEFT] = "rear-left",
        [PA_CHANNEL_POSITION_REAR_RIGHT] = "rear-right",

        [PA_CHANNEL_POSITION_LFE] = "lfe",

        [PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER] = "front-left-of-center",
        [PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER] = "front-right-of-center",
        
        [PA_CHANNEL_POSITION_SIDE_LEFT] = "side-left",
        [PA_CHANNEL_POSITION_SIDE_RIGHT] = "side-right",

        [PA_CHANNEL_POSITION_AUX1] = "aux1",
        [PA_CHANNEL_POSITION_AUX2] = "aux2",
        [PA_CHANNEL_POSITION_AUX3] = "aux3",
        [PA_CHANNEL_POSITION_AUX4] = "aux4",
        [PA_CHANNEL_POSITION_AUX5] = "aux5",
        [PA_CHANNEL_POSITION_AUX6] = "aux6",
        [PA_CHANNEL_POSITION_AUX7] = "aux7",
        [PA_CHANNEL_POSITION_AUX8] = "aux8",
        [PA_CHANNEL_POSITION_AUX9] = "aux9",
        [PA_CHANNEL_POSITION_AUX10] = "aux10",
        [PA_CHANNEL_POSITION_AUX11] = "aux11",
        [PA_CHANNEL_POSITION_AUX12] = "aux12"
    };

    if (pos < 0 || pos >= PA_CHANNEL_POSITION_MAX)
        return NULL;

    return table[pos];
}

int pa_channel_map_equal(const pa_channel_map *a, const pa_channel_map *b) {
    unsigned c;
    
    assert(a);
    assert(b);

    if (a->channels != b->channels)
        return 0;
    
    for (c = 0; c < a->channels; c++)
        if (a->map[c] != b->map[c])
            return 0;

    return 1;
}

char* pa_channel_map_snprint(char *s, size_t l, const pa_channel_map *map) {
    unsigned channel;
    int first = 1;
    char *e;
    
    assert(s);
    assert(l > 0);
    assert(map);

    *(e = s) = 0;

    for (channel = 0; channel < map->channels && l > 1; channel++) {
        l -= snprintf(e, l, "%s%u:%s",
                      first ? "" : " ",
                      channel,
                      pa_channel_position_to_string(map->map[channel]));

        e = strchr(e, 0);
        first = 0;
    }

    return s;
}

int pa_channel_map_valid(const pa_channel_map *map) {
    unsigned c;
    
    assert(map);

    if (map->channels <= 0 || map->channels > PA_CHANNELS_MAX)
        return 0;

    for (c = 0; c < map->channels; c++)
        if (map->map[c] < 0 ||map->map[c] >= PA_CHANNEL_POSITION_MAX)
            return 0;

    return 1;
}
