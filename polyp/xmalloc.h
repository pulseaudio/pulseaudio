#ifndef foomemoryhfoo
#define foomemoryhfoo

#include <sys/types.h>

void* pa_xmalloc(size_t l);
void *pa_xmalloc0(size_t l);
void *pa_xrealloc(void *ptr, size_t size);
#define pa_xfree free

char *pa_xstrdup(const char *s);
char *pa_xstrndup(const char *s, size_t l);

#endif
