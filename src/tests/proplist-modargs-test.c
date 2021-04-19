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

#include <stdio.h>

#include <check.h>

#include <pulse/proplist.h>
#include <pulse/xmalloc.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>

START_TEST (proplist_modargs_test) {
    pa_modargs *ma;
    pa_proplist *a;
    char *v;
    const char *x[] = { "foo", NULL };

    ma = pa_modargs_new("foo='foobar=waldo foo2=\"lj\\\"dhflh\" foo3=\"kjlskj\\'\"'", x);
    fail_unless(ma != NULL);
    a = pa_proplist_new();
    fail_unless(a != NULL);

    fail_unless(pa_modargs_get_proplist(ma, "foo", a, PA_UPDATE_REPLACE) >= 0);

    pa_log_debug("%s", v = pa_proplist_to_string(a));
    pa_xfree(v);

    pa_proplist_free(a);
    pa_modargs_free(ma);
}
END_TEST

int main(int argc, char *argv[]) {
    int failed = 0;
    Suite *s;
    TCase *tc;
    SRunner *sr;

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    s = suite_create("Property List");
    tc = tcase_create("propertylist");
    tcase_add_test(tc, proplist_modargs_test);
    suite_add_tcase(s, tc);

    sr = srunner_create(s);
    srunner_run_all(sr, CK_NORMAL);
    failed = srunner_ntests_failed(sr);
    srunner_free(sr);

    return (failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}
