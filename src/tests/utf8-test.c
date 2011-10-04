#include <stdio.h>
#include <assert.h>

#include <pulse/utf8.h>
#include <pulse/xmalloc.h>

int main(int argc, char *argv[]) {
    char *c;

    assert(pa_utf8_valid("hallo"));
    assert(pa_utf8_valid("hallo\n"));
    assert(!pa_utf8_valid("hüpfburg\n"));
    assert(pa_utf8_valid("hallo\n"));
    assert(pa_utf8_valid("hÃ¼pfburg\n"));

    fprintf(stderr, "LATIN1: %s\n", c = pa_utf8_filter("hüpfburg"));
    pa_xfree(c);
    fprintf(stderr, "UTF8: %sx\n", c = pa_utf8_filter("hÃ¼pfburg"));
    pa_xfree(c);
    fprintf(stderr, "LATIN1: %sx\n", c = pa_utf8_filter("üxknärzmörzeltörszß³§dsjkfh"));
    pa_xfree(c);

    return 0;
}
