#include <assert.h>
#include <arpa/inet.h>

#include "module.h"
#include "socket-server.h"

#ifdef USE_PROTOCOL_SIMPLE
  #include "protocol-simple.h"
  #define protocol_free protocol_simple_free
  #define IPV4_PORT 4711
#else
  #ifdef USE_PROTOCOL_CLI
    #include "protocol-cli.h" 
    #define protocol_new protocol_cli_new
    #define protocol_free protocol_cli_free
    #define IPV4_PORT 4712
  #else
    #ifdef USE_PROTOCOL_NATIVE
      #include "protocol-native.h"
      #define protocol_new protocol_native_new
      #define protocol_free protocol_native_free
      #define IPV4_PORT 4713
    #else
      #error "Broken build system"
    #endif
  #endif
#endif

int module_init(struct core *c, struct module*m) {
    struct socket_server *s;
    assert(c && m);

#ifdef USE_TCP_SOCKETS
    if (!(s = socket_server_new_ipv4(c->mainloop, INADDR_LOOPBACK, IPV4_PORT)))
        return -1;
#else
    if (!(s = socket_server_new_unix(c->mainloop, "/tmp/polypsimple")))
        return -1;
#endif

#ifdef USE_PROTOCOL_SIMPLE
    m->userdata = protocol_simple_new(c, s, PROTOCOL_SIMPLE_PLAYBACK);
#else
    m->userdata = protocol_new(c, s);
#endif
    
    assert(m->userdata);
    return 0;
}

void module_done(struct core *c, struct module*m) {
    assert(c && m);

    protocol_free(m->userdata);
}
