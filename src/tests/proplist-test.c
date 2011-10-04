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
  along with PulseAudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdio.h>

#include <pulse/proplist.h>
#include <pulse/xmalloc.h>
#include <pulsecore/macro.h>
#include <pulsecore/core-util.h>
#include <pulsecore/modargs.h>

int main(int argc, char*argv[]) {
    pa_modargs *ma;
    pa_proplist *a, *b, *c, *d;
    char *s, *t, *u, *v;
    const char *text;
    const char *x[] = { "foo", NULL };

    if (!getenv("MAKE_CHECK"))
        pa_log_set_level(PA_LOG_DEBUG);

    a = pa_proplist_new();
    pa_assert_se(pa_proplist_sets(a, PA_PROP_MEDIA_TITLE, "Brandenburgische Konzerte") == 0);
    pa_assert_se(pa_proplist_sets(a, PA_PROP_MEDIA_ARTIST, "Johann Sebastian Bach") == 0);

    b = pa_proplist_new();
    pa_assert_se(pa_proplist_sets(b, PA_PROP_MEDIA_TITLE, "Goldbergvariationen") == 0);
    pa_assert_se(pa_proplist_set(b, PA_PROP_MEDIA_ICON, "\0\1\2\3\4\5\6\7", 8) == 0);

    pa_proplist_update(a, PA_UPDATE_MERGE, b);

    pa_assert_se(!pa_proplist_gets(a, PA_PROP_MEDIA_ICON));

    pa_log_debug("%s", pa_strnull(pa_proplist_gets(a, PA_PROP_MEDIA_TITLE)));
    pa_assert_se(pa_proplist_unset(b, PA_PROP_MEDIA_TITLE) == 0);

    s = pa_proplist_to_string(a);
    t = pa_proplist_to_string(b);
    pa_log_debug("---\n%s---\n%s", s, t);

    c = pa_proplist_from_string(s);
    u = pa_proplist_to_string(c);
    pa_assert_se(pa_streq(s, u));

    pa_xfree(s);
    pa_xfree(t);
    pa_xfree(u);

    pa_proplist_free(a);
    pa_proplist_free(b);
    pa_proplist_free(c);

    text = "  eins = zwei drei = \"\\\"vier\\\"\" fuenf=sechs sieben ='\\a\\c\\h\\t\\'\\\"' neun= hex:0123456789abCDef ";

    pa_log_debug("%s", text);
    d = pa_proplist_from_string(text);
    v = pa_proplist_to_string(d);
    pa_proplist_free(d);
    pa_log_debug("%s", v);
    d = pa_proplist_from_string(v);
    pa_xfree(v);
    v = pa_proplist_to_string(d);
    pa_proplist_free(d);
    pa_log_debug("%s", v);
    pa_xfree(v);

    pa_assert_se(ma = pa_modargs_new("foo='foobar=waldo foo2=\"lj\\\"dhflh\" foo3=\"kjlskj\\'\"'", x));
    pa_assert_se(a = pa_proplist_new());

    pa_assert_se(pa_modargs_get_proplist(ma, "foo", a, PA_UPDATE_REPLACE) >= 0);

    pa_log_debug("%s", v = pa_proplist_to_string(a));
    pa_xfree(v);

    pa_proplist_free(a);
    pa_modargs_free(ma);

    return 0;
}
