#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

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
    TAG_SAMPLE_SPEC = 'a',
    TAG_ARBITRARY = 'x'
};

struct pa_tagstruct {
    uint8_t *data;
    size_t length, allocated;
    size_t rindex;

    int dynamic;
};

struct pa_tagstruct *pa_tagstruct_new(const uint8_t* data, size_t length) {
    struct pa_tagstruct*t;

    assert(!data || (data && length));
    
    t = malloc(sizeof(struct pa_tagstruct));
    assert(t);
    t->data = (uint8_t*) data;
    t->allocated = t->length = data ? length : 0;
    t->rindex = 0;
    t->dynamic = !data;
    return t;
}
    
void pa_tagstruct_free(struct pa_tagstruct*t) {
    assert(t);
    if (t->dynamic)
        free(t->data);
    free(t);
}

uint8_t* pa_tagstruct_free_data(struct pa_tagstruct*t, size_t *l) {
    uint8_t *p;
    assert(t && t->dynamic && l);
    p = t->data;
    *l = t->length;
    free(t);
    return p;
}

static void extend(struct pa_tagstruct*t, size_t l) {
    assert(t && t->dynamic);

    if (l <= t->allocated)
        return;

    t->data = realloc(t->data, t->allocated = l+100);
    assert(t->data);
}

void pa_tagstruct_puts(struct pa_tagstruct*t, const char *s) {
    size_t l;
    assert(t && s);
    l = strlen(s)+2;
    extend(t, l);
    t->data[t->length] = TAG_STRING;
    strcpy(t->data+t->length+1, s);
    t->length += l;
}

void pa_tagstruct_putu32(struct pa_tagstruct*t, uint32_t i) {
    assert(t);
    extend(t, 5);
    t->data[t->length] = TAG_U32;
    *((uint32_t*) (t->data+t->length+1)) = htonl(i);
    t->length += 5;
}

void pa_tagstruct_putu8(struct pa_tagstruct*t, uint8_t c) {
    assert(t);
    extend(t, 2);
    t->data[t->length] = TAG_U8;
    *(t->data+t->length+1) = c;
    t->length += 2;
}

void pa_tagstruct_put_sample_spec(struct pa_tagstruct *t, const struct pa_sample_spec *ss) {
    assert(t && ss);
    extend(t, 7);
    t->data[t->length] = TAG_SAMPLE_SPEC;
    t->data[t->length+1] = (uint8_t) ss->format;
    t->data[t->length+2] = ss->channels;
    *(uint32_t*) (t->data+t->length+3) = htonl(ss->rate);
    t->length += 7;
}


void pa_tagstruct_put_arbitrary(struct pa_tagstruct *t, const void *p, size_t length) {
    assert(t && p);

    extend(t, 5+length);
    t->data[t->length] = TAG_ARBITRARY;
    *((uint32_t*) (t->data+t->length+1)) = htonl(length);
    if (length)
        memcpy(t->data+t->length+5, p, length);
    t->length += 5+length;
}

int pa_tagstruct_gets(struct pa_tagstruct*t, const char **s) {
    int error = 0;
    size_t n;
    char *c;
    assert(t && s);

    if (t->rindex+2 > t->length)
        return -1;
    
    if (t->data[t->rindex] != TAG_STRING)
        return -1;

    error = 1;
    for (n = 0, c = (char*) (t->data+t->rindex+1); t->rindex+1+n < t->length; n++, c++)
        if (!*c) {
            error = 0;
            break;
        }

    if (error)
        return -1;

    *s = (char*) (t->data+t->rindex+1);

    t->rindex += n+2;
    return 0;
}

int pa_tagstruct_getu32(struct pa_tagstruct*t, uint32_t *i) {
    assert(t && i);

    if (t->rindex+5 > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_U32)
        return -1;
    
    *i = ntohl(*((uint32_t*) (t->data+t->rindex+1)));
    t->rindex += 5;
    return 0;
}

int pa_tagstruct_getu8(struct pa_tagstruct*t, uint8_t *c) {
    assert(t && c);

    if (t->rindex+2 > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_U8)
        return -1;

    *c = t->data[t->rindex+1];
    t->rindex +=2;
    return 0;
}

int pa_tagstruct_get_sample_spec(struct pa_tagstruct *t, struct pa_sample_spec *ss) {
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

int pa_tagstruct_get_arbitrary(struct pa_tagstruct *t, const void **p, size_t length) {
    assert(t && p);
    
    if (t->rindex+5+length > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_ARBITRARY)
        return -1;

    if (ntohl(*((uint32_t*) (t->data+t->rindex+1))) != length)
        return -1;

    *p = t->data+t->rindex+5;
    t->rindex += 5+length;
    return 0;
}

int pa_tagstruct_eof(struct pa_tagstruct*t) {
    assert(t);
    return t->rindex >= t->length;
}

const uint8_t* pa_tagstruct_data(struct pa_tagstruct*t, size_t *l) {
    assert(t && t->dynamic && l);
    *l = t->length;
    return t->data;
}

