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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <pulsecore/core.h>
#include <pulsecore/core-util.h>
#if defined(HAVE_GSTAPTX) || defined(HAVE_GSTLDAC)
#include <gst/gst.h>
#endif

#include "a2dp-codec-util.h"

extern const pa_bt_codec pa_bt_codec_msbc;
extern const pa_bt_codec pa_bt_codec_cvsd;

/* List of HSP/HFP codecs.
 */
static const pa_bt_codec *pa_hf_codecs[] = {
    &pa_bt_codec_cvsd,
    &pa_bt_codec_msbc,
};

extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_sbc;
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_sbc_xq_453;
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_sbc_xq_512;
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_sbc_xq_552;
#ifdef HAVE_GSTAPTX
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_aptx;
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_aptx_hd;
#endif
#ifdef HAVE_GSTLDAC
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_ldac_eqmid_hq;
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_ldac_eqmid_sq;
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_ldac_eqmid_mq;
#endif
extern const pa_a2dp_endpoint_conf pa_a2dp_endpoint_conf_faststream;

/* This is list of supported codecs. Their order is important.
 * Codec with lower index has higher priority. */
static const pa_a2dp_endpoint_conf *pa_a2dp_endpoint_configurations[] = {
#ifdef HAVE_GSTLDAC
    &pa_a2dp_endpoint_conf_ldac_eqmid_hq,
    &pa_a2dp_endpoint_conf_ldac_eqmid_sq,
    &pa_a2dp_endpoint_conf_ldac_eqmid_mq,
#endif
#ifdef HAVE_GSTAPTX
    &pa_a2dp_endpoint_conf_aptx_hd,
    &pa_a2dp_endpoint_conf_aptx,
#endif
    &pa_a2dp_endpoint_conf_sbc,
    &pa_a2dp_endpoint_conf_sbc_xq_453,
    &pa_a2dp_endpoint_conf_sbc_xq_512,
    &pa_a2dp_endpoint_conf_sbc_xq_552,
    &pa_a2dp_endpoint_conf_faststream,
};

unsigned int pa_bluetooth_a2dp_endpoint_conf_count(void) {
    return PA_ELEMENTSOF(pa_a2dp_endpoint_configurations);
}

const pa_a2dp_endpoint_conf *pa_bluetooth_a2dp_endpoint_conf_iter(unsigned int i) {
    pa_assert(i < pa_bluetooth_a2dp_endpoint_conf_count());
    return pa_a2dp_endpoint_configurations[i];
}

unsigned int pa_bluetooth_hf_codec_count(void) {
    return PA_ELEMENTSOF(pa_hf_codecs);
}

const pa_bt_codec *pa_bluetooth_hf_codec_iter(unsigned int i) {
    pa_assert(i < pa_bluetooth_hf_codec_count());
    return pa_hf_codecs[i];
}

const pa_bt_codec *pa_bluetooth_get_hf_codec(const char *name) {
    unsigned int i;

    for (i = 0; i < PA_ELEMENTSOF(pa_hf_codecs); ++i) {
        if (pa_streq(pa_hf_codecs[i]->name, name))
            return pa_hf_codecs[i];
    }

    return NULL;
}

const pa_a2dp_endpoint_conf *pa_bluetooth_get_a2dp_endpoint_conf(const char *name) {
    unsigned int i;
    unsigned int count = pa_bluetooth_a2dp_endpoint_conf_count();

    for (i = 0; i < count; i++) {
        if (pa_streq(pa_a2dp_endpoint_configurations[i]->bt_codec.name, name))
            return pa_a2dp_endpoint_configurations[i];
    }

    return NULL;
}

void pa_bluetooth_a2dp_codec_gst_init(void) {
#if defined(HAVE_GSTAPTX) || defined(HAVE_GSTLDAC)
    GError *error = NULL;

    if (!gst_init_check(NULL, NULL, &error)) {
        pa_log_error("Could not initialise GStreamer: %s", error->message);
        g_error_free(error);
        return;
    }
    pa_log_info("GStreamer initialisation done");
#endif
}

bool pa_bluetooth_a2dp_codec_is_available(const pa_a2dp_codec_id *id, bool is_a2dp_sink) {
    unsigned int i;
    unsigned int count = pa_bluetooth_a2dp_endpoint_conf_count();
    const pa_a2dp_endpoint_conf *conf;

    for (i = 0; i < count; i++) {
        conf = pa_bluetooth_a2dp_endpoint_conf_iter(i);
        if (memcmp(id, &conf->id, sizeof(pa_a2dp_codec_id)) == 0
                && conf->can_be_supported(is_a2dp_sink))
            return true;
    }

    return false;
}
