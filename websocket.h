#ifndef WEBSOCKET_H
#define WEBSOCKET_H

#include <netinet/in.h>
#include <stdbool.h>

void ws_open(char const *host, in_port_t port);
int ws_fileno(void);
bool ws_isalive(void);
void ws_read(void);
int ws_write(char const *msg, size_t msglen);

extern void on_ws_message(char *msg, uint64_t msglen);
extern void on_ws_close(void);
extern void on_ws_open(void);

#endif
