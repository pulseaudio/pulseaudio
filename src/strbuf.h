#ifndef foostrbufhfoo
#define foostrbufhfoo

struct strbuf;

struct strbuf *strbuf_new(void);
void strbuf_free(struct strbuf *sb);
char *strbuf_tostring(struct strbuf *sb);
char *strbuf_tostring_free(struct strbuf *sb);

int strbuf_printf(struct strbuf *sb, const char *format, ...);
void strbuf_puts(struct strbuf *sb, const char *t);

#endif
