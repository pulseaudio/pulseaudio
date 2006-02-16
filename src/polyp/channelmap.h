#ifndef foochannelmaphfoo
#define foochannelmaphfoo

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

#include <polyp/sample.h>
#include <polyp/cdecl.h>

/** \file
 * Constants and routines for channel mapping handling */

PA_C_DECL_BEGIN

typedef enum {
    PA_CHANNEL_POSITION_INVALID = -1,
    PA_CHANNEL_POSITION_MONO = 0,

    PA_CHANNEL_POSITION_LEFT,
    PA_CHANNEL_POSITION_RIGHT,
    PA_CHANNEL_POSITION_CENTER,
    
    PA_CHANNEL_POSITION_FRONT_LEFT = PA_CHANNEL_POSITION_LEFT,
    PA_CHANNEL_POSITION_FRONT_RIGHT = PA_CHANNEL_POSITION_RIGHT,
    PA_CHANNEL_POSITION_FRONT_CENTER = PA_CHANNEL_POSITION_CENTER,

    PA_CHANNEL_POSITION_REAR_CENTER,
    PA_CHANNEL_POSITION_REAR_LEFT,
    PA_CHANNEL_POSITION_REAR_RIGHT,
    
    PA_CHANNEL_POSITION_LFE,
    PA_CHANNEL_POSITION_SUBWOOFER = PA_CHANNEL_POSITION_LFE,
    
    PA_CHANNEL_POSITION_FRONT_LEFT_OF_CENTER,
    PA_CHANNEL_POSITION_FRONT_RIGHT_OF_CENTER,
    
    PA_CHANNEL_POSITION_SIDE_LEFT,
    PA_CHANNEL_POSITION_SIDE_RIGHT,

    PA_CHANNEL_POSITION_AUX0,
    PA_CHANNEL_POSITION_AUX1,
    PA_CHANNEL_POSITION_AUX2,
    PA_CHANNEL_POSITION_AUX3,
    PA_CHANNEL_POSITION_AUX4,
    PA_CHANNEL_POSITION_AUX5,
    PA_CHANNEL_POSITION_AUX6,
    PA_CHANNEL_POSITION_AUX7,
    PA_CHANNEL_POSITION_AUX8,
    PA_CHANNEL_POSITION_AUX9,
    PA_CHANNEL_POSITION_AUX10,
    PA_CHANNEL_POSITION_AUX11,
    PA_CHANNEL_POSITION_AUX12,
    PA_CHANNEL_POSITION_AUX13,
    PA_CHANNEL_POSITION_AUX14,
    PA_CHANNEL_POSITION_AUX15,

    PA_CHANNEL_POSITION_MAX
} pa_channel_position_t;

typedef struct pa_channel_map {
    uint8_t channels;
    pa_channel_position_t map[PA_CHANNELS_MAX];
} pa_channel_map;

pa_channel_map* pa_channel_map_init(pa_channel_map *m);
pa_channel_map* pa_channel_map_init_mono(pa_channel_map *m);
pa_channel_map* pa_channel_map_init_stereo(pa_channel_map *m);
pa_channel_map* pa_channel_map_init_auto(pa_channel_map *m, unsigned channels);

const char* pa_channel_position_to_string(pa_channel_position_t pos);

#define PA_CHANNEL_MAP_SNPRINT_MAX 64
char* pa_channel_map_snprint(char *s, size_t l, const pa_channel_map *map);

int pa_channel_map_equal(const pa_channel_map *a, const pa_channel_map *b);

int pa_channel_map_valid(const pa_channel_map *map);

PA_C_DECL_END

#endif
