#ifndef foopstreamhfoo
#define foopstreamhfoo

#include <inttypes.h>

#include "packet.h"
#include "memblock.h"
#include "iochannel.h"

struct pstream;

struct pstream* pstream_new(struct mainloop *m, struct iochannel *io);
void pstream_free(struct pstream*p);

void pstream_set_send_callback(struct pstream*p, void (*callback) (struct pstream *p, void *userdata), void *userdata);
void pstream_send_packet(struct pstream*p, struct packet *packet);
void pstream_send_memblock(struct pstream*p, uint32_t channel, int32_t delta, struct memchunk *chunk);

void pstream_set_recieve_packet_callback(struct pstream *p, void (*callback) (struct pstream *p, struct packet *packet, void *userdata), void *userdata);
void pstream_set_recieve_memblock_callback(struct pstream *p, void (*callback) (struct pstream *p, uint32_t channel, int32_t delta, struct memchunk *chunk, void *userdata), void *userdata);

#endif
