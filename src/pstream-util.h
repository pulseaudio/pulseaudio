#ifndef foopstreamutilhfoo
#define foopstreamutilhfoo

#include <inttypes.h>
#include "pstream.h"
#include "tagstruct.h"

/* The tagstruct is freed!*/
void pa_pstream_send_tagstruct(struct pa_pstream *p, struct pa_tagstruct *t);

void pa_pstream_send_error(struct pa_pstream *p, uint32_t tag, uint32_t error);
void pa_pstream_send_simple_ack(struct pa_pstream *p, uint32_t tag);

#endif
