#ifndef fooutilhfoo
#define fooutilhfoo

#include <sys/types.h>

void pa_make_nonblock_fd(int fd);

void pa_peer_to_string(char *c, size_t l, int fd);

int pa_make_secure_dir(const char* dir);

int pa_make_socket_low_delay(int fd);
int pa_make_tcp_socket_low_delay(int fd);

#endif
