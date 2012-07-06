/*
 *
 *  BlueZ - Bluetooth protocol stack for Linux
 *
 *  Copyright (C) 2004-2010  Marcel Holtmann <marcel@holtmann.org>
 *
 *  This library is free software; you can redistribute it and/or
 *  modify it under the terms of the GNU Lesser General Public
 *  License as published by the Free Software Foundation; either
 *  version 2.1 of the License, or (at your option) any later version.
 *
 *  This library is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 *  Lesser General Public License for more details.
 *
 *  You should have received a copy of the GNU Lesser General Public
 *  License along with this library; if not, write to the Free Software
 *  Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA  02110-1301  USA
 *
 */

#ifndef BT_AUDIOCLIENT_H
#define BT_AUDIOCLIENT_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <errno.h>

/**
 * SBC Codec parameters as per A2DP profile 1.0 § 4.3
 */

/* A2DP seid are 6 bytes long so HSP/HFP are assigned to 7-8 bits */
#define BT_A2DP_SEID_RANGE			(1 << 6) - 1

#define BT_A2DP_SBC_SOURCE			0x00
#define BT_A2DP_SBC_SINK			0x01
#define BT_A2DP_MPEG12_SOURCE			0x02
#define BT_A2DP_MPEG12_SINK			0x03
#define BT_A2DP_MPEG24_SOURCE			0x04
#define BT_A2DP_MPEG24_SINK			0x05
#define BT_A2DP_ATRAC_SOURCE			0x06
#define BT_A2DP_ATRAC_SINK			0x07
#define BT_A2DP_UNKNOWN_SOURCE			0x08
#define BT_A2DP_UNKNOWN_SINK			0x09

#define BT_SBC_SAMPLING_FREQ_16000		(1 << 3)
#define BT_SBC_SAMPLING_FREQ_32000		(1 << 2)
#define BT_SBC_SAMPLING_FREQ_44100		(1 << 1)
#define BT_SBC_SAMPLING_FREQ_48000		1

#define BT_A2DP_CHANNEL_MODE_MONO		(1 << 3)
#define BT_A2DP_CHANNEL_MODE_DUAL_CHANNEL	(1 << 2)
#define BT_A2DP_CHANNEL_MODE_STEREO		(1 << 1)
#define BT_A2DP_CHANNEL_MODE_JOINT_STEREO	1

#define BT_A2DP_BLOCK_LENGTH_4			(1 << 3)
#define BT_A2DP_BLOCK_LENGTH_8			(1 << 2)
#define BT_A2DP_BLOCK_LENGTH_12			(1 << 1)
#define BT_A2DP_BLOCK_LENGTH_16			1

#define BT_A2DP_SUBBANDS_4			(1 << 1)
#define BT_A2DP_SUBBANDS_8			1

#define BT_A2DP_ALLOCATION_SNR			(1 << 1)
#define BT_A2DP_ALLOCATION_LOUDNESS		1

typedef struct {
	uint8_t seid;
	uint8_t transport;
	uint8_t type;
	uint8_t length;
	uint8_t configured;
	uint8_t lock;
	uint8_t data[0];
} __attribute__ ((packed)) codec_capabilities_t;

typedef struct {
	codec_capabilities_t capability;
	uint8_t channel_mode;
	uint8_t frequency;
	uint8_t allocation_method;
	uint8_t subbands;
	uint8_t block_length;
	uint8_t min_bitpool;
	uint8_t max_bitpool;
} __attribute__ ((packed)) sbc_capabilities_t;

typedef struct {
	codec_capabilities_t capability;
	uint8_t channel_mode;
	uint8_t crc;
	uint8_t layer;
	uint8_t frequency;
	uint8_t mpf;
	uint16_t bitrate;
} __attribute__ ((packed)) mpeg_capabilities_t;

typedef struct {
	codec_capabilities_t capability;
	uint8_t flags;
	uint16_t sampling_rate;
} __attribute__ ((packed)) pcm_capabilities_t;

#ifdef __cplusplus
}
#endif

#endif /* BT_AUDIOCLIENT_H */
