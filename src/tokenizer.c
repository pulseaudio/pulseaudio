#include <string.h>
#include <assert.h>
#include <stdlib.h>

#include "tokenizer.h"
#include "dynarray.h"

struct tokenizer {
    struct dynarray *dynarray;
};

static void token_free(void *p, void *userdata) {
    free(p);
}

static void parse(struct dynarray*a, const char *s, unsigned args) {
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
        dynarray_append(a, n);
        p += l;
        p += strspn(p, delimiter);
        args--;
    }

    if (args && *p) {
        char *n = strdup(p);
        assert(n);
        dynarray_append(a, n);
    }
}

struct tokenizer* tokenizer_new(const char *s, unsigned args) {
    struct tokenizer *t;
    
    t = malloc(sizeof(struct tokenizer));
    assert(t);
    t->dynarray = dynarray_new();
    assert(t->dynarray);

    parse(t->dynarray, s, args);
    return t;
}

void tokenizer_free(struct tokenizer *t) {
    assert(t);
    dynarray_free(t->dynarray, token_free, NULL);
    free(t);
}

const char *tokenizer_get(struct tokenizer *t, unsigned i) {
    assert(t);
    return dynarray_get(t->dynarray, i);
}
