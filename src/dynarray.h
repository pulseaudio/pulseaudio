#ifndef foodynarrayhfoo
#define foodynarrayhfoo

struct pa_dynarray;

struct pa_dynarray* pa_dynarray_new(void);
void pa_dynarray_free(struct pa_dynarray* a, void (*func)(void *p, void *userdata), void *userdata);

void pa_dynarray_put(struct pa_dynarray*a, unsigned i, void *p);
unsigned pa_dynarray_append(struct pa_dynarray*a, void *p);

void *pa_dynarray_get(struct pa_dynarray*a, unsigned i);

unsigned pa_dynarray_ncontents(struct pa_dynarray*a);

#endif
