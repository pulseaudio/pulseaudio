#ifndef footokenizerhfoo
#define footokenizerhfoo

struct tokenizer;

struct tokenizer* tokenizer_new(const char *s, unsigned args);
void tokenizer_free(struct tokenizer *t);

const char *tokenizer_get(struct tokenizer *t, unsigned i);

#endif
