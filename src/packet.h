#ifndef foopackethfoo
#define foopackethfoo

#include <sys/types.h>
#include <stdint.h>

struct packet {
    unsigned ref;
    size_t length;
    uint8_t data[];
};

struct packet* packet_new(uint32_t length);

struct packet* packet_ref(struct packet *p);
void packet_unref(struct packet *p);

#endif
