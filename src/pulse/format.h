#ifndef fooformathfoo
#define fooformathfoo

/***
  This file is part of PulseAudio.

  Copyright 2011 Intel Corporation
  Copyright 2011 Collabora Multimedia
  Copyright 2011 Arun Raghavan <arun.raghavan@collabora.co.uk>

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

#include <pulse/cdecl.h>
#include <pulse/proplist.h>

PA_C_DECL_BEGIN

/**< Represents the type of encoding used in a stream or accepted by a sink. \since 1.0 */
typedef enum pa_encoding {
    PA_ENCODING_ANY,
    /**< Any encoding format, PCM or compressed */

    PA_ENCODING_PCM,
    /**< Any PCM format */

    PA_ENCODING_AC3_IEC61937,
    /**< AC3 data encapsulated in IEC 61937 header/padding */

    PA_ENCODING_EAC3_IEC61937,
    /**< EAC3 data encapsulated in IEC 61937 header/padding */

    PA_ENCODING_MPEG_IEC61937,
    /**< MPEG-1 or MPEG-2 (Part 3, not AAC) data encapsulated in IEC 61937 header/padding */

    PA_ENCODING_MAX,
    /**< Valid encoding types must be less than this value */

    PA_ENCODING_INVALID = -1,
    /**< Represents an invalid encoding */
} pa_encoding_t;

/** Returns a printable string representing the given encoding type. \since 1.0 */
const char *pa_encoding_to_string(pa_encoding_t e) PA_GCC_CONST;

/**< Represents the format of data provided in a stream or processed by a sink. \since 1.0 */
typedef struct pa_format_info {
    pa_encoding_t encoding;
    /**< The encoding used for the format */

    pa_proplist *plist;
    /**< Additional encoding-specific properties such as sample rate, bitrate, etc. */
} pa_format_info;

/**< Allocates a new \a pa_format_info structure. Clients must initialise at least the encoding field themselves. */
pa_format_info* pa_format_info_new(void);

/**< Returns a new \a pa_format_info struct and representing the same format as \a src */
pa_format_info* pa_format_info_copy(const pa_format_info *src);

/**< Frees a \a pa_format_info structure */
void pa_format_info_free(pa_format_info *f);

/** Returns non-zero when the format info structure is valid */
int pa_format_info_valid(const pa_format_info *f);

/** Returns non-zero when the format info structure represents a PCM (i.e. uncompressed data) format */
int pa_format_info_is_pcm(const pa_format_info *f);

/** Maximum required string length for
 * pa_format_info_snprint(). Please note that this value can change
 * with any release without warning and without being considered API
 * or ABI breakage. You should not use this definition anywhere where
 * it might become part of an ABI. \since 1.0 */
#define PA_FORMAT_INFO_SNPRINT_MAX 256

/** Return a human-readable string representing the given format. \since 1.0 */
char *pa_format_info_snprint(char *s, size_t l, const pa_format_info *f);

PA_C_DECL_END

#endif
