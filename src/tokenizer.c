#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "tokenizer.h"
#include "dynarray.h"

struct pa_tokenizer {
    struct pa_dynarray *dynarray;
};

static void token_free(void *p, void *userdata) {
    free(p);
}

static void parse(struct pa_dynarray*a, const char *s, unsigned args) {
    int infty = 0;
    const char delimiter[] = " \t\n\r";
    const char *p;
    assert(a && s);

    if (args == 0)
        infty = 1;

    p = s+strspn(s, delimiter);
    while (*p && (infty || args >= 2)) {
        size_t l = strcspn(p, delimiter);
        char *n = strndup(p, l);
        assert(n);
        pa_dynarray_append(a, n);
        p += l;
        p += strspn(p, delimiter);
        args--;
    }

    if (args && *p) {
        char *n = strdup(p);
        assert(n);
        pa_dynarray_append(a, n);
    }
}

struct pa_tokenizer* pa_tokenizer_new(const char *s, unsigned args) {
    struct pa_tokenizer *t;
    
    t = malloc(sizeof(struct pa_tokenizer));
    assert(t);
    t->dynarray = pa_dynarray_new();
    assert(t->dynarray);

    parse(t->dynarray, s, args);
    return t;
}

void pa_tokenizer_free(struct pa_tokenizer *t) {
    assert(t);
    pa_dynarray_free(t->dynarray, token_free, NULL);
    free(t);
}

const char *pa_tokenizer_get(struct pa_tokenizer *t, unsigned i) {
    assert(t);
    return pa_dynarray_get(t->dynarray, i);
}
