/***
  This file is part of PulseAudio.

  Copyright 2016 Arun Raghavan <mail@arunraghavan.net>

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2.1 of the License,
  or (at your option) any later version.

  PulseAudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  General Public License for more details.

  You should have received a copy of the GNU Lesser General Public License
  along with PulseAudio; if not, see <http://www.gnu.org/licenses/>.
***/

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <pulsecore/hashmap.h>

#define PA_DOUBLE_IS_EQUAL(x, y) (((x) - (y)) < 0.000001 && ((x) - (y)) > -0.000001)

typedef enum {
    PA_JSON_TYPE_INIT = 0,
    PA_JSON_TYPE_NULL,
    PA_JSON_TYPE_INT,
    PA_JSON_TYPE_DOUBLE,
    PA_JSON_TYPE_BOOL,
    PA_JSON_TYPE_STRING,
    PA_JSON_TYPE_ARRAY,
    PA_JSON_TYPE_OBJECT,
} pa_json_type;

typedef struct pa_json_object pa_json_object;

pa_json_object* pa_json_parse(const char *str);
pa_json_type pa_json_object_get_type(const pa_json_object *obj);
void pa_json_object_free(pa_json_object *obj);

/* All pointer members that are returned are valid while the corresponding object is valid */

int64_t pa_json_object_get_int(const pa_json_object *o);
double pa_json_object_get_double(const pa_json_object *o);
bool pa_json_object_get_bool(const pa_json_object *o);
const char* pa_json_object_get_string(const pa_json_object *o);

const pa_json_object* pa_json_object_get_object_member(const pa_json_object *o, const char *name);

/** Returns pa_hashmap (char* -> const pa_json_object*) to iterate over object members. \since 15.0 */
const pa_hashmap *pa_json_object_get_object_member_hashmap(const pa_json_object *o);

int pa_json_object_get_array_length(const pa_json_object *o);
const pa_json_object* pa_json_object_get_array_member(const pa_json_object *o, int index);

bool pa_json_object_equal(const pa_json_object *o1, const pa_json_object *o2);

/** @{ \name Write functions */

/** Structure which holds a JSON encoder. Wrapper for pa_strbuf and encoder context. \since 15.0 */
typedef struct pa_json_encoder pa_json_encoder;

/** Create a new pa_json_encoder structure. \since 15.0 */
pa_json_encoder *pa_json_encoder_new(void);
/** Free a pa_json_encoder structure. \since 15.0 */
void pa_json_encoder_free(pa_json_encoder *encoder);
/** Convert pa_json_encoder to string, free pa_json_encoder structure.
 * The returned string needs to be freed with pa_xree(). \since 15.0 */
char *pa_json_encoder_to_string_free(pa_json_encoder *encoder);
/** Check if a pa_json_encoder is empty (nothing has been added). \since 16.0 */
bool pa_json_encoder_is_empty(pa_json_encoder *encoder);

/** Start appending JSON object element by writing an opening brace. \since 15.0 */
void pa_json_encoder_begin_element_object(pa_json_encoder *encoder);
/** Start appending JSON object member to JSON object. \since 15.0 */
void pa_json_encoder_begin_member_object(pa_json_encoder *encoder, const char *name);
/** End appending JSON object element or member to JSON object. \since 15.0 */
void pa_json_encoder_end_object(pa_json_encoder *encoder);
/** Start appending JSON array element by writing an opening bracket. \since 15.0 */
void pa_json_encoder_begin_element_array(pa_json_encoder *encoder);
/** Start appending JSON array member to JSON object. \since 15.0 */
void pa_json_encoder_begin_member_array(pa_json_encoder *encoder, const char *name);
/** End appending JSON array element or member to JSON object. \since 15.0 */
void pa_json_encoder_end_array(pa_json_encoder *encoder);
/** Append null element to JSON. \since 15.0 */
void pa_json_encoder_add_element_null(pa_json_encoder *encoder);
/** Append null member to JSON object. \since 15.0 */
void pa_json_encoder_add_member_null(pa_json_encoder *encoder, const char *name);
/** Append boolean element to JSON. \since 15.0 */
void pa_json_encoder_add_element_bool(pa_json_encoder *encoder, bool value);
/** Append boolean member to JSON object. \since 15.0 */
void pa_json_encoder_add_member_bool(pa_json_encoder *encoder, const char *name, bool value);
/** Append string element to JSON. Value will be escaped. \since 15.0 */
void pa_json_encoder_add_element_string(pa_json_encoder *encoder, const char *value);
/** Append string member to JSON object. Value will be escaped. \since 15.0 */
void pa_json_encoder_add_member_string(pa_json_encoder *encoder, const char *name, const char *value);
/** Append integer element to JSON. \since 15.0 */
void pa_json_encoder_add_element_int(pa_json_encoder *encoder, int64_t value);
/** Append integer member to JSON object. \since 15.0 */
void pa_json_encoder_add_member_int(pa_json_encoder *encoder, const char *name, int64_t value);
/** Append double element to JSON. \since 15.0 */
void pa_json_encoder_add_element_double(pa_json_encoder *encoder, double value, int precision);
/** Append double member to JSON object. \since 15.0 */
void pa_json_encoder_add_member_double(pa_json_encoder *encoder, const char *name, double value, int precision);
/** Append raw json string element to JSON. String will be written as is. \since 15.0 */
void pa_json_encoder_add_element_raw_json(pa_json_encoder *encoder, const char *raw_json_string);
/** Append raw json string member to JSON object. String will be written as is. \since 15.0 */
void pa_json_encoder_add_member_raw_json(pa_json_encoder *encoder, const char *name, const char *raw_json_string);
