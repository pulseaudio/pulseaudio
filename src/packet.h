#ifndef foopackethfoo
#define foopackethfoo

#include <sys/types.h>
#include <stdint.h>

struct packet {
    enum { PACKET_APPENDED, PACKET_DYNAMIC } type;
    unsigned ref;
    size_t length;
    uint8_t *data;
};

struct packet* packet_new(size_t length);
struct packet* packet_new_dynamic(uint8_t* data, size_t length);

struct packet* packet_ref(struct packet *p);
void packet_unref(struct packet *p);

#endif
