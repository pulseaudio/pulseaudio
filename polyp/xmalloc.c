#include <stdlib.h>
#include <signal.h>
#include <assert.h>

#include "memory.h"
#include "util.h"

#define MAX_ALLOC_SIZE (1024*1024*20)

#undef malloc
#undef free
#undef realloc
#undef strndup
#undef strdup

static void oom(void) {
    static const char e[] = "Not enough memory\n";
    pa_loop_write(2, e, sizeof(e)-1);
    raise(SIGQUIT);
    exit(1);
}

void* pa_xmalloc(size_t size) {
    void *p;
    assert(size > 0);
    assert(size < MAX_ALLOC_SIZE);
    
    if (!(p = malloc(size)))
        oom();
        
    return p;
}

void* pa_xmalloc0(size_t size) {
    void *p;
    assert(size > 0);
    assert(size < MAX_ALLOC_SIZE);
    
    if (!(p = calloc(1, size)))
        oom();
        
    return p;
}
    
void *pa_xrealloc(void *ptr, size_t size) {
    void *p;
    assert(size > 0);
    assert(size < MAX_ALLOC_SIZE);
    
    if (!(p = realloc(ptr, size)))
        oom();
    return p;
}

void* pa_xmemdup(const void *p, size_t l) {
    if (!p)
        return NULL;
    else {
        char *r = pa_xmalloc(l);
        memcpy(r, p, l);
        return r;
    }
}

char *pa_xstrdup(const char *s) {
    if (!s)
        return NULL;

    return pa_xmemdup(s, strlen(s)+1);
}

char *pa_xstrndup(const char *s, size_t l) {
    if (!s)
        return NULL;
    else {
        char *r;
        size_t t = strlen(s);

        if (t > l)
            t = l;
        
        r = pa_xmemdup(s, t+1);
        r[t] = 0;
        return r;
    }
}

