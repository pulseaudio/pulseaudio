#include <string.h>
#include <errno.h>
#include <stdio.h>
#include <assert.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "module.h"
#include "socket-server.h"
#include "util.h"

#ifdef USE_PROTOCOL_SIMPLE
  #include "protocol-simple.h"
  #define protocol_free pa_protocol_simple_free
  #define IPV4_PORT 4711
  #define UNIX_SOCKET_DIR "/tmp/polypaudio"
  #define UNIX_SOCKET "/tmp/polypaudio/simple"
#else
  #ifdef USE_PROTOCOL_CLI
    #include "protocol-cli.h" 
    #define protocol_new pa_protocol_cli_new
    #define protocol_free pa_protocol_cli_free
    #define IPV4_PORT 4712
    #define UNIX_SOCKET_DIR "/tmp/polypaudio"
    #define UNIX_SOCKET "/tmp/polypaudio/cli"
  #else
    #ifdef USE_PROTOCOL_NATIVE
      #include "protocol-native.h"
      #define protocol_new pa_protocol_native_new
      #define protocol_free pa_protocol_native_free
      #define IPV4_PORT 4713
      #define UNIX_SOCKET_DIR "/tmp/polypaudio"
      #define UNIX_SOCKET "/tmp/polypaudio/native"
    #else
      #ifdef USE_PROTOCOL_ESOUND
        #include "protocol-esound.h"
        #include "esound-spec.h"
        #define protocol_new pa_protocol_esound_new
        #define protocol_free pa_protocol_esound_free
        #define IPV4_PORT ESD_DEFAULT_PORT
        #define UNIX_SOCKET_DIR ESD_UNIX_SOCKET_DIR
        #define UNIX_SOCKET ESD_UNIX_SOCKET_NAME
      #else
        #error "Broken build system" 
      #endif
    #endif 
  #endif
#endif

int pa_module_init(struct pa_core *c, struct pa_module*m) {
    struct pa_socket_server *s;
    assert(c && m);

#ifdef USE_TCP_SOCKETS
    if (!(s = pa_socket_server_new_ipv4(c->mainloop, INADDR_ANY, IPV4_PORT)))
        return -1;
#else
    if (pa_make_secure_dir(UNIX_SOCKET_DIR) < 0) {
        fprintf(stderr, "Failed to create secure socket directory.\n");
        return -1;
    }

    {
        int r;
        if ((r = pa_unix_socket_remove_stale(UNIX_SOCKET)) < 0) {
            fprintf(stderr, "Failed to remove stale UNIX socket '%s': %s\n", UNIX_SOCKET, strerror(errno));
            return -1;
        }

        if (r)
            fprintf(stderr, "Removed stale UNIX socket '%s'.", UNIX_SOCKET);
    }
    
    if (!(s = pa_socket_server_new_unix(c->mainloop, UNIX_SOCKET))) {
        rmdir(UNIX_SOCKET_DIR);
        return -1;
    }
#endif

#ifdef USE_PROTOCOL_SIMPLE
    m->userdata = pa_protocol_simple_new(c, s, m, PA_PROTOCOL_SIMPLE_PLAYBACK);
#else
    m->userdata = protocol_new(c, s, m);
#endif

    if (!m->userdata) {
        pa_socket_server_free(s);
        return -1;
    }

    return 0;
}

void pa_module_done(struct pa_core *c, struct pa_module*m) {
    assert(c && m);

    protocol_free(m->userdata);

#ifndef USE_TCP_SOCKETS
    rmdir(UNIX_SOCKET_DIR);
#endif
}
