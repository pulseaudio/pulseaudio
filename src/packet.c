#include <assert.h>
#include <stdlib.h>

#include "packet.h"

struct packet* packet_new(size_t length) {
    struct packet *p;
    assert(length);
    p = malloc(sizeof(struct packet)+length);
    assert(p);

    p->ref = 1;
    p->length = length;
    p->data = (uint8_t*) (p+1);
    p->type = PACKET_APPENDED;
    return p;
}

struct packet* packet_dynamic(uint8_t* data, size_t length) {
    struct packet *p;
    assert(data && length);
    p = malloc(sizeof(struct packet));
    assert(p);

    p->ref = 1;
    p->length = length;
    p->data = data;
    p->type = PACKET_DYNAMIC;
}

struct packet* packet_ref(struct packet *p) {
    assert(p && p->ref >= 1);
    p->ref++;
    return p;
}

void packet_unref(struct packet *p) {
    assert(p && p->ref >= 1);
    p->ref--;

    if (p->ref == 0) {
        if (p->type == PACKET_DYNAMIC)
            free(p->data);
        free(p);
    }
}
