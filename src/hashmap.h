#ifndef foohashmaphfoo
#define foohashmaphfoo

struct pa_hashmap;

struct pa_hashmap *pa_hashmap_new(unsigned (*hash_func) (const void *p), int (*compare_func) (const void*a, const void*b));
void pa_hashmap_free(struct pa_hashmap*, void (*free_func)(void *p, void *userdata), void *userdata);

int pa_hashmap_put(struct pa_hashmap *h, const void *key, void *value);
void* pa_hashmap_get(struct pa_hashmap *h, const void *key);

int pa_hashmap_remove(struct pa_hashmap *h, const void *key);

unsigned pa_hashmap_ncontents(struct pa_hashmap *h);

#endif
