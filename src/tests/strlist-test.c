#include <stdio.h>

#include <pulse/xmalloc.h>
#include <pulse/gccmacro.h>

#include <pulsecore/strlist.h>

int main(int argc, char* argv[]) {
    char *t, *u;
    pa_strlist *l = NULL;

    l = pa_strlist_prepend(l, "e");
    l = pa_strlist_prepend(l, "d");
    l = pa_strlist_prepend(l, "c");
    l = pa_strlist_prepend(l, "b");
    l = pa_strlist_prepend(l, "a");

    t = pa_strlist_tostring(l);
    pa_strlist_free(l);

    fprintf(stderr, "1: %s\n", t);

    l = pa_strlist_parse(t);
    pa_xfree(t);

    t = pa_strlist_tostring(l);
    fprintf(stderr, "2: %s\n", t);
    pa_xfree(t);

    l = pa_strlist_pop(l, &u);
    fprintf(stderr, "3: %s\n", u);
    pa_xfree(u);

    l = pa_strlist_remove(l, "c");

    t = pa_strlist_tostring(l);
    fprintf(stderr, "4: %s\n", t);
    pa_xfree(t);

    pa_strlist_free(l);

    return 0;
}
