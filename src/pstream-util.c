#include <assert.h>

#include "protocol-native-spec.h"
#include "pstream-util.h"

void pstream_send_tagstruct(struct pstream *p, struct tagstruct *t) {
    size_t length;
    uint8_t *data;
    struct packet *packet;
    assert(p && t);

    data = tagstruct_free_data(t, &length);
    assert(data && length);
    packet = packet_new_dynamic(data, length);
    assert(packet);
    pstream_send_packet(p, packet);
    packet_unref(packet);
}

void pstream_send_error(struct pstream *p, uint32_t tag, uint32_t error) {
    struct tagstruct *t = tagstruct_new(NULL, 0);
    assert(t);
    tagstruct_putu32(t, PA_COMMAND_ERROR);
    tagstruct_putu32(t, tag);
    tagstruct_putu32(t, error);
    pstream_send_tagstruct(p, t);
}

void pstream_send_simple_ack(struct pstream *p, uint32_t tag) {
    struct tagstruct *t = tagstruct_new(NULL, 0);
    assert(t);
    tagstruct_putu32(t, PA_COMMAND_REPLY);
    tagstruct_putu32(t, tag);
    pstream_send_tagstruct(p, t);
}
