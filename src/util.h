#ifndef fooutilhfoo
#define fooutilhfoo

#include <sys/types.h>

void pa_make_nonblock_fd(int fd);

void pa_peer_to_string(char *c, size_t l, int fd);

int pa_make_secure_dir(const char* dir);

int pa_socket_low_delay(int fd);
int pa_socket_tcp_low_delay(int fd);

int pa_socket_set_sndbuf(int fd, size_t l);
int pa_socket_set_rcvbuf(int fd, size_t l);

ssize_t pa_loop_read(int fd, void*data, size_t size);
ssize_t pa_loop_write(int fd, const void*data, size_t size);

int pa_unix_socket_is_stale(const char *fn);
int pa_unix_socket_remove_stale(const char *fn);

void pa_check_for_sigpipe(void);

char *pa_sprintf_malloc(const char *format, ...) __attribute__ ((format (printf, 1, 2)));

#endif
