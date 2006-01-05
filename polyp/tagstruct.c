/* $Id$ */

/***
  This file is part of polypaudio.
 
  polypaudio is free software; you can redistribute it and/or modify
  it under the terms of the GNU Lesser General Public License as
  published by the Free Software Foundation; either version 2.1 of the
  License, or (at your option) any later version.
 
  polypaudio is distributed in the hope that it will be useful, but
  WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
  Lesser General Public License for more details.
 
  You should have received a copy of the GNU Lesser General Public
  License along with polypaudio; if not, write to the Free Software
  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307
  USA.
***/

#ifdef HAVE_CONFIG_H
#include <config.h>
#endif

#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <assert.h>

#ifdef HAVE_NETINET_IN_H
#include <netinet/in.h>
#endif

#ifdef HAVE_WINSOCK2_H
#include <winsock2.h>
#endif

#include "tagstruct.h"
#include "xmalloc.h"

enum tags {
    TAG_STRING = 't',
    TAG_NULL_STRING = 'N',
    TAG_U32 = 'L',
    TAG_S32 = 'l',
    TAG_U16 = 'S',
    TAG_S16 = 's',
    TAG_U8 = 'B',
    TAG_S8 = 'b',
    TAG_U64 = 'R',
    TAG_S64 = 'r',
    TAG_SAMPLE_SPEC = 'a',
    TAG_ARBITRARY = 'x',
    TAG_BOOLEAN_TRUE = '1',
    TAG_BOOLEAN_FALSE = '0',
    TAG_TIMEVAL = 'T',
    TAG_USEC = 'U'  /* 64bit unsigned */
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
    
    t = pa_xmalloc(sizeof(struct pa_tagstruct));
    t->data = (uint8_t*) data;
    t->allocated = t->length = data ? length : 0;
    t->rindex = 0;
    t->dynamic = !data;
    return t;
}
    
void pa_tagstruct_free(struct pa_tagstruct*t) {
    assert(t);
    if (t->dynamic)
        pa_xfree(t->data);
    pa_xfree(t);
}

uint8_t* pa_tagstruct_free_data(struct pa_tagstruct*t, size_t *l) {
    uint8_t *p;
    assert(t && t->dynamic && l);
    p = t->data;
    *l = t->length;
    pa_xfree(t);
    return p;
}

static void extend(struct pa_tagstruct*t, size_t l) {
    assert(t && t->dynamic);

    if (t->length+l <= t->allocated)
        return;

    t->data = pa_xrealloc(t->data, t->allocated = t->length+l+100);
}

void pa_tagstruct_puts(struct pa_tagstruct*t, const char *s) {
    size_t l;
    assert(t);
    if (s) {
        l = strlen(s)+2;
        extend(t, l);
        t->data[t->length] = TAG_STRING;
        strcpy((char*) (t->data+t->length+1), s);
        t->length += l;
    } else {
        extend(t, 1);
        t->data[t->length] = TAG_NULL_STRING;
        t->length += 1;
    }
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

void pa_tagstruct_put_boolean(struct pa_tagstruct*t, int b) {
    assert(t);
    extend(t, 1);
    t->data[t->length] = b ? TAG_BOOLEAN_TRUE : TAG_BOOLEAN_FALSE;
    t->length += 1;
}

void pa_tagstruct_put_timeval(struct pa_tagstruct*t, const struct timeval *tv) {
    assert(t);
    extend(t, 9);
    t->data[t->length] = TAG_TIMEVAL;
    *((uint32_t*) (t->data+t->length+1)) = htonl(tv->tv_sec);
    *((uint32_t*) (t->data+t->length+5)) = htonl(tv->tv_usec);
    t->length += 9;
}

void pa_tagstruct_put_usec(struct pa_tagstruct*t, pa_usec_t u) {
    assert(t);
    extend(t, 9);
    t->data[t->length] = TAG_USEC;
    *((uint32_t*) (t->data+t->length+1)) = htonl((uint32_t) (u >> 32));
    *((uint32_t*) (t->data+t->length+5)) = htonl((uint32_t) u);
    t->length += 9;
}

void pa_tagstruct_putu64(struct pa_tagstruct*t, uint64_t u) {
    assert(t);
    extend(t, 9);
    t->data[t->length] = TAG_U64;
    *((uint32_t*) (t->data+t->length+1)) = htonl((uint32_t) (u >> 32));
    *((uint32_t*) (t->data+t->length+5)) = htonl((uint32_t) u);
    t->length += 9;
}

int pa_tagstruct_gets(struct pa_tagstruct*t, const char **s) {
    int error = 0;
    size_t n;
    char *c;
    assert(t && s);

    if (t->rindex+1 > t->length)
        return -1;

    if (t->data[t->rindex] == TAG_NULL_STRING) {
        t->rindex++;
        *s = NULL;
        return 0;
    }
    
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

int pa_tagstruct_get_boolean(struct pa_tagstruct*t, int *b) {
    assert(t && b);

    if (t->rindex+1 > t->length)
        return -1;

    if (t->data[t->rindex] == TAG_BOOLEAN_TRUE)
        *b = 1;
    else if (t->data[t->rindex] == TAG_BOOLEAN_FALSE)
        *b = 0;
    else
        return -1;
    
    t->rindex +=1;
    return 0;
}

int pa_tagstruct_get_timeval(struct pa_tagstruct*t, struct timeval *tv) {

    if (t->rindex+9 > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_TIMEVAL)
        return -1;
    
    tv->tv_sec = ntohl(*((uint32_t*) (t->data+t->rindex+1)));
    tv->tv_usec = ntohl(*((uint32_t*) (t->data+t->rindex+5)));
    t->rindex += 9;
    return 0;
    
}

int pa_tagstruct_get_usec(struct pa_tagstruct*t, pa_usec_t *u) {
    assert(t && u);

    if (t->rindex+9 > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_USEC)
        return -1;

    *u = (pa_usec_t) ntohl(*((uint32_t*) (t->data+t->rindex+1))) << 32;
    *u |= (pa_usec_t) ntohl(*((uint32_t*) (t->data+t->rindex+5)));
    t->rindex +=9;
    return 0;
}

int pa_tagstruct_getu64(struct pa_tagstruct*t, uint64_t *u) {
    assert(t && u);

    if (t->rindex+9 > t->length)
        return -1;

    if (t->data[t->rindex] != TAG_U64)
        return -1;

    *u = (uint64_t) ntohl(*((uint32_t*) (t->data+t->rindex+1))) << 32;
    *u |= (uint64_t) ntohl(*((uint32_t*) (t->data+t->rindex+5)));
    t->rindex +=9;
    return 0;
}
