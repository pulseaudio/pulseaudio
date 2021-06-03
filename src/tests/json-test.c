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

#include <check.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>
#include <pulsecore/json.h>

START_TEST (string_test) {
    pa_json_object *o;
    unsigned int i;
    const char *strings_parse[] = {
        "\"\"", "\"test\"", "\"test123\"", "\"123\"", "\"newline\\n\"", "\"  spaces \"",
        "   \"lots of spaces\"     ", "\"esc\\nape\"", "\"escape a \\\" quote\"",
    };
    const char *strings_compare[] = {
        "", "test", "test123", "123", "newline\n", "  spaces ",
        "lots of spaces", "esc\nape", "escape a \" quote",
    };

    for (i = 0; i < PA_ELEMENTSOF(strings_parse); i++) {
        o = pa_json_parse(strings_parse[i]);

        fail_unless(o != NULL);
        fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_STRING);
        fail_unless(pa_streq(pa_json_object_get_string(o), strings_compare[i]));

        pa_json_object_free(o);
    }
}
END_TEST

START_TEST (encoder_string_test) {
    const char *test_strings[] = {
        "", "test", "test123", "123", "newline\n", "  spaces ",
        "lots of spaces", "esc\nape", "escape a \" quote",
    };

    pa_json_object *o;
    unsigned int i;
    pa_json_encoder *encoder;
    const pa_json_object *v;
    char *received;

    encoder = pa_json_encoder_new();

    pa_json_encoder_begin_element_array(encoder);

    for (i = 0; i < PA_ELEMENTSOF(test_strings); i++) {
        pa_json_encoder_add_element_string(encoder, test_strings[i]);
    }

    pa_json_encoder_end_array(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == PA_ELEMENTSOF(test_strings));

    for (i = 0; i < PA_ELEMENTSOF(test_strings); i++) {
        v = pa_json_object_get_array_member(o, i);

        fail_unless(v != NULL);
        fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_STRING);
        fail_unless(pa_streq(pa_json_object_get_string(v), test_strings[i]));
    }

    pa_json_object_free(o);
}
END_TEST

START_TEST(int_test) {
    pa_json_object *o;
    unsigned int i;
    const char *ints_parse[] = { "1", "-1", "1234", "0" };
    const int64_t ints_compare[] = { 1, -1, 1234, 0 };
    char *uint64_max_str;

    for (i = 0; i < PA_ELEMENTSOF(ints_parse); i++) {
        o = pa_json_parse(ints_parse[i]);

        fail_unless(o != NULL);
        fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_INT);
        fail_unless(pa_json_object_get_int(o) == ints_compare[i]);

        pa_json_object_free(o);
    }

    /* test that parser would fail on integer overflow */
    uint64_max_str = pa_sprintf_malloc("%"PRIu64, UINT64_MAX);
    o = pa_json_parse(uint64_max_str);
    fail_unless(o == NULL);
    pa_xfree(uint64_max_str);
}
END_TEST

START_TEST(encoder_int_test) {
    const int64_t test_ints[] = { 1, -1, 1234, 0, LONG_MIN, LONG_MAX };

    pa_json_object *o;
    unsigned int i;
    pa_json_encoder *encoder;
    const pa_json_object *v;
    char *received;

    encoder = pa_json_encoder_new();

    pa_json_encoder_begin_element_array(encoder);

    for (i = 0; i < PA_ELEMENTSOF(test_ints); i++) {
        pa_json_encoder_add_element_int(encoder, test_ints[i]);
    }

    pa_json_encoder_end_array(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == PA_ELEMENTSOF(test_ints));

    for (i = 0; i < PA_ELEMENTSOF(test_ints); i++) {
        v = pa_json_object_get_array_member(o, i);

        fail_unless(v != NULL);
        fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_INT);
        fail_unless(pa_json_object_get_int(v) == test_ints[i]);
    }

    pa_json_object_free(o);
}
END_TEST

START_TEST(double_test) {
    pa_json_object *o;
    unsigned int i;
    char *very_large_double_str;
    const char *doubles_parse[] = {
        "1.0", "-1.1", "1234e2", "1234e0", "0.1234", "-0.1234", "1234e-1", "1234.5e-1", "1234.5e+2",
    };
    const double doubles_compare[] = {
        1.0, -1.1, 123400.0, 1234.0, 0.1234, -0.1234, 123.4, 123.45, 123450.0,
    };

    for (i = 0; i < PA_ELEMENTSOF(doubles_parse); i++) {
        o = pa_json_parse(doubles_parse[i]);

        fail_unless(o != NULL);
        fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_DOUBLE);
        fail_unless(PA_DOUBLE_IS_EQUAL(pa_json_object_get_double(o), doubles_compare[i]));

        pa_json_object_free(o);
    }

    /* test that parser would fail on double exponent overflow */
    very_large_double_str = pa_sprintf_malloc("%"PRIu64"e%"PRIu64, UINT64_MAX, UINT64_MAX);
    o = pa_json_parse(very_large_double_str);
    fail_unless(o == NULL);
    pa_xfree(very_large_double_str);
}
END_TEST

START_TEST(encoder_double_test) {
    const double test_doubles[] = {
        1.0, -1.1, 123400.0, 1234.0, 0.1234, -0.1234, 123.4, 123.45, 123450.0,
    };
    pa_json_object *o;
    unsigned int i;
    pa_json_encoder *encoder;
    const pa_json_object *v;
    char *received;

    encoder = pa_json_encoder_new();

    pa_json_encoder_begin_element_array(encoder);

    for (i = 0; i < PA_ELEMENTSOF(test_doubles); i++) {
        pa_json_encoder_add_element_double(encoder, test_doubles[i], 6);
    }

    pa_json_encoder_end_array(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == PA_ELEMENTSOF(test_doubles));

    for (i = 0; i < PA_ELEMENTSOF(test_doubles); i++) {
        v = pa_json_object_get_array_member(o, i);

        fail_unless(v != NULL);
        fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_DOUBLE);
        fail_unless(PA_DOUBLE_IS_EQUAL(pa_json_object_get_double(v), test_doubles[i]));
    }

    pa_json_object_free(o);
}
END_TEST

START_TEST(null_test) {
    pa_json_object *o;

    o = pa_json_parse("null");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_NULL);

    pa_json_object_free(o);
}
END_TEST

START_TEST(encoder_null_test) {
    pa_json_object *o;
    pa_json_encoder *encoder;
    char *received;

    encoder = pa_json_encoder_new();
    pa_json_encoder_add_element_null(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_NULL);

    pa_json_object_free(o);
}
END_TEST

START_TEST(bool_test) {
    pa_json_object *o;

    o = pa_json_parse("true");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(o) == true);

    pa_json_object_free(o);

    o = pa_json_parse("false");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(o) == false);

    pa_json_object_free(o);
}
END_TEST

START_TEST(encoder_bool_test) {
    const bool test_bools[] = {
        true, false
    };
    pa_json_object *o;
    unsigned int i;
    pa_json_encoder *encoder;
    const pa_json_object *v;
    char *received;

    encoder = pa_json_encoder_new();

    pa_json_encoder_begin_element_array(encoder);

    for (i = 0; i < PA_ELEMENTSOF(test_bools); i++) {
        pa_json_encoder_add_element_bool(encoder, test_bools[i]);
    }

    pa_json_encoder_end_array(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == PA_ELEMENTSOF(test_bools));

    for (i = 0; i < PA_ELEMENTSOF(test_bools); i++) {
        v = pa_json_object_get_array_member(o, i);

        fail_unless(v != NULL);
        fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_BOOL);
        fail_unless(PA_DOUBLE_IS_EQUAL(pa_json_object_get_bool(v), test_bools[i]));
    }

    pa_json_object_free(o);
}
END_TEST

START_TEST(object_test) {
    pa_json_object *o;
    const pa_json_object *v;

    o = pa_json_parse(" { \"name\" : \"A Person\" } ");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "name");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_STRING);
    fail_unless(pa_streq(pa_json_object_get_string(v), "A Person"));

    pa_json_object_free(o);

    o = pa_json_parse(" { \"age\" : -45.3e-0 } ");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "age");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_DOUBLE);
    fail_unless(PA_DOUBLE_IS_EQUAL(pa_json_object_get_double(v), -45.3));

    pa_json_object_free(o);

    o = pa_json_parse("{\"person\":true}");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "person");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(v) == true);

    pa_json_object_free(o);

    o = pa_json_parse("{ \"parent\": { \"child\": false } }");
    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "parent");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_OBJECT);
    v = pa_json_object_get_object_member(v, "child");
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(v) == false);

    pa_json_object_free(o);
}
END_TEST

START_TEST(object_member_iterator_test) {
    pa_json_object *o;
    const pa_json_object *v;
    const char *k;
    void *state;
    size_t i;

    struct {
        bool visited;
        const char *key;
        pa_json_type type;
        union {
            const char *str;
            int64_t n;
        } value;
    } expected_entries[] = {
            { .key = "name", .type = PA_JSON_TYPE_STRING, .value.str = "sample 1" },
            { .key = "number", .type = PA_JSON_TYPE_INT, .value.n = 42 },
    };

    o = pa_json_parse(" { \"name\" : \"sample 1\", \"number\": 42 } ");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    PA_HASHMAP_FOREACH_KV(k, v, pa_json_object_get_object_member_hashmap(o), state) {
        fail_unless(k != NULL);
        fail_unless(v != NULL);
        for (i = 0; i < PA_ELEMENTSOF(expected_entries); ++i) {
            if (pa_streq(expected_entries[i].key, k)) {
                fail_unless(!expected_entries[i].visited);
                fail_unless(expected_entries[i].type == pa_json_object_get_type(v));
                switch (expected_entries[i].type) {
                    case PA_JSON_TYPE_STRING:
                        fail_unless(pa_streq(expected_entries[i].value.str, pa_json_object_get_string(v)));
                        break;
                    case PA_JSON_TYPE_INT:
                        fail_unless(expected_entries[i].value.n == pa_json_object_get_int(v));
                        break;
                    default:
                        /* unreachable */
                        fail_unless(false);
                        break;
                }
                expected_entries[i].visited = true;
            }
        }
    }

    pa_json_object_free(o);
}
END_TEST

START_TEST(encoder_object_test) {
    pa_json_object *o;
    const pa_json_object *v;
    pa_json_encoder *encoder;
    char *received;

    /* { "name" : "A Person" } */

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(encoder);
    pa_json_encoder_add_member_string(encoder, "name", "A Person");
    pa_json_encoder_end_object(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "name");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_STRING);
    fail_unless(pa_streq(pa_json_object_get_string(v), "A Person"));

    pa_json_object_free(o);

    /* { "age" : -45.3e-0 } */

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(encoder);
    pa_json_encoder_add_member_double(encoder, "age", -45.3e-0, 2);
    pa_json_encoder_end_object(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "age");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_DOUBLE);
    fail_unless(PA_DOUBLE_IS_EQUAL(pa_json_object_get_double(v), -45.3));

    pa_json_object_free(o);

    /* {"person":true} */

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(encoder);
    pa_json_encoder_add_member_bool(encoder, "person", true);
    pa_json_encoder_end_object(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "person");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(v) == true);

    pa_json_object_free(o);
}
END_TEST

START_TEST(encoder_member_object_test) {
    pa_json_object *o;
    const pa_json_object *v;
    pa_json_encoder *encoder;
    char *received;

    /* { "parent": { "child": false } } */

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(encoder);

    pa_json_encoder_begin_member_object(encoder, "parent");
    pa_json_encoder_add_member_bool(encoder, "child", false);
    pa_json_encoder_end_object(encoder);

    pa_json_encoder_end_object(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "parent");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_OBJECT);
    v = pa_json_object_get_object_member(v, "child");
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(v) == false);

    pa_json_object_free(o);
}
END_TEST

START_TEST(array_test) {
    pa_json_object *o;
    const pa_json_object *v, *v2;

    o = pa_json_parse(" [  ] ");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == 0);

    pa_json_object_free(o);

    o = pa_json_parse("[\"a member\"]");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == 1);

    v = pa_json_object_get_array_member(o, 0);
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_STRING);
    fail_unless(pa_streq(pa_json_object_get_string(v), "a member"));

    pa_json_object_free(o);

    o = pa_json_parse("[\"a member\", 1234.5, { \"another\": true } ]");

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == 3);

    v = pa_json_object_get_array_member(o, 0);
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_STRING);
    fail_unless(pa_streq(pa_json_object_get_string(v), "a member"));
    v = pa_json_object_get_array_member(o, 1);
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_DOUBLE);
    fail_unless(PA_DOUBLE_IS_EQUAL(pa_json_object_get_double(v), 1234.5));
    v = pa_json_object_get_array_member(o, 2);
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_OBJECT);
    v2 =pa_json_object_get_object_member(v, "another");
    fail_unless(v2 != NULL);
    fail_unless(pa_json_object_get_type(v2) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(v2) == true);

    pa_json_object_free(o);
}
END_TEST

START_TEST(encoder_element_array_test) {
    pa_json_object *o;
    const pa_json_object *v, *v2;

    pa_json_encoder *encoder;
    char *received;
    pa_json_encoder *subobject;
    char *subobject_string;

    /* [  ] */
    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_array(encoder);
    pa_json_encoder_end_array(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == 0);

    pa_json_object_free(o);

    /* ["a member"] */

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_array(encoder);
    pa_json_encoder_add_element_string(encoder, "a member");
    pa_json_encoder_end_array(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == 1);

    v = pa_json_object_get_array_member(o, 0);
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_STRING);
    fail_unless(pa_streq(pa_json_object_get_string(v), "a member"));

    pa_json_object_free(o);

    /* [\"a member\", 1234.5, { \"another\": true } ] */

    subobject = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(subobject);
    pa_json_encoder_add_member_bool(subobject, "another", true);
    pa_json_encoder_end_object(subobject);
    subobject_string = pa_json_encoder_to_string_free(subobject);

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_array(encoder);
    pa_json_encoder_add_element_string(encoder, "a member");
    pa_json_encoder_add_element_double(encoder, 1234.5, 1);
    pa_json_encoder_add_element_raw_json(encoder, subobject_string);
    pa_xfree(subobject_string);
    pa_json_encoder_end_array(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(o) == 3);

    v = pa_json_object_get_array_member(o, 0);
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_STRING);
    fail_unless(pa_streq(pa_json_object_get_string(v), "a member"));
    v = pa_json_object_get_array_member(o, 1);
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_DOUBLE);
    fail_unless(PA_DOUBLE_IS_EQUAL(pa_json_object_get_double(v), 1234.5));
    v = pa_json_object_get_array_member(o, 2);
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_OBJECT);
    v2 =pa_json_object_get_object_member(v, "another");
    fail_unless(v2 != NULL);
    fail_unless(pa_json_object_get_type(v2) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(v2) == true);

    pa_json_object_free(o);
}
END_TEST

START_TEST(encoder_member_array_test) {
    pa_json_object *o;
    unsigned int i;
    const pa_json_object *v;
    const pa_json_object *e;
    pa_json_encoder *encoder;
    char *received;

    const int64_t test_ints[] = { 1, -1, 1234, 0, LONG_MIN, LONG_MAX };

    /* { "parameters": [ 1, -1, 1234, 0, -9223372036854775808, 9223372036854775807 ] } */


    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(encoder);

    pa_json_encoder_begin_member_array(encoder, "parameters");
    for (i = 0; i < PA_ELEMENTSOF(test_ints); i++) {
        pa_json_encoder_add_element_int(encoder, test_ints[i]);
    }
    pa_json_encoder_end_array(encoder);

    pa_json_encoder_end_object(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "parameters");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(v) == PA_ELEMENTSOF(test_ints));

    for (i = 0; i < PA_ELEMENTSOF(test_ints); i++) {
        e = pa_json_object_get_array_member(v, i);

        fail_unless(e != NULL);
        fail_unless(pa_json_object_get_type(e) == PA_JSON_TYPE_INT);
        fail_unless(pa_json_object_get_int(e) == test_ints[i]);
    }

    pa_json_object_free(o);
}
END_TEST

START_TEST(encoder_member_raw_json_test) {
    pa_json_object *o;
    const pa_json_object *v;
    const pa_json_object *e;
    pa_json_encoder *encoder;
    char *received;
    pa_json_encoder *subobject;
    char *subobject_string;

    /* { "parameters": [1, "a", 2.0] } */

    subobject = pa_json_encoder_new();
    pa_json_encoder_begin_element_array(subobject);
    pa_json_encoder_add_element_int(subobject, 1);
    pa_json_encoder_add_element_string(subobject, "a");
    pa_json_encoder_add_element_double(subobject, 2.0, 6);
    pa_json_encoder_end_array(subobject);
    subobject_string = pa_json_encoder_to_string_free(subobject);

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(encoder);

    pa_json_encoder_add_member_raw_json(encoder, "parameters", subobject_string);
    pa_xfree(subobject_string);

    pa_json_encoder_end_object(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "parameters");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_ARRAY);
    fail_unless(pa_json_object_get_array_length(v) == 3);
    e = pa_json_object_get_array_member(v, 0);
    fail_unless(e != NULL);
    fail_unless(pa_json_object_get_type(e) == PA_JSON_TYPE_INT);
    fail_unless(pa_json_object_get_int(e) == 1);
    e = pa_json_object_get_array_member(v, 1);
    fail_unless(e != NULL);
    fail_unless(pa_json_object_get_type(e) == PA_JSON_TYPE_STRING);
    fail_unless(pa_streq(pa_json_object_get_string(e), "a"));
    e = pa_json_object_get_array_member(v, 2);
    fail_unless(e != NULL);
    fail_unless(pa_json_object_get_type(e) == PA_JSON_TYPE_DOUBLE);
    fail_unless(PA_DOUBLE_IS_EQUAL(pa_json_object_get_double(e), 2.0));

    pa_json_object_free(o);

    /* { "parent": { "child": false } } */

    subobject = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(subobject);
    pa_json_encoder_add_member_bool(subobject, "child", false);
    pa_json_encoder_end_object(subobject);
    subobject_string = pa_json_encoder_to_string_free(subobject);

    encoder = pa_json_encoder_new();
    pa_json_encoder_begin_element_object(encoder);

    pa_json_encoder_add_member_raw_json(encoder, "parent", subobject_string);
    pa_xfree(subobject_string);

    pa_json_encoder_end_object(encoder);

    received = pa_json_encoder_to_string_free(encoder);
    o = pa_json_parse(received);
    pa_xfree(received);

    fail_unless(o != NULL);
    fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_OBJECT);

    v = pa_json_object_get_object_member(o, "parent");
    fail_unless(v != NULL);
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_OBJECT);
    v = pa_json_object_get_object_member(v, "child");
    fail_unless(pa_json_object_get_type(v) == PA_JSON_TYPE_BOOL);
    fail_unless(pa_json_object_get_bool(v) == false);

    pa_json_object_free(o);
}
END_TEST

START_TEST(bad_test) {
    unsigned int i;
    const char *bad_parse[] = {
        "\"" /* Quote not closed */,
        "123456789012345678901234567890" /* Overflow */,
#if 0   /* TODO: check rounding the value is OK */
        "0.123456789012345678901234567890" /* Overflow */,
#endif
        "1e123456789012345678901234567890" /* Overflow */,
        "1e-10000" /* Underflow */,
        "1e" /* Bad number string */,
        "1." /* Bad number string */,
        "1.e3" /* Bad number string */,
        "-" /* Bad number string */,
        "{ \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": { \"a\": {  \"a\": { } } } } } } } } } } } } } } } } } } } } } }" /* Nested too deep */,
        "[ [ [ [ [ [ [ [ [ [ [ [ [ [ [ [ [ [ [ [ { \"a\": \"b\" } ] ] ] ] ] ] ] ] ] ] ] ] ] ] ] ] ] ] ] ]" /* Nested too deep */,
        "asdf" /* Unquoted string */,
        "{ a: true }" /* Unquoted key in object */,
        "\"    \a\"" /* Alarm is not a valid character */
    };

    for (i = 0; i < PA_ELEMENTSOF(bad_parse); i++) {
        pa_json_object *obj;

        fail_unless((obj = pa_json_parse(bad_parse[i])) == NULL);
        if (obj)
            pa_json_object_free(obj);
    }
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    s = suite_create("JSON");
    tc = tcase_create("json");
    tcase_add_test(tc, string_test);
    tcase_add_test(tc, encoder_string_test);
    tcase_add_test(tc, int_test);
    tcase_add_test(tc, encoder_int_test);
    tcase_add_test(tc, double_test);
    tcase_add_test(tc, encoder_double_test);
    tcase_add_test(tc, null_test);
    tcase_add_test(tc, encoder_null_test);
    tcase_add_test(tc, bool_test);
    tcase_add_test(tc, encoder_bool_test);
    tcase_add_test(tc, object_test);
    tcase_add_test(tc, encoder_member_object_test);
    tcase_add_test(tc, object_member_iterator_test);
    tcase_add_test(tc, encoder_object_test);
    tcase_add_test(tc, array_test);
    tcase_add_test(tc, encoder_element_array_test);
    tcase_add_test(tc, encoder_member_array_test);
    tcase_add_test(tc, encoder_member_raw_json_test);
    tcase_add_test(tc, bad_test);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
