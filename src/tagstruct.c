#include <stdlib.h>
#include <string.h>
#include <netinet/in.h>
#include <assert.h>

#include "tagstruct.h"

enum tags {
    TAG_STRING = 't',
    TAG_U32 = 'L',
    TAG_S32 = 'l',
    TAG_U16 = 'S',
    TAG_S16 = 's',
    TAG_U8 = 'B',
    TAG_S8 = 'b',
    TAG_SAMPLE_SPEC = 'a'
};

struct tagstruct {
    uint8_t *data;
    size_t length, allocated;
    size_t rindex;

    int dynamic;
};

struct tagstruct *tagstruct_new(const uint8_t* data, size_t length) {
    struct tagstruct*t;

    assert(!data || (data && length));
    
    t = malloc(sizeof(struct tagstruct));
    assert(t);
    t->data = (uint8_t*) data;
    t->allocated = t->length = data ? length : 0;
    t->rindex = 0;
    t->dynamic = !!data;
    return t;
}
    
void tagstruct_free(struct tagstruct*t) {
    assert(t);
    if (t->dynamic)
        free(t->data);
    free(t);
}

uint8_t* tagstruct_free_data(struct tagstruct*t, size_t *l) {
    uint8_t *p;
    assert(t && t->dynamic && l);
    p = t->data;
    *l = t->length;
    free(t);
    return p;
}

static void extend(struct tagstruct*t, size_t l) {
    assert(t && t->dynamic);

    if (t->allocated <= l)
        return;

    t->data = realloc(t->data, t->allocated = l+100);
    assert(t->data);
}

void tagstruct_puts(struct tagstruct*t, const char *s) {
    size_t l;
    assert(t && s);
    l = strlen(s)+2;
    extend(t, l);
    t->data[t->length] = TAG_STRING;
    strcpy(t->data+t->length+1, s);
    t->length += l;
}

void tagstruct_putu32(struct tagstruct*t, uint32_t i) {
    assert(t && i);
    extend(t, 5);
    t->data[t->length] = TAG_U32;
    *((uint32_t*) (t->data+t->length+1)) = htonl(i);
    t->length += 5;
}

void tagstruct_putu8(struct tagstruct*t, uint8_t c) {
    assert(t && c);
    extend(t, 2);
    t->data[t->length] = TAG_U8;
    *(t->data+t->length+1) = c;
    t->length += 2;
}

void tagstruct_put_sample_spec(struct tagstruct *t, const struct pa_sample_spec *ss) {
    assert(t && ss);
    extend(t, 7);
    t->data[t->length] = TAG_SAMPLE_SPEC;
    t->data[t->length+1] = (uint8_t) ss->format;
    t->data[t->length+2] = ss->channels;
    *(uint32_t*) (t->data+t->length+3) = htonl(ss->rate);
    t->length += 7;
}

int tagstruct_gets(struct tagstruct*t, const char **s) {
    int error = 0;
    size_t n;
    char *c;
    assert(t && s);

    if (t->rindex+2 > t->length)
        return -1;
    
    if (t->data[t->rindex] != TAG_STRING)
        return -1;

    error = 1;
    for (n = 0, c = (char*) (t->data+t->rindex+1); n < t->length-t->rindex-1; c++)
        if (!*c) {
            error = 0;
            break;
        }

    if (error)
        return -1;

    *s = (char*) (t->data+t->rindex+1);

    t->rindex += n+1;
    return 0;
}

int tagstruct_getu32(struct tagstruct*t, uint32_t *i) {
    assert(t && i);

    if (t->rindex+5 > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_U32)
        return -1;
    
    *i = ntohl(*((uint32_t*) (t->data+t->rindex+1)));
    t->rindex += 5;
    return 0;
}

int tagstruct_getu8(struct tagstruct*t, uint8_t *c) {
    assert(t && c);

    if (t->rindex+2 > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_U8)
        return -1;

    *c = t->data[t->rindex+1];
    t->rindex +=2;
    return 0;
}

int tagstruct_get_sample_spec(struct tagstruct *t, struct pa_sample_spec *ss) {
    assert(t && ss);

    if (t->rindex+7 > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_SAMPLE_SPEC)
        return -1;
    
    ss->format = t->data[t->rindex+1];
    ss->channels = t->data[t->rindex+2];
    ss->rate = ntohl(*(uint32_t*) (t->data+t->rindex+3));
    
    t->rindex += 7;
    return 0;
}


int tagstruct_eof(struct tagstruct*t) {
    assert(t);
    return t->rindex >= t->length;
}

const uint8_t* tagstruct_data(struct tagstruct*t, size_t *l) {
    assert(t && t->dynamic && l);
    *l = t->length;
    return t->data;
}

