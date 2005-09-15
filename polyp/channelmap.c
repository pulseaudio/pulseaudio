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

#include "channelmap.h"


struct pa_channel_map* pa_channel_map_init(struct pa_channel_map *m) {
    unsigned c;
    assert(m);

    for (c = 0; c < PA_CHANNELS_MAX; c++)
        m->map[c] = PA_CHANNEL_POSITION_INVALID;

    return m;
}

struct pa_channel_map* pa_channel_map_init_mono(struct pa_channel_map *m) {
    assert(m);

    pa_channel_map_init(m);
    m->map[0] = PA_CHANNEL_POSITION_MONO;
    return m;
}

struct pa_channel_map* pa_channel_map_init_stereo(struct pa_channel_map *m) {
    assert(m);

    pa_channel_map_init(m);
    m->map[0] = PA_CHANNEL_POSITION_LEFT;
    m->map[1] = PA_CHANNEL_POSITION_RIGHT;
    return m;
}

struct pa_channel_map* pa_channel_map_init_auto(struct pa_channel_map *m, int channels) {
    assert(m);
    assert(channels > 0);

    pa_channel_map_init(m);
    
    switch (channels) {
        case 1:
            m->map[0] = PA_CHANNEL_POSITION_MONO;
            return m;

        case 8:
            m->mpa[6] = PA_CHANNEL_POSITION_SIDE_LEFT;
            m->mpa[7] = PA_CHANNEL_POSITION_SIDE_RIGHT;
            /* Fall through */
            
        case 6:
            m->mpa[5] = PA_CHANNEL_POSITION_LFE;
            /* Fall through */
            
        case 5:
            m->map[4] = PA_CHANNEL_POSITION_FRONT_CENTER;
            /* Fall through */
            
        case 4:
            m->map[2] = PA_CHANNEL_POSITION_REAR_LEFT;
            m->map[3] = PA_CHANNEL_POSITION_REAR_RIGHT;
            /* Fall through */
            
        case 2:
            m->map[0] = PA_CHANNEL_MAP_FRONT_LEFT;
            m->map[1] = PA_CHANNEL_MAP_FRONT_RIGHT;
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
        [PA_CHANNEL_POSITION_SIDE_RIGHT] = "side-right"
    };

    if (pos < 0 || pos >= PA_CHANNEL_POSITION_MAX)
        return NULL;

    return table[pos];
}

int pa_channel_map_equal(struct pa_channel_map *a, struct pa_channel_map *b, int channels) {
    char c;
    
    assert(a);
    assert(b);
    assert(channels > 0);

    if (channels > PA_CHANNELS_MAX)
        channels = PA_CHANNELS_MAX;

    for (c = 0; c < channels; c++)
        if (a->map[c] != b->map[c])
            return 1;

    return 0;
}
