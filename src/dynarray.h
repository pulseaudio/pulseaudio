#ifndef foodynarrayhfoo
#define foodynarrayhfoo

struct dynarray;

struct dynarray* dynarray_new(void);
void dynarray_free(struct dynarray* a, void (*func)(void *p, void *userdata), void *userdata);

void dynarray_put(struct dynarray*a, unsigned i, void *p);
unsigned dynarray_append(struct dynarray*a, void *p);

void *dynarray_get(struct dynarray*a, unsigned i);

unsigned dynarray_ncontents(struct dynarray*a);

#endif
