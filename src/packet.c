#include <assert.h>
#include <stdlib.h>

#include "packet.h"

struct pa_packet* pa_packet_new(size_t length) {
    struct pa_packet *p;
    assert(length);
    p = malloc(sizeof(struct pa_packet)+length);
    assert(p);

    p->ref = 1;
    p->length = length;
    p->data = (uint8_t*) (p+1);
    p->type = PA_PACKET_APPENDED;
    return p;
}

struct pa_packet* pa_packet_new_dynamic(uint8_t* data, size_t length) {
    struct pa_packet *p;
    assert(data && length);
    p = malloc(sizeof(struct pa_packet));
    assert(p);

    p->ref = 1;
    p->length = length;
    p->data = data;
    p->type = PA_PACKET_DYNAMIC;
    return p;
}

struct pa_packet* pa_packet_ref(struct pa_packet *p) {
    assert(p && p->ref >= 1);
    p->ref++;
    return p;
}

void pa_packet_unref(struct pa_packet *p) {
    assert(p && p->ref >= 1);
    p->ref--;

    if (p->ref == 0) {
        if (p->type == PA_PACKET_DYNAMIC)
            free(p->data);
        free(p);
    }
}
