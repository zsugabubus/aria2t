#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <netinet/in.h>

void ws_connect(char const *host, in_port_t port);
int ws_fileno(void);
int ws_isalive(void);
void ws_read(void);
void ws_write(char const *msg, size_t msglen);

extern void on_ws_message(char *msg, uint64_t msglen);
extern void on_ws_close(void);
extern void on_ws_open(void);

#endif
