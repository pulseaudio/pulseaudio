#ifndef foopackethfoo
#define foopackethfoo

#include <sys/types.h>
#include <stdint.h>

struct pa_packet {
    enum { PA_PACKET_APPENDED, PA_PACKET_DYNAMIC } type;
    unsigned ref;
    size_t length;
    uint8_t *data;
};

struct pa_packet* pa_packet_new(size_t length);
struct pa_packet* pa_packet_new_dynamic(uint8_t* data, size_t length);

struct pa_packet* pa_packet_ref(struct pa_packet *p);
void pa_packet_unref(struct pa_packet *p);

#endif
