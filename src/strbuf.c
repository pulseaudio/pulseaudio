#include <sys/types.h>
#include <stdlib.h>
#include <assert.h>
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

struct chunk {
    struct chunk *next;
    size_t length;
    char text[];
};

struct strbuf {
    size_t length;
    struct chunk *head, *tail;
};

struct strbuf *strbuf_new(void) {
    struct strbuf *sb = malloc(sizeof(struct strbuf));
    assert(sb);
    sb->length = 0;
    sb->head = sb->tail = NULL;
    return sb;
}

void strbuf_free(struct strbuf *sb) {
    assert(sb);
    while (sb->head) {
        struct chunk *c = sb->head;
        sb->head = sb->head->next;
        free(c);
    }

    free(sb);
}

char *strbuf_tostring(struct strbuf *sb) {
    char *t, *e;
    struct chunk *c;
    assert(sb);

    t = malloc(sb->length+1);
    assert(t);

    e = t;
    for (c = sb->head; c; c = c->next) {
        memcpy(e, c->text, c->length);
        e += c->length;
    }

    *e = 0;
    
    return t;
}

char *strbuf_tostring_free(struct strbuf *sb) {
    char *t;
    assert(sb);
    t = strbuf_tostring(sb);
    strbuf_free(sb);
    return t;
}

void strbuf_puts(struct strbuf *sb, const char *t) {
    struct chunk *c;
    size_t l;
    assert(sb && t);

    l = strlen(t);
    c = malloc(sizeof(struct chunk)+l);
    assert(c);

    c->next = NULL;
    c->length = l;
    memcpy(c->text, t, l);

    if (sb->tail) {
        assert(sb->head);
        sb->tail->next = c;
    } else {
        assert(!sb->head);
        sb->head = c;
    }

    sb->tail = c;
    sb->length += l;
}

int strbuf_printf(struct strbuf *sb, const char *format, ...) {
    int r, size = 100;
    struct chunk *c = NULL;

    assert(sb);
    
    for(;;) {
        va_list ap;

        c = realloc(c, sizeof(struct chunk)+size);
        assert(c);

        va_start(ap, format);
        r = vsnprintf(c->text, size, format, ap);
        va_end(ap);
        
        if (r > -1 && r < size) {
            c->length = r;
            c->next = NULL;
            
            if (sb->tail) {
                assert(sb->head);
                sb->tail->next = c;
            } else {
                assert(!sb->head);
                sb->head = c;
            }
            
            sb->tail = c;
            sb->length += r;
            
            return r;
        }

        if (r > -1)    /* glibc 2.1 */
            size = r+1; 
        else           /* glibc 2.0 */
            size *= 2;
    }
}
