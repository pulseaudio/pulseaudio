#include <assert.h>

#include "native-common.h"
#include "pstream-util.h"

void pa_pstream_send_tagstruct(struct pa_pstream *p, struct pa_tagstruct *t) {
    size_t length;
    uint8_t *data;
    struct pa_packet *packet;
    assert(p && t);

    data = pa_tagstruct_free_data(t, &length);
    assert(data && length);
    packet = pa_packet_new_dynamic(data, length);
    assert(packet);
    pa_pstream_send_packet(p, packet);
    pa_packet_unref(packet);
}

void pa_pstream_send_error(struct pa_pstream *p, uint32_t tag, uint32_t error) {
    struct pa_tagstruct *t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_ERROR);
    pa_tagstruct_putu32(t, tag);
    pa_tagstruct_putu32(t, error);
    pa_pstream_send_tagstruct(p, t);
}

void pa_pstream_send_simple_ack(struct pa_pstream *p, uint32_t tag) {
    struct pa_tagstruct *t = pa_tagstruct_new(NULL, 0);
    assert(t);
    pa_tagstruct_putu32(t, PA_COMMAND_REPLY);
    pa_tagstruct_putu32(t, tag);
    pa_pstream_send_tagstruct(p, t);
}
