#ifndef foosamplehfoo
#define foosamplehfoo

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

#include <inttypes.h>
#include <sys/types.h>
#include <math.h>

#include <polyp/cdecl.h>

/** \file
 * Constants and routines for sample type handling */

PA_C_DECL_BEGIN

/* Maximum allowed channels */
#define PA_CHANNELS_MAX 16

/** Sample format */
typedef enum {
    PA_SAMPLE_U8,              /**< Unsigned 8 Bit PCM */
    PA_SAMPLE_ALAW,            /**< 8 Bit a-Law */
    PA_SAMPLE_ULAW,            /**< 8 Bit mu-Law */
    PA_SAMPLE_S16LE,           /**< Signed 16 Bit PCM, little endian (PC) */
    PA_SAMPLE_S16BE,           /**< Signed 16 Bit PCM, big endian */
    PA_SAMPLE_FLOAT32LE,       /**< 32 Bit IEEE floating point, little endian, range -1..1 */
    PA_SAMPLE_FLOAT32BE,       /**< 32 Bit IEEE floating point, big endian, range -1..1 */
    PA_SAMPLE_MAX,             /**< Upper limit of valid sample types */
    PA_SAMPLE_INVALID = -1     /**< An invalid value */
} pa_sample_format_t;

#ifdef WORDS_BIGENDIAN
/** Signed 16 Bit PCM, native endian */
#define PA_SAMPLE_S16NE PA_SAMPLE_S16BE
/** 32 Bit IEEE floating point, native endian */
#define PA_SAMPLE_FLOAT32NE PA_SAMPLE_FLOAT32BE
#else
/** Signed 16 Bit PCM, native endian */
#define PA_SAMPLE_S16NE PA_SAMPLE_S16LE
/** 32 Bit IEEE floating point, native endian */
#define PA_SAMPLE_FLOAT32NE PA_SAMPLE_FLOAT32LE
#endif

/** A Shortcut for PA_SAMPLE_FLOAT32NE */
#define PA_SAMPLE_FLOAT32 PA_SAMPLE_FLOAT32NE

/** A sample format and attribute specification */
struct pa_sample_spec {
    pa_sample_format_t format;     /**< The sample format */
    uint32_t rate;                 /**< The sample rate. (e.g. 44100) */
    uint8_t channels;              /**< Audio channels. (1 for mono, 2 for stereo, ...) */
};

/** Type for usec specifications (unsigned). May be either 32 or 64 bit, depending on the architecture */
typedef uint64_t pa_usec_t;

/** Return the amount of bytes playback of a second of audio with the specified sample type takes */
size_t pa_bytes_per_second(const struct pa_sample_spec *spec);

/** Return the size of a frame with the specific sample type */
size_t pa_frame_size(const struct pa_sample_spec *spec);

/** Calculate the time the specified bytes take to play with the specified sample type */
pa_usec_t pa_bytes_to_usec(uint64_t length, const struct pa_sample_spec *spec);

/** Return non-zero when the sample type specification is valid */
int pa_sample_spec_valid(const struct pa_sample_spec *spec);

/** Return non-zero when the two sample type specifications match */
int pa_sample_spec_equal(const struct pa_sample_spec*a, const struct pa_sample_spec*b);

/* Return a descriptive string for the specified sample format. \since 0.8 */
const char *pa_sample_format_to_string(pa_sample_format_t f);

/** Parse a sample format text. Inverse of pa_sample_format_to_string() */
pa_sample_format_t pa_parse_sample_format(const char *format);

/** Maximum required string length for pa_sample_spec_snprint() */
#define PA_SAMPLE_SPEC_SNPRINT_MAX 32

/** Pretty print a sample type specification to a string */
char* pa_sample_spec_snprint(char *s, size_t l, const struct pa_sample_spec *spec);

/** Pretty print a byte size value. (i.e. "2.5 MB") */
void pa_bytes_snprint(char *s, size_t l, unsigned v);

PA_C_DECL_END

#endif
