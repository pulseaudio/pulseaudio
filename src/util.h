#ifndef fooutilhfoo
#define fooutilhfoo

#include <sys/types.h>

void pa_make_nonblock_fd(int fd);

int pa_make_secure_dir(const char* dir);

ssize_t pa_loop_read(int fd, void*data, size_t size);
ssize_t pa_loop_write(int fd, const void*data, size_t size);

void pa_check_for_sigpipe(void);

char *pa_sprintf_malloc(const char *format, ...) __attribute__ ((format (printf, 1, 2)));

#endif
