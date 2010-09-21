/***
    This file is part of PulseAudio.

    Copyright 2010 Arun Raghavan <arun.raghavan@collabora.co.uk>

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

#include <pulse/sample.h>
#include <pulse/channelmap.h>
#include <pulsecore/core.h>
#include <pulsecore/macro.h>

#include <speex/speex_echo.h>
#include "adrian.h"

/* Common data structures */

typedef struct pa_echo_canceller_params pa_echo_canceller_params;

struct pa_echo_canceller_params {
    union {
        struct {
            SpeexEchoState *state;
        } speex;
        struct {
            uint32_t blocksize;
            AEC *aec;
        } adrian;
        /* each canceller-specific structure goes here */
    } priv;
};

typedef struct pa_echo_canceller pa_echo_canceller;

struct pa_echo_canceller {
    pa_bool_t   (*init)                 (pa_core *c,
                                         pa_echo_canceller *ec,
                                         pa_sample_spec *source_ss,
                                         pa_channel_map *source_map,
                                         pa_sample_spec *sink_ss,
                                         pa_channel_map *sink_map,
                                         uint32_t *blocksize,
                                         const char *args);
    void        (*run)                  (pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out);
    void        (*done)                 (pa_echo_canceller *ec);

    pa_echo_canceller_params params;
};

/* Speex canceller functions */
pa_bool_t pa_speex_ec_init(pa_core *c, pa_echo_canceller *ec,
                           pa_sample_spec *source_ss, pa_channel_map *source_map,
                           pa_sample_spec *sink_ss, pa_channel_map *sink_map,
                           uint32_t *blocksize, const char *args);
void pa_speex_ec_run(pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out);
void pa_speex_ec_done(pa_echo_canceller *ec);

/* Adrian Andre's echo canceller */
pa_bool_t pa_adrian_ec_init(pa_core *c, pa_echo_canceller *ec,
                           pa_sample_spec *source_ss, pa_channel_map *source_map,
                           pa_sample_spec *sink_ss, pa_channel_map *sink_map,
                           uint32_t *blocksize, const char *args);
void pa_adrian_ec_run(pa_echo_canceller *ec, const uint8_t *rec, const uint8_t *play, uint8_t *out);
void pa_adrian_ec_done(pa_echo_canceller *ec);
