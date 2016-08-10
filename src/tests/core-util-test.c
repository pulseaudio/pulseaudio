/***
  This file is part of PulseAudio.

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

#include <signal.h>

#include <check.h>

#include <pulse/xmalloc.h>
#include <pulsecore/core-util.h>

START_TEST (modargs_test_parse_boolean) {
    ck_assert_int_eq(pa_parse_boolean("true"), true);
    ck_assert_int_eq(pa_parse_boolean("yes"), true);
    ck_assert_int_eq(pa_parse_boolean("1"), true);

    ck_assert_int_eq(pa_parse_boolean("false"), false);
    ck_assert_int_eq(pa_parse_boolean("no"), false);
    ck_assert_int_eq(pa_parse_boolean("0"), false);

    ck_assert_int_eq(pa_parse_boolean("maybe"), -1);
    ck_assert_int_eq(pa_parse_boolean("42"), -1);
}
END_TEST

START_TEST (modargs_test_parse_volume) {
    pa_volume_t value;

    // dB volumes
    ck_assert_int_eq(pa_parse_volume("-20dB", &value), 0);
    ck_assert_int_eq(value, 30419);
    ck_assert_int_eq(pa_parse_volume("-10dB", &value), 0);
    ck_assert_int_eq(value, 44649);
    ck_assert_int_eq(pa_parse_volume("-1dB", &value), 0);
    ck_assert_int_eq(value, 63069);
    ck_assert_int_eq(pa_parse_volume("0dB", &value), 0);
    ck_assert_int_eq(value, 65536);
    ck_assert_int_eq(pa_parse_volume("1dB", &value), 0);
    ck_assert_int_eq(value, 68100);
    ck_assert_int_eq(pa_parse_volume("10dB", &value), 0);
    ck_assert_int_eq(value, 96194);

    // lowercase db
    ck_assert_int_eq(pa_parse_volume("10db", &value), 0);
    ck_assert_int_eq(value, 96194);

    // percentage volumes
    ck_assert_int_eq(pa_parse_volume("0%", &value), 0);
    ck_assert_int_eq(value, 0);
    ck_assert_int_eq(pa_parse_volume("50%", &value), 0);
    ck_assert_int_eq(value, 32768);
    ck_assert_int_eq(pa_parse_volume("100%", &value), 0);
    ck_assert_int_eq(value, 65536);
    ck_assert_int_eq(pa_parse_volume("150%", &value), 0);
    ck_assert_int_eq(value, 98304);

    // integer volumes`
    ck_assert_int_eq(pa_parse_volume("0", &value), 0);
    ck_assert_int_eq(value, 0);
    ck_assert_int_eq(pa_parse_volume("100", &value), 0);
    ck_assert_int_eq(value, 100);
    ck_assert_int_eq(pa_parse_volume("1000", &value), 0);
    ck_assert_int_eq(value, 1000);
    ck_assert_int_eq(pa_parse_volume("65536", &value), 0);
    ck_assert_int_eq(value, 65536);
    ck_assert_int_eq(pa_parse_volume("100000", &value), 0);
    ck_assert_int_eq(value, 100000);

    // invalid volumes
    ck_assert_int_lt(pa_parse_volume("", &value), 0);
    ck_assert_int_lt(pa_parse_volume("-2", &value), 0);
    ck_assert_int_lt(pa_parse_volume("on", &value), 0);
    ck_assert_int_lt(pa_parse_volume("off", &value), 0);
    ck_assert_int_lt(pa_parse_volume("none", &value), 0);
}
END_TEST

START_TEST (modargs_test_atoi) {
    int32_t value;

    // decimal
    ck_assert_int_eq(pa_atoi("100000", &value), 0);
    ck_assert_int_eq(value, 100000);
    ck_assert_int_eq(pa_atoi("-100000", &value), 0);
    ck_assert_int_eq(value, -100000);

    // hexadecimal
    ck_assert_int_eq(pa_atoi("0x100000", &value), 0);
    ck_assert_int_eq(value, 0x100000);
    ck_assert_int_eq(pa_atoi("-0x100000", &value), 0);
    ck_assert_int_eq(value, -0x100000);

    // invalid values
    ck_assert_int_lt(pa_atoi("3.14", &value), 0);
    ck_assert_int_lt(pa_atoi("7*8", &value), 0);
    ck_assert_int_lt(pa_atoi("false", &value), 0);
}
END_TEST

START_TEST (modargs_test_atou) {
    uint32_t value;

    // decimal
    ck_assert_int_eq(pa_atou("100000", &value), 0);
    ck_assert_int_eq(value, 100000);

    // hexadecimal
    ck_assert_int_eq(pa_atou("0x100000", &value), 0);
    ck_assert_int_eq(value, 0x100000);

    // invalid values
    ck_assert_int_lt(pa_atou("-100000", &value), 0);
    ck_assert_int_lt(pa_atou("-0x100000", &value), 0);
    ck_assert_int_lt(pa_atou("3.14", &value), 0);
    ck_assert_int_lt(pa_atou("7*8", &value), 0);
    ck_assert_int_lt(pa_atou("false", &value), 0);
}
END_TEST

START_TEST (modargs_test_atol) {
    long value;

    // decimal
    ck_assert_int_eq(pa_atol("100000", &value), 0);
    ck_assert_int_eq(value, 100000l);
    ck_assert_int_eq(pa_atol("-100000", &value), 0);
    ck_assert_int_eq(value, -100000l);

    // hexadecimal
    ck_assert_int_eq(pa_atol("0x100000", &value), 0);
    ck_assert_int_eq(value, 0x100000l);
    ck_assert_int_eq(pa_atol("-0x100000", &value), 0);
    ck_assert_int_eq(value, -0x100000l);

    // invalid values
    ck_assert_int_lt(pa_atol("3.14", &value), 0);
    ck_assert_int_lt(pa_atol("7*8", &value), 0);
    ck_assert_int_lt(pa_atol("false", &value), 0);
}
END_TEST

START_TEST (modargs_test_atod) {
    double value;
    double epsilon = 0.001;

    // decimal
    ck_assert_int_eq(pa_atod("100000", &value), 0);
    ck_assert(value > 100000 - epsilon);
    ck_assert(value < 100000 + epsilon);
    ck_assert_int_eq(pa_atod("-100000", &value), 0);
    ck_assert(value > -100000 - epsilon);
    ck_assert(value < -100000 + epsilon);
    ck_assert_int_eq(pa_atod("3.14", &value), 0);
    ck_assert(value > 3.14 - epsilon);
    ck_assert(value < 3.14 + epsilon);

    // invalid values
    ck_assert_int_lt(pa_atod("7*8", &value), 0);
    ck_assert_int_lt(pa_atod("false", &value), 0);
}
END_TEST

START_TEST (modargs_test_replace) {
    char* value;

    value = pa_replace("abcde", "bcd", "XYZ");
    ck_assert_str_eq(value, "aXYZe");
    pa_xfree(value);

    value = pa_replace("abe", "b", "bab");
    ck_assert_str_eq(value, "ababe");
    pa_xfree(value);

    value = pa_replace("abe", "c", "bab");
    ck_assert_str_eq(value, "abe");
    pa_xfree(value);

    value = pa_replace("abcde", "bcd", "");
    ck_assert_str_eq(value, "ae");
    pa_xfree(value);
}
END_TEST

START_TEST (modargs_test_replace_fail_1) {
    pa_replace(NULL, "b", "bab");
}
END_TEST

START_TEST (modargs_test_replace_fail_2) {
    pa_replace("abe", NULL, "bab");
}
END_TEST

START_TEST (modargs_test_replace_fail_3) {
    pa_replace("abcde", "b", NULL);
}
END_TEST

START_TEST (modargs_test_escape) {
    char* value;

    value = pa_escape("abcde", "bcd");
    ck_assert_str_eq(value, "a\\b\\c\\de");
    pa_xfree(value);

    value = pa_escape("\\", "bcd");
    ck_assert_str_eq(value, "\\\\");
    pa_xfree(value);

    value = pa_escape("\\", NULL);
    ck_assert_str_eq(value, "\\\\");
    pa_xfree(value);
}
END_TEST

START_TEST (modargs_test_replace_fail_4) {
    pa_replace("abe", "", "bab");
}
END_TEST

START_TEST (modargs_test_unescape) {
    char* value;

    value = pa_unescape(pa_xstrdup("a\\b\\c\\de"));
    ck_assert_str_eq(value, "abcde");
    pa_xfree(value);

    value = pa_unescape(pa_xstrdup("\\\\"));
    ck_assert_str_eq(value, "\\");
    pa_xfree(value);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    s = suite_create("Core-Util");

    tc = tcase_create("core-util");
    suite_add_tcase(s, tc);
    tcase_add_test(tc, modargs_test_parse_boolean);
    tcase_add_test(tc, modargs_test_parse_volume);
    tcase_add_test(tc, modargs_test_atoi);
    tcase_add_test(tc, modargs_test_atou);
    tcase_add_test(tc, modargs_test_atol);
    tcase_add_test(tc, modargs_test_atod);
    tcase_add_test(tc, modargs_test_replace);
    tcase_add_test_raise_signal(tc, modargs_test_replace_fail_1, SIGABRT);
    tcase_add_test_raise_signal(tc, modargs_test_replace_fail_2, SIGABRT);
    tcase_add_test_raise_signal(tc, modargs_test_replace_fail_3, SIGABRT);
    tcase_add_test_raise_signal(tc, modargs_test_replace_fail_4, SIGABRT);
    tcase_add_test(tc, modargs_test_escape);
    tcase_add_test(tc, modargs_test_unescape);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
