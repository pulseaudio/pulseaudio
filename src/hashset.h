#ifndef foohashsethfoo
#define foohashsethfoo

struct hashset;

struct hashset *hashset_new(unsigned (*hash_func) (const void *p), int (*compare_func) (const void*a, const void*b));
void hashset_free(struct hashset*, void (*free_func)(void *p, void *userdata), void *userdata);

int hashset_put(struct hashset *h, const void *key, void *value);
void* hashset_get(struct hashset *h, const void *key);

int hashset_remove(struct hashset *h, const void *key);

unsigned hashset_ncontents(struct hashset *h);

#endif
