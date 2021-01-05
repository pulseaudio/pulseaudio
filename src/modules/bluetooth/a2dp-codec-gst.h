/***
  This file is part of PulseAudio.

  Copyright (C) 2020 Asymptotic <sanchayan@asymptotic.io>

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

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/base/gstadapter.h>
#include <pulsecore/fdsem.h>

enum a2dp_codec_type {
    AAC = 0,
    APTX,
    APTX_HD,
    LDAC_EQMID_HQ,
    LDAC_EQMID_SQ,
    LDAC_EQMID_MQ
};

struct gst_info {
    pa_core *core;
    pa_sample_spec *ss;
    enum a2dp_codec_type codec_type;
    union {
        const a2dp_aac_t *aac_config;
        const a2dp_aptx_t *aptx_config;
        const a2dp_aptx_hd_t *aptx_hd_config;
        const a2dp_ldac_t *ldac_config;
    } a2dp_codec_t;

    GstElement *enc_bin, *dec_bin;
    GstElement *enc_src, *enc_sink;
    GstElement *dec_src, *dec_sink;
    GstElement *enc_pipeline, *dec_pipeline;
    GstAdapter *enc_adapter, *dec_adapter;

    pa_fdsem *enc_fdsem;
    pa_fdsem *dec_fdsem;

    uint16_t seq_num;
};

bool gst_codec_init(struct gst_info *info, bool for_encoding);
size_t gst_encode_buffer(void *codec_info, uint32_t timestamp, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed);
size_t gst_decode_buffer(void *codec_info, const uint8_t *input_buffer, size_t input_size, uint8_t *output_buffer, size_t output_size, size_t *processed);
void gst_codec_deinit(void *codec_info);

bool gst_init_enc_common(struct gst_info *info);
bool gst_init_dec_common(struct gst_info *info);
void gst_deinit_enc_common(struct gst_info *info);
void gst_deinit_dec_common(struct gst_info *info);
