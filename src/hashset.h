#ifndef foohashsethfoo
#define foohashsethfoo

struct pa_hashset;

struct pa_hashset *pa_hashset_new(unsigned (*hash_func) (const void *p), int (*compare_func) (const void*a, const void*b));
void pa_hashset_free(struct pa_hashset*, void (*free_func)(void *p, void *userdata), void *userdata);

int pa_hashset_put(struct pa_hashset *h, const void *key, void *value);
void* pa_hashset_get(struct pa_hashset *h, const void *key);

int pa_hashset_remove(struct pa_hashset *h, const void *key);

unsigned pa_hashset_ncontents(struct pa_hashset *h);

#endif
