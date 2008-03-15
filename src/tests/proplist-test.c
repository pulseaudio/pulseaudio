/* $Id$ */

/***
  This file is part of PulseAudio.

  PulseAudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as published
  by the Free Software Foundation; either version 2 of the License,
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

int main(int argc, char*argv[]) {
    pa_proplist *a, *b;
    char *s, *t;

    a = pa_proplist_new();
    pa_assert_se(pa_proplist_puts(a, PA_PROP_MEDIA_TITLE, "Brandenburgische Konzerte") == 0);
    pa_assert_se(pa_proplist_puts(a, PA_PROP_MEDIA_ARTIST, "Johann Sebastian Bach") == 0);

    b = pa_proplist_new();
    pa_assert_se(pa_proplist_puts(b, PA_PROP_MEDIA_TITLE, "Goldbergvariationen") == 0);
    pa_assert_se(pa_proplist_put(b, PA_PROP_MEDIA_ICON, "\0\1\2\3\4\5\6\7", 8) == 0);

    pa_proplist_merge(a, b);

    pa_assert_se(!pa_proplist_gets(a, PA_PROP_MEDIA_ICON));

    printf("%s\n", pa_strnull(pa_proplist_gets(a, PA_PROP_MEDIA_TITLE)));
    pa_assert_se(pa_proplist_remove(b, PA_PROP_MEDIA_TITLE) == 0);

    s = pa_proplist_to_string(a);
    t = pa_proplist_to_string(b);
    printf("---\n%s---\n%s", s, t);
    pa_xfree(s);
    pa_xfree(t);

    pa_proplist_free(a);
    pa_proplist_free(b);

    return 0;
}
