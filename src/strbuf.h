#ifndef foostrbufhfoo
#define foostrbufhfoo

struct pa_strbuf;

struct pa_strbuf *pa_strbuf_new(void);
void pa_strbuf_free(struct pa_strbuf *sb);
char *pa_strbuf_tostring(struct pa_strbuf *sb);
char *pa_strbuf_tostring_free(struct pa_strbuf *sb);

int pa_strbuf_printf(struct pa_strbuf *sb, const char *format, ...);
void pa_strbuf_puts(struct pa_strbuf *sb, const char *t);

#endif
