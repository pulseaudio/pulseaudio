#ifndef fooauthkeyhfoo
#define fooauthkeyhfoo

#include <sys/types.h>

int pa_authkey_load(const char *path, void *data, size_t len);
int pa_authkey_load_from_home(const char *fn, void *data, size_t length);
int pa_authkey_load_auto(const char *fn, void *data, size_t length);

#endif
