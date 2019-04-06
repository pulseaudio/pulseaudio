#ifndef fooa2dpcodecutilhfoo
#define fooa2dpcodecutilhfoo

/***
  This file is part of PulseAudio.

  Copyright 2019 Pali Rohár <pali.rohar@gmail.com>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public
  License along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#include "a2dp-codec-api.h"

/* Get number of supported A2DP codecs */
unsigned int pa_bluetooth_a2dp_codec_count(void);

/* Get i-th codec. Codec with higher number has higher priority */
const pa_a2dp_codec *pa_bluetooth_a2dp_codec_iter(unsigned int i);

/* Get codec by name */
const pa_a2dp_codec *pa_bluetooth_get_a2dp_codec(const char *name);

#endif
