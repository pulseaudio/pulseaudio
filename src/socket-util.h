#ifndef foosocketutilhfoo
#define foosocketutilhfoo

#include <sys/types.h>

void pa_socket_peer_to_string(int fd, char *c, size_t l);

int pa_socket_low_delay(int fd);
int pa_socket_tcp_low_delay(int fd);

int pa_socket_set_sndbuf(int fd, size_t l);
int pa_socket_set_rcvbuf(int fd, size_t l);

int pa_unix_socket_is_stale(const char *fn);
int pa_unix_socket_remove_stale(const char *fn);

#endif
