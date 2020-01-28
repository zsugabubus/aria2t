#ifndef WEBSOCKET_H
#define WEBSOCKET_H
#include <netinet/in.h>

extern int ws_fd;

int ws_connect(char const *host, in_port_t port);
int ws_shutdown(void);

int ws_read(void);
int ws_write(char const *msg, size_t msglen);

int on_ws_message(char *msg, size_t msglen);

#endif
