#ifndef foopstreamutilhfoo
#define foopstreamutilhfoo

#include <inttypes.h>
#include "pstream.h"
#include "tagstruct.h"

/* The tagstruct is freed!*/
void pstream_send_tagstruct(struct pstream *p, struct tagstruct *t);

void pstream_send_error(struct pstream *p, uint32_t tag, uint32_t error);
void pstream_send_simple_ack(struct pstream *p, uint32_t tag);

#endif
