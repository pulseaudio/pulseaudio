#ifndef footokenizerhfoo
#define footokenizerhfoo

struct pa_tokenizer;

struct pa_tokenizer* pa_tokenizer_new(const char *s, unsigned args);
void pa_tokenizer_free(struct pa_tokenizer *t);

const char *pa_tokenizer_get(struct pa_tokenizer *t, unsigned i);

#endif
