#ifndef footagstructhfoo
#define footagstructhfoo

#include <inttypes.h>
#include <sys/types.h>

#include "sample.h"

struct pa_tagstruct;

struct pa_tagstruct *pa_tagstruct_new(const uint8_t* data, size_t length);
void pa_tagstruct_free(struct pa_tagstruct*t);
uint8_t* pa_tagstruct_free_data(struct pa_tagstruct*t, size_t *l);

void pa_tagstruct_puts(struct pa_tagstruct*t, const char *s);
void pa_tagstruct_putu32(struct pa_tagstruct*t, uint32_t i);
void pa_tagstruct_putu8(struct pa_tagstruct*t, uint8_t c);
void pa_tagstruct_put_sample_spec(struct pa_tagstruct *t, const struct pa_sample_spec *ss);

int pa_tagstruct_gets(struct pa_tagstruct*t, const char **s);
int pa_tagstruct_getu32(struct pa_tagstruct*t, uint32_t *i);
int pa_tagstruct_getu8(struct pa_tagstruct*t, uint8_t *c);
int pa_tagstruct_get_sample_spec(struct pa_tagstruct *t, struct pa_sample_spec *ss);

int pa_tagstruct_eof(struct pa_tagstruct*t);
const uint8_t* pa_tagstruct_data(struct pa_tagstruct*t, size_t *l);



#endif
