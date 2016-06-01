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

#include <pulse/json.h>
#include <pulsecore/core-util.h>

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

START_TEST(int_test) {
    pa_json_object *o;
    unsigned int i;
    const char *ints_parse[] = { "1", "-1", "1234", "0" };
    const int ints_compare[] = { 1, -1, 1234, 0 };

    for (i = 0; i < PA_ELEMENTSOF(ints_parse); i++) {
        o = pa_json_parse(ints_parse[i]);

        fail_unless(o != NULL);
        fail_unless(pa_json_object_get_type(o) == PA_JSON_TYPE_INT);
        fail_unless(pa_json_object_get_int(o) == ints_compare[i]);

        pa_json_object_free(o);
    }
}
END_TEST

START_TEST(double_test) {
    pa_json_object *o;
    unsigned int i;
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

START_TEST(bad_test) {
    unsigned int i;
    const char *bad_parse[] = {
        "\"" /* Quote not closed */,
        "123456789012345678901234567890" /* Overflow */,
        "0.123456789012345678901234567890" /* Overflow */,
        "1e123456789012345678901234567890" /* Overflow */,
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
        fail_unless(pa_json_parse(bad_parse[i]) == NULL);
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
    tcase_add_test(tc, int_test);
    tcase_add_test(tc, double_test);
    tcase_add_test(tc, null_test);
    tcase_add_test(tc, bool_test);
    tcase_add_test(tc, object_test);
    tcase_add_test(tc, array_test);
    tcase_add_test(tc, bad_test);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
