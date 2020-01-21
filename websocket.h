#ifndef WEBSOCKET_H
#define WEBSOCKET_H
#include <netinet/in.h>

extern char const* ws_host;
extern in_port_t ws_port;
extern int ws_fd;

int ws_connect(void);
int ws_write(char const *msg, size_t msglen);

int ws_shutdown(void);

int ws_read(void);

int on_ws_message(char *msg, size_t msglen);
#endif
