#include <assert.h>
#include <stdlib.h>

#include "packet.h"

struct packet* packet_new(uint32_t length) {
    struct packet *p;
    assert(length);
    p = malloc(sizeof(struct packet)+length);
    assert(p);

    p->ref = 1;
    p->length = length;
    return p;
}

struct packet* packet_ref(struct packet *p) {
    assert(p && p->ref >= 1);
    p->ref++;
    return p;
}

void packet_unref(struct packet *p) {
    assert(p && p->ref >= 1);
    p->ref--;

    if (p->ref == 0)
        free(p);
}
