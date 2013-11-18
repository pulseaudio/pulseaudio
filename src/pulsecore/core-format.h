#ifndef foocoreformathfoo
#define foocoreformathfoo

/***
  This file is part of PulseAudio.

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

#include <pulse/format.h>

/* Gets the sample format stored in the format info. Returns a negative error
 * code on failure. If the sample format property is not set at all, returns
 * -PA_ERR_NOENTITY. */
int pa_format_info_get_sample_format(pa_format_info *f, pa_sample_format_t *sf);

/* Gets the sample rate stored in the format info. Returns a negative error
 * code on failure. If the sample rate property is not set at all, returns
 * -PA_ERR_NOENTITY. */
int pa_format_info_get_rate(pa_format_info *f, uint32_t *rate);

/* Gets the channel count stored in the format info. Returns a negative error
 * code on failure. If the channels property is not set at all, returns
 * -PA_ERR_NOENTITY. */
int pa_format_info_get_channels(pa_format_info *f, uint8_t *channels);

/* Gets the channel map stored in the format info. Returns a negative error
 * code on failure. If the channel map property is not set at all, returns
 * -PA_ERR_NOENTITY. */
int pa_format_info_get_channel_map(pa_format_info *f, pa_channel_map *map);

/* Convert the format info into a sample spec and a channel map. If the format
 * info doesn't contain some information, the fallback sample spec and channel
 * map are used to populate the output.
 *
 * pa_format_info_to_sample_spec() exists too. This "version 2" was created,
 * because the original function doesn't provide the possibility of specifying
 * a fallback sample spec and channel map. That functionality can't be added to
 * the original function, because the function is part of the public API and
 * adding parameters to it would break the API. */
int pa_format_info_to_sample_spec2(pa_format_info *f, pa_sample_spec *ss, pa_channel_map *map, pa_sample_spec *fallback_ss,
                                   pa_channel_map *fallback_map);

/* For compressed formats. Converts the format info into a sample spec and a
 * channel map that an ALSA device can use as its configuration parameters when
 * playing back the compressed data. That is, the returned sample spec doesn't
 * describe the audio content, but the device parameters. */
int pa_format_info_to_sample_spec_fake(pa_format_info *f, pa_sample_spec *ss, pa_channel_map *map);

#endif
