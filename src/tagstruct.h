#ifndef footagstructhfoo
#define footagstructhfoo

#include <inttypes.h>
#include <sys/types.h>

#include "sample.h"

struct tagstruct;

struct tagstruct *tagstruct_new(const uint8_t* data, size_t length);
void tagstruct_free(struct tagstruct*t);
uint8_t* tagstruct_free_data(struct tagstruct*t, size_t *l);

void tagstruct_puts(struct tagstruct*t, const char *s);
void tagstruct_putu32(struct tagstruct*t, uint32_t i);
void tagstruct_putu8(struct tagstruct*t, uint8_t c);
void tagstruct_put_sample_spec(struct tagstruct *t, struct sample_spec *ss);

int tagstruct_gets(struct tagstruct*t, const char **s);
int tagstruct_getu32(struct tagstruct*t, uint32_t *i);
int tagstruct_getu8(struct tagstruct*t, uint8_t *c);
int tagstruct_get_sample_spec(struct tagstruct *t, struct sample_spec *ss);

int tagstruct_eof(struct tagstruct*t);
const uint8_t* tagstruct_data(struct tagstruct*t, size_t *l);



#endif
