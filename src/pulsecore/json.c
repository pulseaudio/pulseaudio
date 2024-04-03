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

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <math.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/hashmap.h>
#include <pulsecore/json.h>
#include <pulsecore/strbuf.h>

#define MAX_NESTING_DEPTH 20 /* Arbitrary number to make sure we don't have a stack overflow */

struct pa_json_object {
    pa_json_type type;

    union {
        int64_t int_value;
        double double_value;
        bool bool_value;
        char *string_value;
        pa_hashmap *object_values; /* name -> object */
        pa_idxset *array_values; /* objects */
    };
};

/* JSON encoder context type */
typedef enum pa_json_context_type {
    /* Top-level context of empty encoder. JSON element can be added. */
    PA_JSON_CONTEXT_EMPTY  = 0,
    /* Top-level context of encoder with an element. JSON element cannot be added. */
    PA_JSON_CONTEXT_TOP    = 1,
    /* JSON array context. JSON elements can be added. */
    PA_JSON_CONTEXT_ARRAY  = 2,
    /* JSON object context. JSON object members can be added. */
    PA_JSON_CONTEXT_OBJECT = 3,
} pa_json_context_type_t;

typedef struct encoder_context {
    pa_json_context_type_t type;
    int counter;
    struct encoder_context *next;
} encoder_context;

/* JSON encoder structure, a wrapper for pa_strbuf and encoder context */
struct pa_json_encoder {
    pa_strbuf *buffer;
    encoder_context *context;
};

static const char* parse_value(const char *str, const char *end, pa_json_object **obj, unsigned int depth);

static pa_json_object* json_object_new(void) {
    pa_json_object *obj;

    obj = pa_xnew0(pa_json_object, 1);

    return obj;
}

static bool is_whitespace(char c) {
    return c == '\t' || c == '\n' || c == '\r' || c == ' ';
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

static bool is_end(const char c, const char *end) {
    if (!end)
        return c == '\0';
    else  {
        while (*end) {
            if (c == *end)
                return true;
            end++;
        }
    }

    return false;
}

static const char* consume_string(const char *str, const char *expect) {
    while (*expect) {
        if (*str != *expect)
            return NULL;

        str++;
        expect++;
    }

    return str;
}

static const char* parse_null(const char *str, pa_json_object *obj) {
    str = consume_string(str, "null");

    if (str)
        obj->type = PA_JSON_TYPE_NULL;

    return str;
}

static const char* parse_boolean(const char *str, pa_json_object *obj) {
    const char *tmp;

    tmp = consume_string(str, "true");

    if (tmp) {
        obj->type = PA_JSON_TYPE_BOOL;
        obj->bool_value = true;
    } else {
        tmp = consume_string(str, "false");

        if (str) {
            obj->type = PA_JSON_TYPE_BOOL;
            obj->bool_value = false;
        }
    }

    return tmp;
}

static const char* parse_string(const char *str, pa_json_object *obj) {
    pa_strbuf *buf = pa_strbuf_new();

    str++; /* Consume leading '"' */

    while (*str && *str != '"') {
        if (*str != '\\') {
            /* JSON specifies that ASCII control characters 0x00 through 0x1F
             * must not appear in the string. */
            if (*str < 0x20) {
                pa_log("Invalid ASCII character: 0x%x", (unsigned int) *str);
                goto error;
            }

            /* Normal character, juts consume */
            pa_strbuf_putc(buf, *str);
        } else {
            /* Need to unescape */
            str++;

            switch (*str) {
                case '"':
                case '\\':
                case '/':
                    pa_strbuf_putc(buf, *str);
                    break;

                case 'b':
                    pa_strbuf_putc(buf, '\b' /* backspace */);
                    break;

                case 'f':
                    pa_strbuf_putc(buf, '\f' /* form feed */);
                    break;

                case 'n':
                    pa_strbuf_putc(buf, '\n' /* new line */);
                    break;

                case 'r':
                    pa_strbuf_putc(buf, '\r' /* carriage return */);
                    break;

                case 't':
                    pa_strbuf_putc(buf, '\t' /* horizontal tab */);
                    break;

                case 'u':
                    pa_log("Unicode code points are currently unsupported");
                    goto error;

                default:
                    pa_log("Unexpected escape value: %c", *str);
                    goto error;
            }
        }

        str++;
    }

    if (*str != '"') {
        pa_log("Failed to parse remainder of string: %s", str);
        goto error;
    }

    str++;

    obj->type = PA_JSON_TYPE_STRING;
    obj->string_value = pa_strbuf_to_string_free(buf);

    return str;

error:
    pa_strbuf_free(buf);
    return NULL;
}

static const char* parse_number(const char *str, pa_json_object *obj) {
    bool has_fraction = false, has_exponent = false, valid = false;
    char *candidate = NULL;
    const char *s = str;

    if (*s == '-')
        s++;

    if (*s == '0') {
        valid = true;
        s++;
        goto fraction;
    }

    while (is_digit(*s)) {
        valid = true;
        s++;
    }

fraction:

    if (!valid) {
        pa_log("Missing digits while parsing number");
        goto error;
    }

    if (*s == '.') {
        has_fraction = true;
        s++;
        valid = false;

        while (is_digit(*s)) {
            valid = true;
            s++;
        }

        if (!valid) {
            pa_log("No digit after '.' while parsing fraction");
            goto error;
        }
    }

    if (*s == 'e' || *s == 'E') {
        has_exponent = true;
        s++;
        valid = false;

        if (*s == '-' || *s == '+')
            s++;

        while (is_digit(*s)) {
            valid = true;
            s++;
        }

        if (!valid) {
            pa_log("No digit in exponent while parsing fraction");
            goto error;
        }
    }

    /* Number format looks good, now try to extract the value.
     * Here 's' points just after the string which will be consumed. */

    candidate = pa_xstrndup(str, s - str);

    if (has_fraction || has_exponent) {
        if (pa_atod(candidate, &obj->double_value) < 0) {
            pa_log("Cannot convert string '%s' to double value", str);
            goto error;
        }
        obj->type = PA_JSON_TYPE_DOUBLE;
    } else {
        if (pa_atoi64(candidate, &obj->int_value) < 0) {
            pa_log("Cannot convert string '%s' to int64_t value", str);
            goto error;
        }
        obj->type = PA_JSON_TYPE_INT;
    }

    pa_xfree(candidate);

    return s;

error:
    pa_xfree(candidate);
    return NULL;
}

static const char *parse_object(const char *str, pa_json_object *obj, unsigned int depth) {
    pa_json_object *name = NULL, *value = NULL;

    obj->object_values = pa_hashmap_new_full(pa_idxset_string_hash_func, pa_idxset_string_compare_func,
                                             pa_xfree, (pa_free_cb_t) pa_json_object_free);

    while (*str != '}') {
        str++; /* Consume leading '{' or ',' */

        str = parse_value(str, ":", &name, depth + 1);
        if (!str || pa_json_object_get_type(name) != PA_JSON_TYPE_STRING) {
            pa_log("Could not parse key for object");
            goto error;
        }

        /* Consume the ':' */
        str++;

        str = parse_value(str, ",}", &value, depth + 1);
        if (!str) {
            pa_log("Could not parse value for object");
            goto error;
        }

        pa_hashmap_put(obj->object_values, pa_xstrdup(pa_json_object_get_string(name)), value);
        pa_json_object_free(name);

        name = NULL;
        value = NULL;
    }

    /* Drop trailing '}' */
    str++;

    /* We now know the value was correctly parsed */
    obj->type = PA_JSON_TYPE_OBJECT;

    return str;

error:
    pa_hashmap_free(obj->object_values);
    obj->object_values = NULL;

    if (name)
        pa_json_object_free(name);
    if (value)
        pa_json_object_free(value);

    return NULL;
}

static const char *parse_array(const char *str, pa_json_object *obj, unsigned int depth) {
    pa_json_object *value;

    obj->array_values = pa_idxset_new(NULL, NULL);

    while (*str != ']') {
        str++; /* Consume leading '[' or ',' */

        /* Need to chew up whitespaces as a special case to deal with the
         * possibility of an empty array */
        while (is_whitespace(*str))
            str++;

        if (*str == ']')
            break;

        str = parse_value(str, ",]", &value, depth + 1);
        if (!str) {
            pa_log("Could not parse value for array");
            goto error;
        }

        pa_idxset_put(obj->array_values, value, NULL);
    }

    /* Drop trailing ']' */
    str++;

    /* We now know the value was correctly parsed */
    obj->type = PA_JSON_TYPE_ARRAY;

    return str;

error:
    pa_idxset_free(obj->array_values, (pa_free_cb_t) pa_json_object_free);
    obj->array_values = NULL;
    return NULL;
}

typedef enum {
    JSON_PARSER_STATE_INIT,
    JSON_PARSER_STATE_FINISH,
} json_parser_state;

static const char* parse_value(const char *str, const char *end, pa_json_object **obj, unsigned int depth) {
    json_parser_state state = JSON_PARSER_STATE_INIT;
    pa_json_object *o;

    pa_assert(str != NULL);

    o = json_object_new();

    if (depth > MAX_NESTING_DEPTH) {
        pa_log("Exceeded maximum permitted nesting depth of objects (%u)", MAX_NESTING_DEPTH);
        goto error;
    }

    while (!is_end(*str, end)) {
        switch (state) {
            case JSON_PARSER_STATE_INIT:
                if (is_whitespace(*str)) {
                    str++;
                } else if (*str == 'n') {
                    str = parse_null(str, o);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (*str == 't' || *str == 'f') {
                    str = parse_boolean(str, o);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (*str == '"') {
                    str = parse_string(str, o);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (is_digit(*str) || *str == '-') {
                    str = parse_number(str, o);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (*str == '{') {
                    str = parse_object(str, o, depth);
                    state = JSON_PARSER_STATE_FINISH;
                } else if (*str == '[') {
                    str = parse_array(str, o, depth);
                    state = JSON_PARSER_STATE_FINISH;
                } else {
                    pa_log("Invalid JSON string: %s", str);
                    goto error;
                }

                if (!str)
                    goto error;

                break;

            case JSON_PARSER_STATE_FINISH:
                /* Consume trailing whitespaces */
                if (is_whitespace(*str)) {
                    str++;
                } else {
                    goto error;
                }
        }
    }

    if (pa_json_object_get_type(o) == PA_JSON_TYPE_INIT) {
        /* We didn't actually get any data */
        pa_log("No data while parsing json string: '%s' till '%s'", str, pa_strnull(end));
        goto error;
    }

    *obj = o;

    return str;

error:
    pa_json_object_free(o);
    return NULL;
}


pa_json_object* pa_json_parse(const char *str) {
    pa_json_object *obj;

    str = parse_value(str, NULL, &obj, 0);

    if (!str) {
        pa_log("JSON parsing failed");
        return NULL;
    }

    if (*str != '\0') {
        pa_log("Unable to parse complete JSON string, remainder is: %s", str);
        pa_json_object_free(obj);
        return NULL;
    }

    return obj;
}

pa_json_type pa_json_object_get_type(const pa_json_object *obj) {
    return obj->type;
}

void pa_json_object_free(pa_json_object *obj) {

    switch (pa_json_object_get_type(obj)) {
        case PA_JSON_TYPE_INIT:
        case PA_JSON_TYPE_INT:
        case PA_JSON_TYPE_DOUBLE:
        case PA_JSON_TYPE_BOOL:
        case PA_JSON_TYPE_NULL:
            break;

        case PA_JSON_TYPE_STRING:
            pa_xfree(obj->string_value);
            break;

        case PA_JSON_TYPE_OBJECT:
            pa_hashmap_free(obj->object_values);
            break;

        case PA_JSON_TYPE_ARRAY:
            pa_idxset_free(obj->array_values, (pa_free_cb_t) pa_json_object_free);
            break;

        default:
            pa_assert_not_reached();
    }

    pa_xfree(obj);
}

int64_t pa_json_object_get_int(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_INT);
    return o->int_value;
}

double pa_json_object_get_double(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_DOUBLE);
    return o->double_value;
}

bool pa_json_object_get_bool(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_BOOL);
    return o->bool_value;
}

const char* pa_json_object_get_string(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_STRING);
    return o->string_value;
}

const pa_json_object* pa_json_object_get_object_member(const pa_json_object *o, const char *name) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);
    return pa_hashmap_get(o->object_values, name);
}

const pa_hashmap *pa_json_object_get_object_member_hashmap(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);
    return o->object_values;
}

int pa_json_object_get_array_length(const pa_json_object *o) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    return pa_idxset_size(o->array_values);
}

const pa_json_object* pa_json_object_get_array_member(const pa_json_object *o, int index) {
    pa_assert(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    return pa_idxset_get_by_index(o->array_values, index);
}

bool pa_json_object_equal(const pa_json_object *o1, const pa_json_object *o2) {
    int i;

    if (pa_json_object_get_type(o1) != pa_json_object_get_type(o2))
        return false;

    switch (pa_json_object_get_type(o1)) {
        case PA_JSON_TYPE_NULL:
            return true;

        case PA_JSON_TYPE_BOOL:
            return o1->bool_value == o2->bool_value;

        case PA_JSON_TYPE_INT:
            return o1->int_value == o2->int_value;

        case PA_JSON_TYPE_DOUBLE:
            return PA_DOUBLE_IS_EQUAL(o1->double_value, o2->double_value);

        case PA_JSON_TYPE_STRING:
            return pa_streq(o1->string_value, o2->string_value);

        case PA_JSON_TYPE_ARRAY:
            if (pa_json_object_get_array_length(o1) != pa_json_object_get_array_length(o2))
                return false;

            for (i = 0; i < pa_json_object_get_array_length(o1); i++) {
                if (!pa_json_object_equal(pa_json_object_get_array_member(o1, i),
                            pa_json_object_get_array_member(o2, i)))
                    return false;
            }

            return true;

        case PA_JSON_TYPE_OBJECT: {
            void *state;
            const char *key;
            const pa_json_object *v1, *v2;

            if (pa_hashmap_size(o1->object_values) != pa_hashmap_size(o2->object_values))
                return false;

            PA_HASHMAP_FOREACH_KV(key, v1, o1->object_values, state) {
                v2 = pa_json_object_get_object_member(o2, key);
                if (!v2 || !pa_json_object_equal(v1, v2))
                    return false;
            }

            return true;
        }

        default:
            pa_assert_not_reached();
    }
}

/* Write functions. The functions are wrapper functions around pa_strbuf,
 * so that the client does not need to use pa_strbuf directly. */

static void json_encoder_context_push(pa_json_encoder *encoder, pa_json_context_type_t type) {
    pa_assert(encoder);

    encoder_context *head = pa_xnew0(encoder_context, 1);
    head->type = type;
    head->next = encoder->context;
    encoder->context = head;
}

/* Returns type of context popped off encoder context stack. */
static pa_json_context_type_t json_encoder_context_pop(pa_json_encoder *encoder) {
    encoder_context *head;
    pa_json_context_type_t type;

    pa_assert(encoder);
    pa_assert(encoder->context);

    type = encoder->context->type;

    head = encoder->context->next;
    pa_xfree(encoder->context);
    encoder->context = head;

    return type;
}

bool pa_json_encoder_is_empty(pa_json_encoder *encoder) {
    pa_json_context_type_t type;

    pa_assert(encoder);
    pa_assert(encoder->context);

    type = encoder->context->type;
    return type == PA_JSON_CONTEXT_EMPTY;
}

pa_json_encoder *pa_json_encoder_new(void) {
    pa_json_encoder *encoder;

    encoder = pa_xnew(pa_json_encoder, 1);
    encoder->buffer = pa_strbuf_new();

    encoder->context = NULL;
    json_encoder_context_push(encoder, PA_JSON_CONTEXT_EMPTY);

    return encoder;
}

void pa_json_encoder_free(pa_json_encoder *encoder) {
    pa_json_context_type_t type;
    pa_assert(encoder);

    /* should have exactly one encoder context left at this point */
    pa_assert(encoder->context);
    type = json_encoder_context_pop(encoder);
    pa_assert(encoder->context == NULL);

    pa_assert(type == PA_JSON_CONTEXT_TOP || type == PA_JSON_CONTEXT_EMPTY);
    if (type == PA_JSON_CONTEXT_EMPTY)
        pa_log_warn("JSON encoder is empty.");

    if (encoder->buffer)
        pa_strbuf_free(encoder->buffer);

    pa_xfree(encoder);
}

char *pa_json_encoder_to_string_free(pa_json_encoder *encoder) {
    char *result;

    pa_assert(encoder);

    result = pa_strbuf_to_string_free(encoder->buffer);

    encoder->buffer = NULL;
    pa_json_encoder_free(encoder);

    return result;
}

static void json_encoder_insert_delimiter(pa_json_encoder *encoder) {
    pa_assert(encoder);

    if (encoder->context->counter++)
        pa_strbuf_putc(encoder->buffer, ',');
}

/* Escapes p to create valid JSON string.
 * The caller has to free the returned string. */
static char *pa_json_escape(const char *p) {
    const char *s;
    char *out_string, *output;
    int char_count = strlen(p);

    /* Maximum number of characters in output string
     * including trailing 0. */
    char_count = 2 * char_count + 1;

    /* allocate output string */
    out_string = pa_xmalloc(char_count);
    output = out_string;

    /* write output string */
    for (s = p; *s; ++s) {
        switch (*s) {
            case '"':
                *output++ = '\\';
                *output++ = '"';
                break;
            case '\\':
                *output++ = '\\';
                *output++ = '\\';
                break;
            case '\b':
                *output++ = '\\';
                *output++ = 'b';
                break;

            case '\f':
                *output++ = '\\';
                *output++ = 'f';
                break;

            case '\n':
                *output++ = '\\';
                *output++ = 'n';
                break;

            case '\r':
                *output++ = '\\';
                *output++ = 'r';
                break;

            case '\t':
                *output++ = '\\';
                *output++ = 't';
                break;
            default:
                if (*s < 0x20 || *s > 0x7E) {
                    pa_log("Invalid non-ASCII character: 0x%x", (unsigned int) *s);
                    pa_xfree(out_string);
                    return NULL;
                }
                *output++ = *s;
                break;
        }
    }

    *output = 0;

    return out_string;
}

static void json_write_string_escaped(pa_json_encoder *encoder, const char *value) {
    char *escaped_value;

    pa_assert(encoder);

    escaped_value = pa_json_escape(value);
    pa_strbuf_printf(encoder->buffer, "\"%s\"", escaped_value);
    pa_xfree(escaped_value);
}

/* Writes an opening curly brace */
void pa_json_encoder_begin_element_object(pa_json_encoder *encoder) {
    pa_assert(encoder);
    pa_assert(encoder->context->type != PA_JSON_CONTEXT_TOP);

    if (encoder->context->type == PA_JSON_CONTEXT_EMPTY)
        encoder->context->type = PA_JSON_CONTEXT_TOP;

    json_encoder_insert_delimiter(encoder);
    pa_strbuf_putc(encoder->buffer, '{');

    json_encoder_context_push(encoder, PA_JSON_CONTEXT_OBJECT);
}

/* Writes an opening curly brace */
void pa_json_encoder_begin_member_object(pa_json_encoder *encoder, const char *name) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_OBJECT);
    pa_assert(name && name[0]);

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, name);
    pa_strbuf_putc(encoder->buffer, ':');

    pa_strbuf_putc(encoder->buffer, '{');

    json_encoder_context_push(encoder, PA_JSON_CONTEXT_OBJECT);
}

/* Writes a closing curly brace */
void pa_json_encoder_end_object(pa_json_encoder *encoder) {
    pa_json_context_type_t type;
    pa_assert(encoder);

    type = json_encoder_context_pop(encoder);
    pa_assert(type == PA_JSON_CONTEXT_OBJECT);

    pa_strbuf_putc(encoder->buffer, '}');
}

/* Writes an opening bracket */
void pa_json_encoder_begin_element_array(pa_json_encoder *encoder) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type != PA_JSON_CONTEXT_TOP);

    if (encoder->context->type == PA_JSON_CONTEXT_EMPTY)
        encoder->context->type = PA_JSON_CONTEXT_TOP;

    json_encoder_insert_delimiter(encoder);
    pa_strbuf_putc(encoder->buffer, '[');

    json_encoder_context_push(encoder, PA_JSON_CONTEXT_ARRAY);
}

/* Writes member name and an opening bracket */
void pa_json_encoder_begin_member_array(pa_json_encoder *encoder, const char *name) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_OBJECT);
    pa_assert(name && name[0]);

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, name);
    pa_strbuf_putc(encoder->buffer, ':');

    pa_strbuf_putc(encoder->buffer, '[');

    json_encoder_context_push(encoder, PA_JSON_CONTEXT_ARRAY);
}

/* Writes a closing bracket */
void pa_json_encoder_end_array(pa_json_encoder *encoder) {
    pa_json_context_type_t type;
    pa_assert(encoder);

    type = json_encoder_context_pop(encoder);
    pa_assert(type == PA_JSON_CONTEXT_ARRAY);

    pa_strbuf_putc(encoder->buffer, ']');
}

void pa_json_encoder_add_element_string(pa_json_encoder *encoder, const char *value) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_EMPTY || encoder->context->type == PA_JSON_CONTEXT_ARRAY);

    if (encoder->context->type == PA_JSON_CONTEXT_EMPTY)
        encoder->context->type = PA_JSON_CONTEXT_TOP;

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, value);
}

void pa_json_encoder_add_member_string(pa_json_encoder *encoder, const char *name, const char *value) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_OBJECT);
    pa_assert(name && name[0]);

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, name);

    pa_strbuf_putc(encoder->buffer, ':');

    /* Null value is written as empty element */
    if (!value)
        value = "";

    json_write_string_escaped(encoder, value);
}

static void json_write_null(pa_json_encoder *encoder) {
    pa_assert(encoder);

    pa_strbuf_puts(encoder->buffer, "null");
}

void pa_json_encoder_add_element_null(pa_json_encoder *encoder) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_EMPTY || encoder->context->type == PA_JSON_CONTEXT_ARRAY);

    if (encoder->context->type == PA_JSON_CONTEXT_EMPTY)
        encoder->context->type = PA_JSON_CONTEXT_TOP;

    json_encoder_insert_delimiter(encoder);

    json_write_null(encoder);
}

void pa_json_encoder_add_member_null(pa_json_encoder *encoder, const char *name) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_OBJECT);
    pa_assert(name && name[0]);

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, name);
    pa_strbuf_putc(encoder->buffer, ':');

    json_write_null(encoder);
}

static void json_write_bool(pa_json_encoder *encoder, bool value) {
    pa_assert(encoder);

    pa_strbuf_puts(encoder->buffer, value ? "true" : "false");
}

void pa_json_encoder_add_element_bool(pa_json_encoder *encoder, bool value) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_EMPTY || encoder->context->type == PA_JSON_CONTEXT_ARRAY);

    if (encoder->context->type == PA_JSON_CONTEXT_EMPTY)
        encoder->context->type = PA_JSON_CONTEXT_TOP;

    json_encoder_insert_delimiter(encoder);

    json_write_bool(encoder, value);
}

void pa_json_encoder_add_member_bool(pa_json_encoder *encoder, const char *name, bool value) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_OBJECT);
    pa_assert(name && name[0]);

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, name);

    pa_strbuf_putc(encoder->buffer, ':');

    json_write_bool(encoder, value);
}

static void json_write_int(pa_json_encoder *encoder, int64_t value) {
    pa_assert(encoder);

    pa_strbuf_printf(encoder->buffer, "%"PRId64, value);
}

void pa_json_encoder_add_element_int(pa_json_encoder *encoder, int64_t value) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_EMPTY || encoder->context->type == PA_JSON_CONTEXT_ARRAY);

    if (encoder->context->type == PA_JSON_CONTEXT_EMPTY)
        encoder->context->type = PA_JSON_CONTEXT_TOP;

    json_encoder_insert_delimiter(encoder);

    json_write_int(encoder, value);
}

void pa_json_encoder_add_member_int(pa_json_encoder *encoder, const char *name, int64_t value) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_OBJECT);
    pa_assert(name && name[0]);

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, name);

    pa_strbuf_putc(encoder->buffer, ':');

    json_write_int(encoder, value);
}

static void json_write_double(pa_json_encoder *encoder, double value, int precision) {
    pa_assert(encoder);
    pa_strbuf_printf(encoder->buffer, "%.*f",  precision, value);
}

void pa_json_encoder_add_element_double(pa_json_encoder *encoder, double value, int precision) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_EMPTY || encoder->context->type == PA_JSON_CONTEXT_ARRAY);

    if (encoder->context->type == PA_JSON_CONTEXT_EMPTY)
        encoder->context->type = PA_JSON_CONTEXT_TOP;

    json_encoder_insert_delimiter(encoder);

    json_write_double(encoder, value, precision);
}

void pa_json_encoder_add_member_double(pa_json_encoder *encoder, const char *name, double value, int precision) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_OBJECT);
    pa_assert(name && name[0]);

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, name);

    pa_strbuf_putc(encoder->buffer, ':');

    json_write_double(encoder, value, precision);
}

static void json_write_raw(pa_json_encoder *encoder, const char *raw_string) {
    pa_assert(encoder);
    pa_strbuf_puts(encoder->buffer, raw_string);
}

void pa_json_encoder_add_element_raw_json(pa_json_encoder *encoder, const char *raw_json_string) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_EMPTY || encoder->context->type == PA_JSON_CONTEXT_ARRAY);

    if (encoder->context->type == PA_JSON_CONTEXT_EMPTY)
        encoder->context->type = PA_JSON_CONTEXT_TOP;

    json_encoder_insert_delimiter(encoder);

    json_write_raw(encoder, raw_json_string);
}

void pa_json_encoder_add_member_raw_json(pa_json_encoder *encoder, const char *name, const char *raw_json_string) {
    pa_assert(encoder);
    pa_assert(encoder->context);
    pa_assert(encoder->context->type == PA_JSON_CONTEXT_OBJECT);
    pa_assert(name && name[0]);

    json_encoder_insert_delimiter(encoder);

    json_write_string_escaped(encoder, name);

    pa_strbuf_putc(encoder->buffer, ':');

    json_write_raw(encoder, raw_json_string);
}
