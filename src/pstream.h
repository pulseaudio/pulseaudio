#ifndef foopstreamhfoo
#define foopstreamhfoo

#include <inttypes.h>

#include "packet.h"
#include "memblock.h"
#include "iochannel.h"
#include "mainloop-api.h"
#include "memchunk.h"

/* It is safe to destroy the calling pstream object from all callbacks */

struct pa_pstream;

struct pa_pstream* pa_pstream_new(struct pa_mainloop_api *m, struct pa_iochannel *io);
void pa_pstream_free(struct pa_pstream*p);

void pa_pstream_send_packet(struct pa_pstream*p, struct pa_packet *packet);
void pa_pstream_send_memblock(struct pa_pstream*p, uint32_t channel, int32_t delta, struct pa_memchunk *chunk);

void pa_pstream_set_recieve_packet_callback(struct pa_pstream *p, void (*callback) (struct pa_pstream *p, struct pa_packet *packet, void *userdata), void *userdata);
void pa_pstream_set_recieve_memblock_callback(struct pa_pstream *p, void (*callback) (struct pa_pstream *p, uint32_t channel, int32_t delta, struct pa_memchunk *chunk, void *userdata), void *userdata);
void pa_pstream_set_drain_callback(struct pa_pstream *p, void (*cb)(struct pa_pstream *p, void *userdata), void *userdata);

void pa_pstream_set_die_callback(struct pa_pstream *p, void (*callback)(struct pa_pstream *p, void *userdata), void *userdata);

int pa_pstream_is_pending(struct pa_pstream *p);

#endif
